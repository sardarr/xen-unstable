/******************************************************************************
 * evtchn.c
 * 
 * Driver for receiving and demuxing event-channel signals.
 * 
 * Copyright (c) 2004-2005, K A Fraser
 * Multi-process extensions Copyright (c) 2004, Steven Smith
 * 
 * This file may be distributed separately from the Linux kernel, or
 * incorporated into other software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/major.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/poll.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <asm-xen/evtchn.h>
#include <asm-xen/linux-public/evtchn.h>

struct per_user_data {
	/* Notification ring, accessed via /dev/xen/evtchn. */
#define EVTCHN_RING_SIZE     (PAGE_SIZE / sizeof(evtchn_port_t))
#define EVTCHN_RING_MASK(_i) ((_i)&(EVTCHN_RING_SIZE-1))
	evtchn_port_t *ring;
	unsigned int ring_cons, ring_prod, ring_overflow;

	/* Processes wait on this queue when ring is empty. */
	wait_queue_head_t evtchn_wait;
	struct fasync_struct *evtchn_async_queue;
};

/* Who's bound to each port? */
static struct per_user_data *port_user[NR_EVENT_CHANNELS];
static spinlock_t port_user_lock;

void evtchn_device_upcall(int port)
{
	struct per_user_data *u;

	spin_lock(&port_user_lock);

	mask_evtchn(port);
	clear_evtchn(port);

	if ((u = port_user[port]) != NULL) {
		if ((u->ring_prod - u->ring_cons) < EVTCHN_RING_SIZE) {
			u->ring[EVTCHN_RING_MASK(u->ring_prod)] = port;
			if (u->ring_cons == u->ring_prod++) {
				wake_up_interruptible(&u->evtchn_wait);
				kill_fasync(&u->evtchn_async_queue,
					    SIGIO, POLL_IN);
			}
		} else {
			u->ring_overflow = 1;
		}
	}

	spin_unlock(&port_user_lock);
}

static ssize_t evtchn_read(struct file *file, char __user *buf,
                           size_t count, loff_t *ppos)
{
	int rc;
	unsigned int c, p, bytes1 = 0, bytes2 = 0;
	struct per_user_data *u = file->private_data;

	/* Whole number of ports. */
	count &= ~(sizeof(evtchn_port_t)-1);

	if (count == 0)
		return 0;

	if (count > PAGE_SIZE)
		count = PAGE_SIZE;

	for (;;) {
		if (u->ring_overflow)
			return -EFBIG;

		if ((c = u->ring_cons) != (p = u->ring_prod))
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		rc = wait_event_interruptible(
			u->evtchn_wait, u->ring_cons != u->ring_prod);
		if (rc)
			return rc;
	}

	/* Byte lengths of two chunks. Chunk split (if any) is at ring wrap. */
	if (((c ^ p) & EVTCHN_RING_SIZE) != 0) {
		bytes1 = (EVTCHN_RING_SIZE - EVTCHN_RING_MASK(c)) *
			sizeof(evtchn_port_t);
		bytes2 = EVTCHN_RING_MASK(p) * sizeof(evtchn_port_t);
	} else {
		bytes1 = (p - c) * sizeof(evtchn_port_t);
		bytes2 = 0;
	}

	/* Truncate chunks according to caller's maximum byte count. */
	if (bytes1 > count) {
		bytes1 = count;
		bytes2 = 0;
	} else if ((bytes1 + bytes2) > count) {
		bytes2 = count - bytes1;
	}

	if (copy_to_user(buf, &u->ring[EVTCHN_RING_MASK(c)], bytes1) ||
	    ((bytes2 != 0) &&
	     copy_to_user(&buf[bytes1], &u->ring[0], bytes2)))
		return -EFAULT;

	u->ring_cons += (bytes1 + bytes2) / sizeof(evtchn_port_t);

	return bytes1 + bytes2;
}

