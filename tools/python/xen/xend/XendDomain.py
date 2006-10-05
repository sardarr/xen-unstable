#============================================================================
# This library is free software; you can redistribute it and/or
# modify it under the terms of version 2.1 of the GNU Lesser General Public
# License as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#============================================================================
# Copyright (C) 2004, 2005 Mike Wray <mike.wray@hp.com>
# Copyright (C) 2005 Christian Limpach <Christian.Limpach@cl.cam.ac.uk>
# Copyright (C) 2005 XenSource Ltd
#============================================================================

"""Handler for domain operations.
 Nothing here is persistent (across reboots).
 Needs to be persistent for one uptime.
"""

import os
import shutil
import socket
import threading

import xen.lowlevel.xc


from xen.xend import XendRoot, XendCheckpoint, XendDomainInfo
from xen.xend.PrettyPrint import prettyprint
from xen.xend.XendConfig import XendConfig
from xen.xend.XendError import XendError, XendInvalidDomain
from xen.xend.XendLogging import log
from xen.xend.XendConstants import XS_VMROOT

from xen.xend.xenstore.xstransact import xstransact
from xen.xend.xenstore.xswatch import xswatch
from xen.util import security
from xen.xend import uuid

xc = xen.lowlevel.xc.xc()
xroot = XendRoot.instance() 

__all__ = [ "XendDomain" ]

CACHED_CONFIG_FILE = 'config.sxp'
CHECK_POINT_FILE = 'checkpoint.chk'
DOM0_UUID = "00000000-0000-0000-0000-000000000000"
DOM0_NAME = "Domain-0"
DOM0_ID   = 0

