#!/usr/bin/python

# Copyright (C) International Business Machines Corp., 2005
# Author: Murillo F. Bernardes <mfb@br.ibm.com>

import sys
import re
import time

from XmTestLib import *

if ENABLE_HVM_SUPPORT:
    SKIP("Block-attach not supported for HVM domains")

# Create a domain (default XmTestDomain, with our ramdisk)
domain = XmTestDomain()

try:
    console = domain.start()
except DomainError, e:
    if verbose:
        print "Failed to create test domain because:"
        print e.extra
    FAIL(str(e))

try:
    console.setHistorySaveCmds(value=True)
    # Run 'ls'
    run = console.runCmd("ls")
except ConsoleError, e:
    saveLog(console.getHistory())
    FAIL(str(e))

status, output = traceCommand("xm block-attach %s file:/dev/NOT-EXIST sdb1 w" % domain.getName())
eyecatcher = "Error"
where = output.find(eyecatcher)
if status == 0:
	FAIL("xm block-attach returned bad status, expected non 0, status is: %i" % status )
elif where == -1:
	FAIL("xm block-attach returned bad output, expected Error, output is: %s" % output )
	
try:
	run = console.runCmd("cat /proc/partitions")
except ConsoleError, e:
	FAIL(str(e))

# Close the console
domain.closeConsole()

# Stop the domain (nice shutdown)
domain.stop()

if re.search("sdb1",run["output"]):
	FAIL("Non existent Device was connected to the domU")
