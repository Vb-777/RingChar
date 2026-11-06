// SPDX-License-Identifier: GPL-2.0
/*
 * ringchar.c - Character device exposing a ring buffer with blocking I/O,
 *              poll/epoll support, ioctl control and sysfs statistics.
 *
 * The buffer discipline lives in ../core/ring.h and is unit-tested, fuzzed and
 * benchmarked in userspace. This file is deliberately only the glue: the
 * file_operations table, the user-copy boundary, the sleep/wake discipline and
 * the lifetime rules. Keeping it that thin is the point -- a bug in wrap-around
 * arithmetic is an assertion failure in a test binary, while a bug here is a
 * machine that needs rebooting.
 *
 * LOCKING
 * =======
 * The core is lock-free for exactly one producer and one consumer. A character
 * device cannot assume that: nothing stops two processes opening the node and
 * writing concurrently. So the module serialises each side with its own mutex
 * -- read_lock for consumers, write_lock for producers -- which preserves the
 * core's SPSC precondition while still allowing a reader and a writer to
 * proceed in parallel. A single device-wide mutex would serialise reader
 * against writer and throw away the lock-free property the core provides.
 *
 * The mutexes are taken with mutex_lock_interruptible so a blocked process
 * stays killable.
 *
 * SLEEPING
 * ========
 * wait_event_interruptible is used rather than a hand-rolled sleep, and the
 * wake-up is issued after the data is published, so a woken reader always sees
 * the bytes that woke it. Both wait queues are woken on every state change
 * that could unblock the other side, including on release, so a reader blocked
 * on an empty buffer does not hang forever once the last writer closes.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "../core/ring.h"
#include "ringchar_ioctl.h"

#define DRV_NAME   "ringchar"
#define DRV_CLASS  "ringchar"

/* Bounce buffer size for the user-copy boundary. copy_from_user cannot be
 * called while holding a spinlock and cannot fault into the ring directly, so
 * transfers are staged through a per-device scratch buffer under the relevant
 * mutex. One page is a good trade: large enough that the syscall overhead
 * dominates, small enough to kmalloc reliably. */
#define BOUNCE_SIZE PAGE_SIZE

static int capacity = 65536;
module_param(capacity, int, 0444);
MODULE_PARM_DESC(capacity, "Ring capacity in bytes, rounded up to a power of two (default 65536)");

static int n_devices = 1;
module_param(n_devices, int, 0444);
MODULE_PARM_DESC(n_devices, "Number of /dev/ringchar* nodes to create (default 1)");

struct ringchar_dev {
	struct ring         ring;
	u8                 *storage;
	u8                 *bounce_in;
	u8                 *bounce_out;
	struct mutex        read_lock;
	struct mutex        write_lock;
	wait_queue_head_t   readq;
	wait_queue_head_t   writeq;
	atomic_t            readers;
	atomic_t            writers;
	bool                overwrite;
	struct device      *device;
	int                 index;
};

static int ringchar_major;
static struct class *ringchar_class;
static struct cdev *ringchar_cdevs;
static struct ringchar_dev *ringchar_devs;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static u32 round_up_pow2(u32 n)
{
	u32 p = 1;

	while (p < n && p < (1u << 31))
		p <<= 1;
	return p;
}

/* Readable/writable predicates for the wait conditions. Each takes the
 * matching mutex-free fast path: ring_used() is safe from either side. */
static bool dev_readable(struct ringchar_dev *dev)
{
	return !ring_empty(&dev->ring);
}

static bool dev_writable(struct ringchar_dev *dev)
{
	return dev->overwrite || !ring_full(&dev->ring);
}

/* ------------------------------------------------------------------ */
/* open / release                                                      */
/* ------------------------------------------------------------------ */