class XendDomain:
    """Index of all domains. Singleton.

    @ivar domains: map of domains indexed by UUID Strings
    @type domains: dict of XendDomainInfo
    @ivar domains_managed: uuid of domains that are managed by Xend
    @type managed_domains: list of (uuids, dom_name)
    @ivar domains_lock: lock that must be held when manipulating self.domains
    @type domains_lock: threaading.RLock
    @ivar _allow_new_domains: Flag to set that allows creating of new domains.
    @type _allow_new_domains: boolean
    
    """

    def __init__(self):
        self.domains = {}
        self.managed_domains = []
        self.domains_lock = threading.RLock()

        # xen api instance vars
        # TODO: nothing uses this at the moment
        self._allow_new_domains = True

    # This must be called only the once, by instance() below.  It is separate
    # from the constructor because XendDomainInfo calls back into this class
    # in order to check the uniqueness of domain names.  This means that
    # instance() must be able to return a valid instance of this class even
    # during this initialisation.
    def init(self):
        """Singleton initialisation function."""

        xstransact.Mkdir(XS_VMROOT)
        xstransact.SetPermissions(XS_VMROOT, {'dom': DOM0_ID})

        self.domains_lock.acquire()
        try:
            try:
                dom0info = [d for d in self._running_domains() \
                            if d['domid'] == DOM0_ID][0]
                
                dom0 = XendDomainInfo.recreate(dom0info, True)
                # Sometimes this is not set?
                dom0.setName(DOM0_NAME)
                self._add_domain(dom0)
            except IndexError:
                raise XendError('Unable to find Domain 0')
            
            self._setDom0CPUCount()

            # This watch registration needs to be before the refresh call, so
            # that we're sure that we haven't missed any releases, but inside
            # the domains_lock, as we don't want the watch to fire until after
            # the refresh call has completed.
            xswatch("@introduceDomain", self._on_domains_changed)
            xswatch("@releaseDomain",   self._on_domains_changed)

            self._init_domains()
        finally:
            self.domains_lock.release()

    
    def _on_domains_changed(self, _):
        """ Callback method when xenstore changes.

        Calls refresh which will keep the local cache of domains
        in sync.

        @rtype: int
        @return: 1
        """
        self.domains_lock.acquire()
        try:
            self._refresh()
        finally:
            self.domains_lock.release()
        return 1

    def _init_domains(self):
        """Does the initial scan of managed and active domains to
        populate self.domains.

        Note: L{XendDomainInfo._checkName} will call back into XendDomain
        to make sure domain name is not a duplicate.

        """
        self.domains_lock.acquire()
        try:
            running = self._running_domains()
            managed = self._managed_domains()

            # add all active domains, replacing managed ones
            for dom in running:
                if dom['domid'] != DOM0_ID:
                    try:
                        new_dom = XendDomainInfo.recreate(dom, False)
                        self._add_domain(new_dom)
                    except Exception:
                        log.exception("Failed to create reference to running "
                                      "domain id: %d" % dom['domid'])

            # add all managed domains as dormant domains.
            for dom in managed:
                dom_uuid = dom.get('uuid', uuid.createString())
                dom['uuid'] = dom_uuid
                dom_name = dom.get('name', 'Domain-%s' % dom_uuid)
                
                try:
                    # instantiate domain if not started.
                    if not self.domain_lookup_nr(dom_name):
                        new_dom = XendDomainInfo.createDormant(dom)
                        self._add_domain(new_dom)
                    # keep track of maanged domains
                    self._managed_domain_register(new_dom)
                except Exception:
                    log.exception("Failed to create reference to managed "
                                  "domain: %s" % dom_name)

        finally:
            self.domains_lock.release()


    # -----------------------------------------------------------------
    # Getting managed domains storage path names

    def _managed_path(self, domuuid = None):
        """Returns the path of the directory where managed domain
        information is stored.

        @keyword domuuid: If not None, will return the path to the domain
                          otherwise, will return the path containing
                          the directories which represent each domain.
        @type: None or String.
        @rtype: String
        @return: Path.
        """
        dom_path = xroot.get_xend_domains_path()
        if domuuid:
            dom_path = os.path.join(dom_path, domuuid)
        return dom_path

    def _managed_config_path(self, domuuid):
        """Returns the path to the configuration file of a managed domain.

        @param domname: Domain uuid
        @type domname: String
        @rtype: String
        @return: path to config file.
        """
        return os.path.join(self._managed_path(domuuid), CACHED_CONFIG_FILE)

    def _managed_check_point_path(self, domuuid):
        """Returns absolute path to check point file for managed domain.
        
        @param domuuid: Name of managed domain
        @type domname: String
        @rtype: String
        @return: Path
        """
        return os.path.join(self._managed_path(domuuid), CHECK_POINT_FILE)

    def _managed_config_remove(self, domuuid):
        """Removes a domain configuration from managed list

        @param domuuid: Name of managed domain
        @type domname: String
        @raise XendError: fails to remove the domain.
        """
        config_path = self._managed_path(domuuid)
        try:
            if os.path.exists(config_path) and os.path.isdir(config_path):
                shutil.rmtree(config_path)
        except IOError:
            log.exception('managed_config_remove failed removing conf')
            raise XendError("Unable to remove managed configuration"
                            " for domain: %s" % domuuid)            

    def managed_config_save(self, dominfo):
        """Save a domain's configuration to disk
        
        @param domninfo: Managed domain to save.
        @type dominfo: XendDomainInfo
        @raise XendError: fails to save configuration.
        @rtype: None
        """
        if dominfo:
            domains_dir = self._managed_path()
            dom_uuid = dominfo.get_uuid()            
            domain_config_dir = self._managed_path(dom_uuid)
        
            # make sure the domain dir exists
            if not os.path.exists(domains_dir):
                os.makedirs(domains_dir, 0755)
            elif not os.path.isdir(domains_dir):
                log.error("xend_domain_dir is not a directory.")
                raise XendError("Unable to save managed configuration "
                                "because %s is not a directory." %
                                domains_dir)
            
            if not os.path.exists(domain_config_dir):
                try:
                    os.makedirs(domain_config_dir, 0755)
                except IOError:
                    log.exception("Failed to create directory: %s" %
                                  domain_config_dir)
                    raise XendError("Failed to create directory: %s" %
                                    domain_config_dir)
                
            try:
                sxp_cache_file = open(self._managed_config_path(dom_uuid),'w')
                prettyprint(dominfo.sxpr(), sxp_cache_file, width = 78)
                sxp_cache_file.close()
            except IOError:
                log.error("Error occurred saving configuration file to %s" %
                          domain_config_dir)
                raise XendError("Failed to save configuration file to: %s" %
                                domain_config_dir)
        else:
            log.warn("Trying to save configuration for invalid domain")


    def _managed_domains(self):
        """ Returns list of domains that are managed.
        
        Expects to be protected by domains_lock.

        @rtype: list of XendConfig
        @return: List of domain configurations that are managed.
        """
        dom_path = self._managed_path()
        dom_uuids = os.listdir(dom_path)
        doms = []
        for dom_uuid in dom_uuids:
            try:
                cfg_file = self._managed_config_path(dom_uuid)
                cfg = XendConfig(filename = cfg_file)
                doms.append(cfg)
            except Exception:
                log.exception('Unable to open or parse config.sxp: %s' % \
                              cfg_file)
        return doms

    def _managed_domain_unregister(self, dom):
        try:
            self.managed_domains.remove((dom.get_uuid(), dom.getName()))
        except ValueError:
            log.warn("Domain is not registered: %s" % dom.get_uuid())

    def _managed_domain_register(self, dom):
        self.managed_domains.append((dom.get_uuid(), dom.getName()))

    def _managed_domain_rename(self, dom, new_name):
        for i in range(len(self.managed_domains)):
            if self.managed_domains[i][0] == dom.get_uuid():
                self.managed_domains[i][1] = new_name
                return True
        return False

    def is_domain_managed(self, dom = None, dom_name = None):
        dom_uuid = dom.get_uuid()
        dom_name = dom.getName()
        if dom:
            return ((dom_uuid, dom_name) in self.managed_domains)
        if dom_name:
            results = [d for d in self.managed_domains if d[1] == dom_name]
            return (len(results) > 0)
        return False

    

    # End of Managed Domain Access
    # --------------------------------------------------------------------

    def _running_domains(self):
        """Get table of domains indexed by id from xc.

        @requires: Expects to be protected by domains_lock.
        @rtype: list of dicts
        @return: A list of dicts representing the running domains.
        """
        return xc.domain_getinfo()

    def _setDom0CPUCount(self):
        """Sets the number of VCPUs dom0 has. Retreived from the
        Xend configuration, L{XendRoot}.

        @requires: Expects to be protected by domains_lock.
        @rtype: None
        """
        dom0 = self.privilegedDomain()

        # get max number of vcpus to use for dom0 from config
        target = int(xroot.get_dom0_vcpus())
        log.debug("number of vcpus to use is %d", target)
   
        # target == 0 means use all processors
        if target > 0:
            dom0.setVCpuCount(target)


    def _refresh(self):
        """Refresh the domain list. Needs to be called when
        either xenstore has changed or when a method requires
        up to date information (like uptime, cputime stats).

        @rtype: None
        """
        self.domains_lock.acquire()
        try:
            # update information for all running domains
            # - like cpu_time, status, dying, etc.
            running = self._running_domains()
            for dom in running:
                dom_info = self.domain_lookup_nr(dom['domid'])
                if dom_info:
                    dom_info.update(dom)

            # clean up unmanaged domains
            for dom in self.domains.values():
                if (dom.getDomid() == None) and \
                       not self.is_domain_managed(dom):
                    self._remove_domain(dom)
                    
        finally:
            self.domains_lock.release()

    def _add_domain(self, info, managed = False):
        """Add the given domain entry to this instance's internal cache.
        
        @requires: Expects to be protected by the domains_lock.
        @param info: XendDomainInfo of a domain to be added.
        @type info: XendDomainInfo
        @keyword managed: Whether this domain is maanged by Xend
        @type managed: boolean
        """
        log.debug("Adding Domain: %s" % info.get_uuid())
        self.domains[info.get_uuid()] = info
        if managed and not self.is_domain_managed(info):
            self._managed_domain_register(info)

    def _remove_domain(self, info):
        """Remove the given domain from this instance's internal cache.
        
        @requires: Expects to be protected by the domains_lock.
        @param info: XendDomainInfo of a domain to be removed.
        @type info: XendDomainInfo
        """
        if info:
            dom_name = info.getName()
            dom_uuid = info.get_uuid()
            
            if info.state != XendDomainInfo.DOM_STATE_HALTED:
                info.cleanupDomain()

            if self.is_domain_managed(info):
                self._managed_config_remove(dom_uuid)
                self._managed_domain_unregister(info)
                
            try:
                del self.domains[dom_uuid]
            except KeyError:
                pass
        else:
            log.warning("Attempted to remove non-existent domain.")

    def restore_(self, config):
        """Create a domain as part of the restore process.  This is called
        only from L{XendCheckpoint}.

        A restore request comes into XendDomain through L{domain_restore}
        or L{domain_restore_fd}.  That request is
        forwarded immediately to XendCheckpoint which, when it is ready, will
        call this method.  It is necessary to come through here rather than go
        directly to L{XendDomainInfo.restore} because we need to
        serialise the domain creation process, but cannot lock
        domain_restore_fd as a whole, otherwise we will deadlock waiting for
        the old domain to die.

        @param config: Configuration of domain to restore
        @type config: SXP Object (eg. list of lists)
        """
        self.domains_lock.acquire()
        try:
            security.refresh_ssidref(config)
            dominfo = XendDomainInfo.restore(config)
            self._add_domain(dominfo)
            return dominfo
        finally:
            self.domains_lock.release()


    def domain_lookup(self, domid):
        """Look up given I{domid} in the list of managed and running
        domains.
        
        @note: Will cause a refresh before lookup up domains, for
               a version that does not need to re-read xenstore
               use L{domain_lookup_nr}.

        @param domid: Domain ID or Domain Name.
        @type domid: int or string
        @return: Found domain.
        @rtype: XendDomainInfo
        @raise XendError: If domain is not found.
        """
        self.domains_lock.acquire()
        try:
            self._refresh()
            dom = self.domain_lookup_nr(domid)
            if not dom:
                raise XendError("No domain named '%s'." % str(domid))
            return dom
        finally:
            self.domains_lock.release()


    def domain_lookup_nr(self, domid):
        """Look up given I{domid} in the list of managed and running
        domains.

        @param domid: Domain ID or Domain Name.
        @type domid: int or string
        @return: Found domain.
        @rtype: XendDomainInfo or None
        """
        self.domains_lock.acquire()
        try:
            # lookup by name
            match = [dom for dom in self.domains.values() \
                     if dom.getName() == domid]
            if match:
                return match[0]

            # lookup by id
            try:
                match = [d for d in self.domains.values() \
                       if d.getDomid() == int(domid)]
                if match:
                    return match[0]
            except (ValueError, TypeError):
                pass

            return None
        finally:
            self.domains_lock.release()

    def privilegedDomain(self):
        """ Get the XendDomainInfo of a dom0

        @rtype: XendDomainInfo
        """
        self.domains_lock.acquire()
        try:
            return self.domains[DOM0_UUID]
        finally:
            self.domains_lock.release()

    def cleanup_domains(self):
        """Clean up domains that are marked as autostop.
        Should be called when Xend goes down. This is currently
        called from L{xen.xend.servers.XMLRPCServer}.

        """
        log.debug('cleanup_domains')
        self.domains_lock.acquire()
        try:
            for dom in self.domains.values():
                if dom.getName() == DOM0_NAME:
                    continue
                
                if dom.state == XendDomainInfo.DOM_STATE_RUNNING:
                    shutdownAction = dom.info.get('on_xend_stop', 'ignore')
                    if shouldShutdown and shutdownAction == 'shutdown':
                        log.debug('Shutting down domain: %s' % dom.getName())
                        dom.shutdown("poweroff")
                    elif shouldShutdown and shutdownAction == 'suspend':
                        chkfile = self._managed_check_point_path(dom.getName())
                        self.domain_save(dom.domid, chkfile)
        finally:
            self.domains_lock.release()



    # ----------------------------------------------------------------
    # Xen API 
    

    def set_allow_new_domains(self, allow_new_domains):
        self._allow_new_domains = allow_new_domains

    def allow_new_domains(self):
        return self._allow_new_domains

    def get_domain_refs(self):
        result = []
        try:
            self.domains_lock.acquire()
            result = [d.get_uuid() for d in self.domains.values()]
        finally:
            self.domains_lock.release()
        return result

    def get_vm_by_uuid(self, vm_uuid):
        self.domains_lock.acquire()
        try:
            if vm_uuid in self.domains:
                return self.domains[vm_uuid]
            return None
        finally:
            self.domains_lock.release()

    def get_dev_by_uuid(self, klass, dev_uuid, field):
        parts = dev_uuid.split('-%s-' % klass, 1)
        try:
            if len(parts) > 1:
                dom = self.get_vm_by_uuid(parts[0])
                if not dom:
                    return None
                
                if field == 'VM':
                    return dom.get_uuid()
                if field == 'uuid':
                    return dev_uuid

                devid = int(parts[1])
                value = dom.get_device_property(klass, devid, field)
                return value
        except ValueError, e:
            pass
        
        return None

    def is_valid_vm(self, vm_ref):
        return (self.get_vm_by_uuid(vm_ref) != None)

    def is_valid_dev(self, klass, dev_uuid):
        parts = dev_uuid.split('-%s-' % klass, 1)
        try:
            if len(parts) > 1:
                dom = self.get_vm_by_uuid(parts[0])
                if not dom:
                    return False
                devid = int(parts[1])
                return dom.isDeviceValid(klass, devid)
        except ValueError, e:
            pass
            
        return False

    def do_legacy_api_with_uuid(self, fn, vm_uuid, *args):
        self.domains_lock.acquire()
        try:
            if vm_uuid in self.domains:
                # problem is domid does not exist for unstarted
                # domains, so in that case, we use the name.
                # TODO: probably want to modify domain_lookup_nr
                #       to lookup uuids, or just ignore
                #       the legacy api and reimplement all these
                #       calls.
                domid = self.domains[vm_uuid].getDomid()
                if domid == None:
                    domid = self.domains[vm_uuid].getName()
                return fn(domid, *args)
            raise XendInvalidDomain("Domain does not exist")
        finally:
            self.domains_lock.release()
        

    def create_domain(self, xenapi_vm):
        self.domains_lock.acquire()
        try:
            try:
                xeninfo = XendConfig(xenapi_vm = xenapi_vm)
                dominfo = XendDomainInfo.createDormant(xeninfo)
                log.debug("Creating new managed domain: %s: %s" %
                          (dominfo.getName(), dominfo.get_uuid()))
                self._add_domain(dominfo, managed = True)
                self.managed_config_save(dominfo)
                return dominfo.get_uuid()
            except XendError, e:
                raise
            except Exception, e:
                raise XendError(str(e))
        finally:
            self.domains_lock.release()        

    def rename_domain(self, dom, new_name):
        self.domains_lock.acquire()
        try:
            old_name = dom.getName()
            dom.setName(new_name)

            if self.is_domain_managed(dom):
                self._managed_domain_rename(dom, new_name)

        finally:
            self.domains_lock.release()
                
    
    #
    # End of Xen API 
    # ----------------------------------------------------------------

    # ------------------------------------------------------------
    # Xen Legacy API     

    def list(self):
        """Get list of domain objects.

        @return: domains
        @rtype: list of XendDomainInfo
        """
        self.domains_lock.acquire()
        try:
            self._refresh()
            return self.domains.values()
        finally:
            self.domains_lock.release()


    def list_sorted(self):
        """Get list of domain objects, sorted by name.

        @return: domain objects
        @rtype: list of XendDomainInfo
        """
        doms = self.list()
        doms.sort(lambda x, y: cmp(x.getName(), y.getName()))
        return doms

    def list_names(self):
        """Get list of domain names.

        @return: domain names
        @rtype: list of strings.
        """
        return [d.getName() for d in self.list_sorted()]

    def domain_suspend(self, domname):
        """Suspends a domain that is persistently managed by Xend

        @param domname: Domain Name
        @type domname: string
        @rtype: None
        @raise XendError: Failure during checkpointing.
        """

        try:
            dominfo = self.domain_lookup_nr(domname)
            if not dominfo:
                raise XendInvalidDomain(domname)

            if dominfo.getDomid() == DOM0_ID:
                raise XendError("Cannot save privileged domain %s" % domname)

            if dominfo.state != XendDomainInfo.DOM_STATE_RUNNING:
                raise XendError("Cannot suspend domain that is not running.")

            if not os.path.exists(self._managed_config_path(domname)):
                raise XendError("Domain is not managed by Xend lifecycle " +
                                "support.")
            
            path = self._managed_check_point_path(domname)
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
            try:
                # For now we don't support 'live checkpoint' 
                XendCheckpoint.save(fd, dominfo, False, False, path)
            finally:
                os.close(fd)
        except OSError, ex:
            raise XendError("can't write guest state file %s: %s" %
                            (path, ex[1]))

    def domain_resume(self, domname):
        """Resumes a domain that is persistently managed by Xend.

        @param domname: Domain Name
        @type domname: string
        @rtype: None
        @raise XendError: If failed to restore.
        """
        try:
            dominfo = self.domain_lookup_nr(domname)
            
            if not dominfo:
                raise XendInvalidDomain(domname)

            if dominfo.getDomid() == DOM0_ID:
                raise XendError("Cannot save privileged domain %s" % domname)

            if dominfo.state != XendDomainInfo.DOM_STATE_HALTED:
                raise XendError("Cannot suspend domain that is not running.")

            chkpath = self._managed_check_point_path(domname)
            if not os.path.exists(chkpath):
                raise XendError("Domain was not suspended by Xend")

            # Restore that replaces the existing XendDomainInfo
            try:
                log.debug('Current DomainInfo state: %d' % dominfo.state)
                XendCheckpoint.restore(self,
                                       os.open(chkpath, os.O_RDONLY),
                                       dominfo)
                os.unlink(chkpath)
            except OSError, ex:
                raise XendError("Failed to read stored checkpoint file")
            except IOError, ex:
                raise XendError("Failed to delete checkpoint file")
        except Exception, ex:
            log.exception("Exception occurred when resuming")
            raise XendError("Error occurred when resuming: %s" % str(ex))


    def domain_create(self, config):
        """Create a domain from a configuration.

        @param config: configuration
        @type config: SXP Object (list of lists)
        @rtype: XendDomainInfo
        """
        self.domains_lock.acquire()
        try:
            dominfo = XendDomainInfo.create(config)
            self._add_domain(dominfo)
            return dominfo
        finally:
            self.domains_lock.release()


    def domain_new(self, config):
        """Create a domain from a configuration but do not start it.
        
        @param config: configuration
        @type config: SXP Object (list of lists)
        @rtype: XendDomainInfo
        """
        self.domains_lock.acquire()
        try:
            try:
                xeninfo = XendConfig(sxp = config)
                dominfo = XendDomainInfo.createDormant(xeninfo)
                log.debug("Creating new managed domain: %s" %
                          dominfo.getName())
                self._add_domain(dominfo, managed = True)
                self.managed_config_save(dominfo)
                # no return value because it isn't meaningful for client
            except XendError, e:
                raise
            except Exception, e:
                raise XendError(str(e))
        finally:
            self.domains_lock.release()

    def domain_start(self, domid):
        """Start a managed domain

        @require: Domain must not be running.
        @param domid: Domain name or domain ID.
        @type domid: string or int
        @rtype: None
        @raise XendError: If domain is still running
        @rtype: None
        """
        self.domains_lock.acquire()
        try:
            dominfo = self.domain_lookup_nr(domid)
            if not dominfo:
                raise XendInvalidDomain(str(domid))

            if dominfo.state != XendDomainInfo.DOM_STATE_HALTED:
                raise XendError("Domain is already running")
            
            dominfo.start(is_managed = True)

            
        finally:
            self.domains_lock.release()
        

    def domain_delete(self, domid):
        """Remove a domain from database

        @require: Domain must not be running.
        @param domid: Domain name or domain ID.
        @type domid: string or int
        @rtype: None
        @raise XendError: If domain is still running
        """
        self.domains_lock.acquire()
        try:
            try:
                dominfo = self.domain_lookup_nr(domid)
                if not dominfo:
                    raise XendInvalidDomain(str(domid))

                if dominfo.state != XendDomainInfo.DOM_STATE_HALTED:
                    raise XendError("Domain is still running")

                self._remove_domain(dominfo)
                
            except Exception, ex:
                raise XendError(str(ex))
        finally:
            self.domains_lock.release()
        

    def domain_configure(self, config):
        """Configure an existing domain.

        @param vmconfig: vm configuration
        @type vmconfig: SXP Object (list of lists)
        @todo: Not implemented
        """
        # !!!
        raise XendError("Unsupported")

    def domain_restore(self, src):
        """Restore a domain from file.

        @param src: filename of checkpoint file to restore from
        @type src: string
        @return: Restored domain
        @rtype: XendDomainInfo
        @raise XendError: Failure to restore domain
        """
        try:
            fd = os.open(src, os.O_RDONLY)
            try:
                return self.domain_restore_fd(fd)
            finally:
                os.close(fd)
        except OSError, ex:
            raise XendError("can't read guest state file %s: %s" %
                            (src, ex[1]))

    def domain_restore_fd(self, fd):
        """Restore a domain from the given file descriptor.

        @param fd: file descriptor of the checkpoint file
        @type fd: File object
        @rtype: XendDomainInfo
        @raise XendError: if failed to restore
        """

        try:
            return XendCheckpoint.restore(self, fd)
        except:
            # I don't really want to log this exception here, but the error
            # handling in the relocation-socket handling code (relocate.py) is
            # poor, so we need to log this for debugging.
            log.exception("Restore failed")
            raise XendError("Restore failed")
 
    def domain_unpause(self, domid):
        """Unpause domain execution.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @rtype: None
        @raise XendError: Failed to unpause
        @raise XendInvalidDomain: Domain is not valid        
        """
        try:
            dominfo = self.domain_lookup_nr(domid)
            if not dominfo:
                raise XendInvalidDomain(str(domid))
            
            log.info("Domain %s (%d) unpaused.", dominfo.getName(),
                     int(dominfo.getDomid()))
            
            dominfo.unpause()
        except XendInvalidDomain:
            log.exception("domain_unpause")
            raise
        except Exception, ex:
            log.exception("domain_unpause")
            raise XendError(str(ex))

    def domain_pause(self, domid):
        """Pause domain execution.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @rtype: None
        @raise XendError: Failed to pause
        @raise XendInvalidDomain: Domain is not valid
        """        
        try:
            dominfo = self.domain_lookup_nr(domid)
            if not dominfo:
                raise XendInvalidDomain(str(domid))
            log.info("Domain %s (%d) paused.", dominfo.getName(),
                     int(dominfo.getDomid()))
            dominfo.pause()
        except XendInvalidDomain:
            log.exception("domain_pause")
            raise
        except Exception, ex:
            log.exception("domain_pause")
            raise XendError(str(ex))

    def domain_dump(self, domid, filename, live, crash):
        """Dump domain core."""

        dominfo = self.domain_lookup_by_name_or_id_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))

        if dominfo.getDomid() == PRIV_DOMAIN:
            raise XendError("Cannot dump core for privileged domain %s" % domid)

        try:
            log.info("Domain core dump requested for domain %s (%d) live=%d crash=%d.",
                     dominfo.getName(), dominfo.getDomid(), live, crash)
            return dominfo.dumpCore(filename)
        except Exception, ex:
            raise XendError(str(ex))

    def domain_destroy(self, domid):
        """Terminate domain immediately.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @rtype: None
        @raise XendError: Failed to destroy
        @raise XendInvalidDomain: Domain is not valid        
        """

        dominfo = self.domain_lookup_nr(domid)
        if dominfo and dominfo.getDomid() == DOM0_ID:
            raise XendError("Cannot destroy privileged domain %s" % domid)

        if dominfo:
            val = dominfo.destroy()
        else:
            try:
                val = xc.domain_destroy(int(domid))
            except ValueError:
                raise XendInvalidDomain(domid)
            except Exception, e:
                raise XendError(str(e))

        return val       

    def domain_migrate(self, domid, dst, live=False, resource=0, port=0):
        """Start domain migration.
        
        @param domid: Domain ID or Name
        @type domid: int or string.
        @param dst: Destination IP address
        @type dst: string
        @keyword port: relocation port on destination
        @type port: int        
        @keyword live: Live migration
        @type live: bool
        @keyword resource: not used??
        @rtype: None
        @raise XendError: Failed to migrate
        @raise XendInvalidDomain: Domain is not valid        
        """

        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))

        if dominfo.getDomid() == DOM0_ID:
            raise XendError("Cannot migrate privileged domain %i" % domid)

        """ The following call may raise a XendError exception """
        dominfo.testMigrateDevices(True, dst)

        if live:
            """ Make sure there's memory free for enabling shadow mode """
            dominfo.checkLiveMigrateMemory()

        if port == 0:
            port = xroot.get_xend_relocation_port()
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect((dst, port))
        except socket.error, err:
            raise XendError("can't connect: %s" % err[1])

        sock.send("receive\n")
        sock.recv(80)
        XendCheckpoint.save(sock.fileno(), dominfo, True, live, dst)
        dominfo.testDeviceComplete()
        sock.close()

    def domain_save(self, domid, dst):
        """Start saving a domain to file.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @param dst: Destination filename
        @type dst: string
        @rtype: None
        @raise XendError: Failed to save domain
        @raise XendInvalidDomain: Domain is not valid        
        """
        try:
            dominfo = self.domain_lookup_nr(domid)
            if not dominfo:
                raise XendInvalidDomain(str(domid))

            if dominfo.getDomid() == DOM0_ID:
                raise XendError("Cannot save privileged domain %i" % domid)

            fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
            try:
                # For now we don't support 'live checkpoint' 
                XendCheckpoint.save(fd, dominfo, False, False, dst)
            finally:
                os.close(fd)
        except OSError, ex:
            raise XendError("can't write guest state file %s: %s" %
                            (dst, ex[1]))

    def domain_pincpu(self, domid, vcpu, cpumap):
        """Set which cpus vcpu can use

        @param domid: Domain ID or Name
        @type domid: int or string.
        @param vcpu: vcpu to pin to
        @type vcpu: int
        @param cpumap:  string repr of usable cpus
        @type cpumap: string
        @rtype: 0
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))

        # if vcpu is keyword 'all', apply the cpumap to all vcpus
        vcpus = [ vcpu ]
        if str(vcpu).lower() == "all":
            vcpus = range(0, int(dominfo.getVCpuCount()))
       
        # set the same cpumask for all vcpus
        rc = 0
        for v in vcpus:
            try:
                rc = xc.vcpu_setaffinity(dominfo.getDomid(), int(v), cpumap)
            except Exception, ex:
                raise XendError(str(ex))
        return rc

    def domain_cpu_sedf_set(self, domid, period, slice_, latency, extratime,
                            weight):
        """Set Simple EDF scheduler parameters for a domain.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @rtype: 0
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))
        try:
            return xc.sedf_domain_set(dominfo.getDomid(), period, slice_,
                                      latency, extratime, weight)
        except Exception, ex:
            raise XendError(str(ex))

    def domain_cpu_sedf_get(self, domid):
        """Get Simple EDF scheduler parameters for a domain.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @rtype: SXP object
        @return: The parameters for Simple EDF schedule for a domain.
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))
        try:
            sedf_info = xc.sedf_domain_get(dominfo.getDomid())
            # return sxpr
            return ['sedf',
                    ['domain',    sedf_info['domain']],
                    ['period',    sedf_info['period']],
                    ['slice',     sedf_info['slice']],
                    ['latency',   sedf_info['latency']],
                    ['extratime', sedf_info['extratime']],
                    ['weight',    sedf_info['weight']]]

        except Exception, ex:
            raise XendError(str(ex))

    def domain_shadow_control(self, domid, op):
        """Shadow page control.
        
        @param domid: Domain ID or Name
        @type domid: int or string.
        @param op: operation
        @type op: int
        @rtype: 0
        """
        dominfo = self.domain_lookup(domid)
        try:
            return xc.shadow_control(dominfo.getDomid(), op)
        except Exception, ex:
            raise XendError(str(ex))

    def domain_shadow_mem_get(self, domid):
        """Get shadow pagetable memory allocation.
        
        @param domid: Domain ID or Name
        @type domid: int or string.
        @rtype: int
        @return: shadow memory in MB
        """
        dominfo = self.domain_lookup(domid)
        try:
            return xc.shadow_mem_control(dominfo.getDomid())
        except Exception, ex:
            raise XendError(str(ex))

    def domain_shadow_mem_set(self, domid, mb):
        """Set shadow pagetable memory allocation.
        
        @param domid: Domain ID or Name
        @type domid: int or string.
        @param mb: shadow memory to set in MB
        @type: mb: int
        @rtype: int
        @return: shadow memory in MB
        """
        dominfo = self.domain_lookup(domid)
        try:
            return xc.shadow_mem_control(dominfo.getDomid(), mb=mb)
        except Exception, ex:
            raise XendError(str(ex))

    def domain_sched_credit_get(self, domid):
        """Get credit scheduler parameters for a domain.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @rtype: dict with keys 'weight' and 'cap'
        @return: credit scheduler parameters
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))
        try:
            return xc.sched_credit_domain_get(dominfo.getDomid())
        except Exception, ex:
            raise XendError(str(ex))
    
    def domain_sched_credit_set(self, domid, weight = None, cap = None):
        """Set credit scheduler parameters for a domain.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @type weight: int
        @type cap: int
        @rtype: 0
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))
        try:
            if weight is None:
                weight = int(0)
            elif weight < 1 or weight > 65535:
                raise XendError("weight is out of range")

            if cap is None:
                cap = int(~0)
            elif cap < 0 or cap > dominfo.getVCpuCount() * 100:
                raise XendError("cap is out of range")

            return xc.sched_credit_domain_set(dominfo.getDomid(), weight, cap)
        except Exception, ex:
            raise XendError(str(ex))

    def domain_maxmem_set(self, domid, mem):
        """Set the memory limit for a domain.

        @param domid: Domain ID or Name
        @type domid: int or string.
        @param mem: memory limit (in MiB)
        @type mem: int
        @raise XendError: fail to set memory
        @rtype: 0
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))
        maxmem = int(mem) * 1024
        try:
            return xc.domain_setmaxmem(dominfo.getDomid(), maxmem)
        except Exception, ex:
            raise XendError(str(ex))

    def domain_ioport_range_enable(self, domid, first, last):
        """Enable access to a range of IO ports for a domain

        @param first: first IO port
        @param last: last IO port
        @raise XendError: failed to set range
        @rtype: 0
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))
        nr_ports = last - first + 1
        try:
            return xc.domain_ioport_permission(dominfo.getDomid(),
                                               first_port = first,
                                               nr_ports = nr_ports,
                                               allow_access = 1)
        except Exception, ex:
            raise XendError(str(ex))

    def domain_ioport_range_disable(self, domid, first, last):
        """Disable access to a range of IO ports for a domain

        @param first: first IO port
        @param last: last IO port
        @raise XendError: failed to set range
        @rtype: 0
        """
        dominfo = self.domain_lookup_nr(domid)
        if not dominfo:
            raise XendInvalidDomain(str(domid))
        nr_ports = last - first + 1
        try:
            return xc.domain_ioport_permission(dominfo.getDomid(),
                                               first_port = first,
                                               nr_ports = nr_ports,
                                               allow_access = 0)
        except Exception, ex:
            raise XendError(str(ex))


def instance():
    """Singleton constructor. Use this instead of the class constructor.
    """
    global inst
    try:
        inst
    except:
        inst = XendDomain()
        inst.init()
    return inst