static ssize_t evtchn_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos)
{
	int  rc, i;
	evtchn_port_t *kbuf = (evtchn_port_t *)__get_free_page(GFP_KERNEL);
	struct per_user_data *u = file->private_data;

	if (kbuf == NULL)
		return -ENOMEM;

	/* Whole number of ports. */
	count &= ~(sizeof(evtchn_port_t)-1);

	if (count == 0) {
		rc = 0;
		goto out;
	}

	if (count > PAGE_SIZE)
		count = PAGE_SIZE;

	if (copy_from_user(kbuf, buf, count) != 0) {
		rc = -EFAULT;
		goto out;
	}

	spin_lock_irq(&port_user_lock);
	for (i = 0; i < (count/sizeof(evtchn_port_t)); i++)
		if ((kbuf[i] < NR_EVENT_CHANNELS) && (port_user[kbuf[i]] == u))
			unmask_evtchn(kbuf[i]);
	spin_unlock_irq(&port_user_lock);

	rc = count;

 out:
	free_page((unsigned long)kbuf);
	return rc;
}

static void evtchn_bind_to_user(struct per_user_data *u, int port)
{
	spin_lock_irq(&port_user_lock);
	BUG_ON(port_user[port] != NULL);
	port_user[port] = u;
	unmask_evtchn(port);
	spin_unlock_irq(&port_user_lock);
}

static int evtchn_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg)
{
	int rc;
	struct per_user_data *u = file->private_data;
	void __user *uarg = (void __user *) arg;
	evtchn_op_t op = { 0 };

	switch (cmd) {
	case IOCTL_EVTCHN_BIND_VIRQ: {
		struct ioctl_evtchn_bind_virq bind;

		rc = -EFAULT;
		if (copy_from_user(&bind, uarg, sizeof(bind)))
			break;

		op.cmd = EVTCHNOP_bind_virq;
		op.u.bind_virq.virq = bind.virq;
		op.u.bind_virq.vcpu = 0;
		rc = HYPERVISOR_event_channel_op(&op);
		if (rc != 0)
			break;

		rc = op.u.bind_virq.port;
		evtchn_bind_to_user(u, rc);
		break;
	}

	case IOCTL_EVTCHN_BIND_INTERDOMAIN: {
		struct ioctl_evtchn_bind_interdomain bind;

		rc = -EFAULT;
		if (copy_from_user(&bind, uarg, sizeof(bind)))
			break;

		op.cmd = EVTCHNOP_bind_interdomain;
		op.u.bind_interdomain.remote_dom  = bind.remote_domain;
		op.u.bind_interdomain.remote_port = bind.remote_port;
		rc = HYPERVISOR_event_channel_op(&op);
		if (rc != 0)
			break;

		rc = op.u.bind_interdomain.local_port;
		evtchn_bind_to_user(u, rc);
		break;
	}

	case IOCTL_EVTCHN_BIND_UNBOUND_PORT: {
		struct ioctl_evtchn_bind_unbound_port bind;

		rc = -EFAULT;
		if (copy_from_user(&bind, uarg, sizeof(bind)))
			break;

		op.cmd = EVTCHNOP_alloc_unbound;
		op.u.alloc_unbound.dom        = DOMID_SELF;
		op.u.alloc_unbound.remote_dom = bind.remote_domain;
		rc = HYPERVISOR_event_channel_op(&op);
		if (rc != 0)
			break;

		rc = op.u.alloc_unbound.port;
		evtchn_bind_to_user(u, rc);
		break;
	}

	case IOCTL_EVTCHN_UNBIND: {
		struct ioctl_evtchn_unbind unbind;
		int ret;

		rc = -EFAULT;
		if (copy_from_user(&unbind, uarg, sizeof(unbind)))
			break;

		rc = -EINVAL;
		if (unbind.port >= NR_EVENT_CHANNELS)
			break;

		spin_lock_irq(&port_user_lock);
    
		rc = -ENOTCONN;
		if (port_user[unbind.port] != u) {
			spin_unlock_irq(&port_user_lock);
			break;
		}

		port_user[unbind.port] = NULL;
		mask_evtchn(unbind.port);

		spin_unlock_irq(&port_user_lock);

		op.cmd = EVTCHNOP_close;
		op.u.close.port = unbind.port;
		ret = HYPERVISOR_event_channel_op(&op);
		BUG_ON(ret);

		rc = 0;
		break;
	}

	case IOCTL_EVTCHN_NOTIFY: {
		struct ioctl_evtchn_notify notify;

		rc = -EFAULT;
		if (copy_from_user(&notify, uarg, sizeof(notify)))
			break;

		if (notify.port >= NR_EVENT_CHANNELS) {
			rc = -EINVAL;
		} else if (port_user[notify.port] != u) {
			rc = -ENOTCONN;
		} else {
			notify_remote_via_evtchn(notify.port);
			rc = 0;
		}
		break;
	}

	case IOCTL_EVTCHN_RESET: {
		/* Initialise the ring to empty. Clear errors. */
		spin_lock_irq(&port_user_lock);
		u->ring_cons = u->ring_prod = u->ring_overflow = 0;
		spin_unlock_irq(&port_user_lock);
		rc = 0;
		break;
	}

	default:
		rc = -ENOSYS;
		break;
	}

	return rc;
}