static int ringchar_open(struct inode *inode, struct file *filp)
{
	struct ringchar_dev *dev;
	unsigned int minor = iminor(inode);

	if (minor >= (unsigned int)n_devices)
		return -ENODEV;

	dev = &ringchar_devs[minor];
	filp->private_data = dev;

	/* Track both counts regardless of mode so the stats are meaningful for
	 * O_RDWR openers too. */
	if (filp->f_mode & FMODE_READ)
		atomic_inc(&dev->readers);
	if (filp->f_mode & FMODE_WRITE)
		atomic_inc(&dev->writers);

	return 0;
}

static int ringchar_release(struct inode *inode, struct file *filp)
{
	struct ringchar_dev *dev = filp->private_data;

	if (filp->f_mode & FMODE_READ)
		atomic_dec(&dev->readers);
	if (filp->f_mode & FMODE_WRITE)
		atomic_dec(&dev->writers);

	/* Wake both sides: a reader blocked on an empty ring must be allowed to
	 * observe that the last writer has gone, otherwise it sleeps forever. */
	wake_up_interruptible(&dev->readq);
	wake_up_interruptible(&dev->writeq);
	return 0;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

static ssize_t ringchar_read(struct file *filp, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct ringchar_dev *dev = filp->private_data;
	size_t done = 0;
	int ret;

	if (count == 0)
		return 0;

	if (mutex_lock_interruptible(&dev->read_lock))
		return -ERESTARTSYS;

	while (!dev_readable(dev)) {
		mutex_unlock(&dev->read_lock);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* Returning 0 at EOF requires knowing no writer can appear. With
		 * no writers open and an empty ring, report end-of-file rather
		 * than blocking indefinitely -- this is what makes
		 * `cat /dev/ringchar0` terminate. */
		if (atomic_read(&dev->writers) == 0)
			return 0;

		ret = wait_event_interruptible(dev->readq,
					       dev_readable(dev) ||
					       atomic_read(&dev->writers) == 0);
		if (ret)
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&dev->read_lock))
			return -ERESTARTSYS;

		if (!dev_readable(dev) && atomic_read(&dev->writers) == 0) {
			mutex_unlock(&dev->read_lock);
			return 0;
		}
	}

	/* Drain through the bounce buffer. Stop at the first short copy so a
	 * partial read is returned rather than blocking for the remainder --
	 * read(2) semantics, and what a framing-aware reader expects. */
	while (done < count) {
		size_t want = min_t(size_t, count - done, BOUNCE_SIZE);
		u32 got = ring_read(&dev->ring, dev->bounce_out, (u32)want);

		if (got == 0)
			break;
		if (copy_to_user(ubuf + done, dev->bounce_out, got)) {
			/* The bytes are already consumed from the ring. Report
			 * what was successfully delivered, if any, so the caller
			 * is not told a fault lost data it did receive. */
			mutex_unlock(&dev->read_lock);
			wake_up_interruptible(&dev->writeq);
			return done ? (ssize_t)done : -EFAULT;
		}
		done += got;
	}

	mutex_unlock(&dev->read_lock);

	/* Space freed: a blocked writer may now proceed. */
	wake_up_interruptible(&dev->writeq);
	return (ssize_t)done;
}

/* ------------------------------------------------------------------ */
/* write                                                               */
/* ------------------------------------------------------------------ */

static ssize_t ringchar_write(struct file *filp, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct ringchar_dev *dev = filp->private_data;
	size_t done = 0;
	int ret;

	if (count == 0)
		return 0;

	if (mutex_lock_interruptible(&dev->write_lock))
		return -ERESTARTSYS;

	while (!dev_writable(dev)) {
		mutex_unlock(&dev->write_lock);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(dev->writeq, dev_writable(dev));
		if (ret)
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&dev->write_lock))
			return -ERESTARTSYS;
	}

	while (done < count) {
		size_t want = min_t(size_t, count - done, BOUNCE_SIZE);
		u32 put;

		if (copy_from_user(dev->bounce_in, ubuf + done, want)) {
			mutex_unlock(&dev->write_lock);
			wake_up_interruptible(&dev->readq);
			return done ? (ssize_t)done : -EFAULT;
		}

		if (dev->overwrite) {
			/* Drop-oldest: make room, then write. Discarding only
			 * the shortfall keeps as much history as will fit
			 * instead of clearing the whole buffer. */
			u32 space = ring_free(&dev->ring);

			if (space < want)
				ring_discard(&dev->ring, (u32)want - space);
		}

		put = ring_write(&dev->ring, dev->bounce_in, (u32)want);
		done += put;
		if (put < want)
			break;   /* filled up: short write */
	}

	mutex_unlock(&dev->write_lock);

	/* Data published: wake readers after the ring_write release store, so a
	 * woken reader is guaranteed to see the bytes. */
	wake_up_interruptible(&dev->readq);
	return (ssize_t)done;
}

