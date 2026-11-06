/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ringchar_ioctl.h - Userspace ABI for the ringchar character device.
 *
 * Shared verbatim between the module and userspace, so the two can never
 * disagree about command numbers or struct layout. Every field is explicitly
 * sized and the struct is padded to a multiple of 8 bytes, which keeps the
 * layout identical for a 32-bit process on a 64-bit kernel and removes the
 * need for a compat_ioctl handler.
 */

#ifndef RINGCHAR_IOCTL_H
#define RINGCHAR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
/*
 * Userspace gets the __u32/__u64 definitions from <linux/types.h> as well.
 * Typedefing them here instead collides with the kernel UAPI headers that any
 * non-trivial program already pulls in transitively (sys/stat.h reaches them
 * via statx), because __u64 is `unsigned long long` there and `uint64_t` is
 * `unsigned long` on LP64 -- the same width, but a different type to the
 * compiler.
 */
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

#define RINGCHAR_IOC_MAGIC 'R'

/* Snapshot of device state. Read-only from userspace. */
struct ringchar_stats {
	__u32 capacity;      /* ring size in bytes */
	__u32 used;          /* bytes currently readable */
	__u32 free;          /* bytes currently writable */
	__u32 overruns;      /* writes that hit a full buffer */
	__u32 underruns;     /* reads that hit an empty buffer */
	__u32 readers;       /* open file descriptors reading */
	__u32 writers;       /* open file descriptors writing */
	__u32 overwrite;     /* 1 if drop-oldest mode is enabled */
	__u64 bytes_in;
	__u64 bytes_out;
};

/*
 * Overwrite policy.
 *
 * Default (0) is drop-newest: a write to a full buffer returns a short count,
 * or -EAGAIN when nothing fits and O_NONBLOCK is set. This is right for a
 * reliable stream, where losing the newest data is recoverable by retrying.
 *
 * Mode 1 is drop-oldest: the oldest bytes are discarded to make room. This is
 * right for a real-time capture path -- a sensor or audio producer that cannot
 * stall, where the freshest sample matters more than a complete history.
 *
 * The choice belongs to the application, not the driver, which is why it is an
 * ioctl rather than a module parameter.
 */
#define RINGCHAR_IOC_GET_STATS    _IOR(RINGCHAR_IOC_MAGIC, 1, struct ringchar_stats)
#define RINGCHAR_IOC_RESET        _IO(RINGCHAR_IOC_MAGIC,  2)
#define RINGCHAR_IOC_RESET_STATS  _IO(RINGCHAR_IOC_MAGIC,  3)
#define RINGCHAR_IOC_SET_OVERWRITE _IOW(RINGCHAR_IOC_MAGIC, 4, __u32)
#define RINGCHAR_IOC_GET_OVERWRITE _IOR(RINGCHAR_IOC_MAGIC, 5, __u32)
/* Non-destructive read: copies out without consuming. Argument is a pointer to
 * a user buffer; the count comes from the low 16 bits of the ioctl size field,
 * so peek is capped at a single page in practice. */
#define RINGCHAR_IOC_PEEK         _IOR(RINGCHAR_IOC_MAGIC, 6, __u32)
#define RINGCHAR_IOC_DISCARD      _IOW(RINGCHAR_IOC_MAGIC, 7, __u32)

#define RINGCHAR_IOC_MAXNR 7

#endif /* RINGCHAR_IOCTL_H */