static unsigned int evtchn_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = POLLOUT | POLLWRNORM;
	struct per_user_data *u = file->private_data;

	poll_wait(file, &u->evtchn_wait, wait);
	if (u->ring_cons != u->ring_prod)
		mask |= POLLIN | POLLRDNORM;
	if (u->ring_overflow)
		mask = POLLERR;
	return mask;
}

static int evtchn_fasync(int fd, struct file *filp, int on)
{
	struct per_user_data *u = filp->private_data;
	return fasync_helper(fd, filp, on, &u->evtchn_async_queue);
}

static int evtchn_open(struct inode *inode, struct file *filp)
{
	struct per_user_data *u;

	if ((u = kmalloc(sizeof(*u), GFP_KERNEL)) == NULL)
		return -ENOMEM;

	memset(u, 0, sizeof(*u));
	init_waitqueue_head(&u->evtchn_wait);

	u->ring = (evtchn_port_t *)__get_free_page(GFP_KERNEL);
	if (u->ring == NULL) {
		kfree(u);
		return -ENOMEM;
	}

	filp->private_data = u;

	return 0;
}

static int evtchn_release(struct inode *inode, struct file *filp)
{
	int i;
	struct per_user_data *u = filp->private_data;
	evtchn_op_t op = { 0 };

	spin_lock_irq(&port_user_lock);

	free_page((unsigned long)u->ring);

	for (i = 0; i < NR_EVENT_CHANNELS; i++) {
		int ret;
		if (port_user[i] != u)
			continue;

		port_user[i] = NULL;
		mask_evtchn(i);

		op.cmd = EVTCHNOP_close;
		op.u.close.port = i;
		ret = HYPERVISOR_event_channel_op(&op);
		BUG_ON(ret);
	}

	spin_unlock_irq(&port_user_lock);

	kfree(u);

	return 0;
}

static struct file_operations evtchn_fops = {
	.owner   = THIS_MODULE,
	.read    = evtchn_read,
	.write   = evtchn_write,
	.ioctl   = evtchn_ioctl,
	.poll    = evtchn_poll,
	.fasync  = evtchn_fasync,
	.open    = evtchn_open,
	.release = evtchn_release,
};

static struct miscdevice evtchn_miscdev = {
	.minor        = EVTCHN_MINOR,
	.name         = "evtchn",
	.fops         = &evtchn_fops,
	.devfs_name   = "misc/evtchn",
};

static int __init evtchn_init(void)
{
	int err;

	spin_lock_init(&port_user_lock);
	memset(port_user, 0, sizeof(port_user));

	/* Create '/dev/misc/evtchn'. */
	err = misc_register(&evtchn_miscdev);
	if (err != 0) {
		printk(KERN_ALERT "Could not register /dev/misc/evtchn\n");
		return err;
	}

	printk("Event-channel device installed.\n");

	return 0;
}

static void evtchn_cleanup(void)
{
	misc_deregister(&evtchn_miscdev);
}

module_init(evtchn_init);
module_exit(evtchn_cleanup);

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