/* ------------------------------------------------------------------ */
/* poll                                                                */
/* ------------------------------------------------------------------ */

static __poll_t ringchar_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct ringchar_dev *dev = filp->private_data;
	__poll_t mask = 0;

	/* Both queues are registered before the state is sampled. Sampling
	 * first would race: a change between the check and poll_wait would be
	 * missed and the caller would sleep with data available. */
	poll_wait(filp, &dev->readq, wait);
	poll_wait(filp, &dev->writeq, wait);

	if (dev_readable(dev))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (dev_writable(dev))
		mask |= EPOLLOUT | EPOLLWRNORM;
	/* No writers and nothing buffered is end-of-stream, which epoll
	 * consumers detect via EPOLLHUP. */
	if (atomic_read(&dev->writers) == 0 && ring_empty(&dev->ring))
		mask |= EPOLLHUP;

	return mask;
}

/* ------------------------------------------------------------------ */
/* ioctl                                                               */
/* ------------------------------------------------------------------ */

static long ringchar_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct ringchar_dev *dev = filp->private_data;
	struct ringchar_stats stats;
	u32 value;
	long ret = 0;

	if (_IOC_TYPE(cmd) != RINGCHAR_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > RINGCHAR_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case RINGCHAR_IOC_GET_STATS:
		memset(&stats, 0, sizeof(stats));
		stats.capacity  = dev->ring.capacity;
		stats.used      = ring_used(&dev->ring);
		stats.free      = ring_free(&dev->ring);
		stats.overruns  = dev->ring.overruns;
		stats.underruns = dev->ring.underruns;
		stats.readers   = atomic_read(&dev->readers);
		stats.writers   = atomic_read(&dev->writers);
		stats.overwrite = dev->overwrite ? 1 : 0;
		stats.bytes_in  = dev->ring.bytes_in;
		stats.bytes_out = dev->ring.bytes_out;
		if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		break;

	case RINGCHAR_IOC_RESET:
		/* Both locks: reset is not safe against either side, and they
		 * are always taken read-then-write to avoid a deadlock with a
		 * concurrent reset. */
		if (mutex_lock_interruptible(&dev->read_lock))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&dev->write_lock)) {
			mutex_unlock(&dev->read_lock);
			return -ERESTARTSYS;
		}
		ring_reset(&dev->ring);
		mutex_unlock(&dev->write_lock);
		mutex_unlock(&dev->read_lock);
		wake_up_interruptible(&dev->writeq);
		break;

	case RINGCHAR_IOC_RESET_STATS:
		ring_reset_stats(&dev->ring);
		break;

	case RINGCHAR_IOC_SET_OVERWRITE:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		dev->overwrite = !!value;
		/* Enabling drop-oldest makes the device permanently writable,
		 * so anyone blocked in write() can proceed. */
		wake_up_interruptible(&dev->writeq);
		break;

	case RINGCHAR_IOC_GET_OVERWRITE:
		value = dev->overwrite ? 1 : 0;
		if (copy_to_user((void __user *)arg, &value, sizeof(value)))
			return -EFAULT;
		break;

	case RINGCHAR_IOC_DISCARD:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (mutex_lock_interruptible(&dev->read_lock))
			return -ERESTARTSYS;
		value = ring_discard(&dev->ring, value);
		mutex_unlock(&dev->read_lock);
		wake_up_interruptible(&dev->writeq);
		if (copy_to_user((void __user *)arg, &value, sizeof(value)))
			return -EFAULT;
		break;

	default:
		ret = -ENOTTY;
	}
	return ret;
}

static const struct file_operations ringchar_fops = {
	.owner          = THIS_MODULE,
	.open           = ringchar_open,
	.release        = ringchar_release,
	.read           = ringchar_read,
	.write          = ringchar_write,
	.poll           = ringchar_poll,
	.unlocked_ioctl = ringchar_ioctl,
	/* The ABI struct is explicitly sized and padded, so the 32-bit layout
	 * matches the 64-bit one and compat needs no translation. */
	.compat_ioctl   = compat_ptr_ioctl,
	.llseek         = no_llseek,
};

/* ------------------------------------------------------------------ */
/* sysfs                                                               */
/* ------------------------------------------------------------------ */

static struct ringchar_dev *dev_from_kobj(struct device *d)
{
	return dev_get_drvdata(d);
}

static ssize_t used_show(struct device *d, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "%u\n", ring_used(&dev_from_kobj(d)->ring));
}

static ssize_t capacity_show(struct device *d, struct device_attribute *a,
			     char *b)
{
	return sysfs_emit(b, "%u\n", dev_from_kobj(d)->ring.capacity);
}

static ssize_t overruns_show(struct device *d, struct device_attribute *a,
			     char *b)
{
	return sysfs_emit(b, "%u\n", dev_from_kobj(d)->ring.overruns);
}

static ssize_t bytes_show(struct device *d, struct device_attribute *a, char *b)
{
	struct ringchar_dev *dev = dev_from_kobj(d);

	return sysfs_emit(b, "in=%llu out=%llu\n",
			  dev->ring.bytes_in, dev->ring.bytes_out);
}

static ssize_t overwrite_show(struct device *d, struct device_attribute *a,
			      char *b)
{
	return sysfs_emit(b, "%u\n", dev_from_kobj(d)->overwrite ? 1 : 0);
}

static ssize_t overwrite_store(struct device *d, struct device_attribute *a,
			       const char *b, size_t n)
{
	struct ringchar_dev *dev = dev_from_kobj(d);
	bool v;

	if (kstrtobool(b, &v))
		return -EINVAL;
	dev->overwrite = v;
	wake_up_interruptible(&dev->writeq);
	return n;
}

static DEVICE_ATTR_RO(used);
static DEVICE_ATTR_RO(capacity);
static DEVICE_ATTR_RO(overruns);
static DEVICE_ATTR_RO(bytes);
static DEVICE_ATTR_RW(overwrite);

static struct attribute *ringchar_attrs[] = {
	&dev_attr_used.attr,
	&dev_attr_capacity.attr,
	&dev_attr_overruns.attr,
	&dev_attr_bytes.attr,
	&dev_attr_overwrite.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ringchar);

/* ------------------------------------------------------------------ */
/* init / exit                                                         */
/* ------------------------------------------------------------------ */

static void ringchar_teardown(int created)
{
	int i;

	for (i = created - 1; i >= 0; i--) {
		if (ringchar_devs[i].device)
			device_destroy(ringchar_class,
				       MKDEV(ringchar_major, i));
		cdev_del(&ringchar_cdevs[i]);
		kfree(ringchar_devs[i].storage);
		kfree(ringchar_devs[i].bounce_in);
		kfree(ringchar_devs[i].bounce_out);
	}
	if (ringchar_class)
		class_destroy(ringchar_class);
	unregister_chrdev_region(MKDEV(ringchar_major, 0), n_devices);
	kfree(ringchar_cdevs);
	kfree(ringchar_devs);
}

static int __init ringchar_init(void)
{
	dev_t first;
	u32 cap;
	int i, ret, created = 0;

	if (n_devices < 1 || n_devices > 64)
		return -EINVAL;
	if (capacity < 2)
		return -EINVAL;

	cap = round_up_pow2((u32)capacity);
	if (cap != (u32)capacity)
		pr_info("%s: capacity rounded up to %u\n", DRV_NAME, cap);

	ringchar_devs = kcalloc(n_devices, sizeof(*ringchar_devs), GFP_KERNEL);
	ringchar_cdevs = kcalloc(n_devices, sizeof(*ringchar_cdevs), GFP_KERNEL);
	if (!ringchar_devs || !ringchar_cdevs) {
		kfree(ringchar_devs);
		kfree(ringchar_cdevs);
		return -ENOMEM;
	}

	ret = alloc_chrdev_region(&first, 0, n_devices, DRV_NAME);
	if (ret) {
		kfree(ringchar_devs);
		kfree(ringchar_cdevs);
		return ret;
	}
	ringchar_major = MAJOR(first);

	ringchar_class = class_create(DRV_CLASS);
	if (IS_ERR(ringchar_class)) {
		ret = PTR_ERR(ringchar_class);
		ringchar_class = NULL;
		goto fail;
	}
	ringchar_class->dev_groups = ringchar_groups;

	for (i = 0; i < n_devices; i++) {
		struct ringchar_dev *dev = &ringchar_devs[i];

		dev->index = i;
		dev->storage    = kmalloc(cap, GFP_KERNEL);
		dev->bounce_in  = kmalloc(BOUNCE_SIZE, GFP_KERNEL);
		dev->bounce_out = kmalloc(BOUNCE_SIZE, GFP_KERNEL);
		if (!dev->storage || !dev->bounce_in || !dev->bounce_out) {
			ret = -ENOMEM;
			goto fail_partial;
		}
		if (ring_init(&dev->ring, dev->storage, cap)) {
			ret = -EINVAL;
			goto fail_partial;
		}

		mutex_init(&dev->read_lock);
		mutex_init(&dev->write_lock);
		init_waitqueue_head(&dev->readq);
		init_waitqueue_head(&dev->writeq);
		atomic_set(&dev->readers, 0);
		atomic_set(&dev->writers, 0);
		dev->overwrite = false;

		cdev_init(&ringchar_cdevs[i], &ringchar_fops);
		ringchar_cdevs[i].owner = THIS_MODULE;
		ret = cdev_add(&ringchar_cdevs[i],
			       MKDEV(ringchar_major, i), 1);
		if (ret)
			goto fail_partial;

		dev->device = device_create(ringchar_class, NULL,
					    MKDEV(ringchar_major, i), dev,
					    "%s%d", DRV_NAME, i);
		if (IS_ERR(dev->device)) {
			ret = PTR_ERR(dev->device);
			dev->device = NULL;
			cdev_del(&ringchar_cdevs[i]);
			goto fail_partial;
		}
		created = i + 1;
	}

	pr_info("%s: major %d, %d device(s), %u-byte ring\n",
		DRV_NAME, ringchar_major, n_devices, cap);
	return 0;

fail_partial:
	/* The device at index i was not fully created; free its allocations
	 * here and let teardown unwind the ones that were. */
	kfree(ringchar_devs[i].storage);
	kfree(ringchar_devs[i].bounce_in);
	kfree(ringchar_devs[i].bounce_out);
	ringchar_devs[i].storage = NULL;
	ringchar_devs[i].bounce_in = NULL;
	ringchar_devs[i].bounce_out = NULL;
fail:
	ringchar_teardown(created);
	return ret;
}

static void __exit ringchar_exit(void)
{
	ringchar_teardown(n_devices);
	pr_info("%s: unloaded\n", DRV_NAME);
}

module_init(ringchar_init);
module_exit(ringchar_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Vaibhav Barsaiyan");
MODULE_DESCRIPTION("Ring-buffer character device with poll, ioctl and sysfs");
MODULE_VERSION("1.0");
