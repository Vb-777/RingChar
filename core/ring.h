/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ring.h - Lock-free single-producer/single-consumer byte ring buffer.
 *
 * WHY THIS FILE HAS NO KERNEL HEADERS
 * ===================================
 * The hard part of a character driver is not the file_operations table; it is
 * the buffer discipline underneath it -- wrap-around arithmetic, the
 * full-versus-empty ambiguity, partial reads and writes, and the memory
 * ordering that makes a concurrent producer and consumer safe without a lock.
 * Those are exactly the parts that are painful to debug inside a kernel module,
 * where a mistake is a hung machine rather than a failed assertion.
 *
 * So this layer is deliberately free of kernel dependencies: it compiles both
 * in-kernel and in userspace, which means the buffer logic can be unit-tested,
 * fuzzed and benchmarked as ordinary code, and the kernel module reduces to
 * thin glue over an already-verified core.
 *
 * MEMORY MODEL
 * ============
 * Single producer, single consumer, no locks. Correctness rests on:
 *
 *   - head is written only by the producer, tail only by the consumer;
 *   - each side reads the other's index with ACQUIRE semantics and publishes
 *     its own with RELEASE semantics, so data writes cannot be reordered past
 *     the index update that advertises them;
 *   - indices are free-running and masked on use, so the full/empty ambiguity
 *     disappears: empty is head == tail, full is head - tail == capacity.
 *     Storing masked indices instead would make those two states identical and
 *     force a wasted slot.
 *
 * Capacity must be a power of two so that masking replaces the modulo, which
 * matters on the per-byte path.
 *
 * The free-running indices are unsigned and are allowed to wrap. Differences
 * are computed in unsigned arithmetic, which stays correct across the wrap as
 * long as capacity is far below the index range -- true for any realistic
 * buffer with a 32-bit index.
 */

#ifndef RINGCHAR_RING_H
#define RINGCHAR_RING_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#include <linux/compiler.h>
#define RING_ACQUIRE(p)      smp_load_acquire(p)
#define RING_RELEASE(p, v)   smp_store_release((p), (v))
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t  u8;
/* C11 atomics give the same ordering guarantees the kernel macros do. */
#include <stdatomic.h>
#define RING_ACQUIRE(p)      atomic_load_explicit((const _Atomic u32 *)(p),    \
                                                  memory_order_acquire)
#define RING_RELEASE(p, v)   atomic_store_explicit((_Atomic u32 *)(p), (v),    \
                                                   memory_order_release)
#endif

struct ring {
	u8  *buf;
	u32  capacity;   /* power of two */
	u32  mask;       /* capacity - 1 */
	u32  head;       /* producer index, free-running */
	u32  tail;       /* consumer index, free-running */
	/* Statistics. Not part of the concurrency contract: they are advisory
	 * counters for sysfs and may be read torn. */
	u32  overruns;   /* writes refused because the buffer was full */
	u32  underruns;  /* reads refused because the buffer was empty */
	u64  bytes_in;
	u64  bytes_out;
};

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static inline int ring_is_power_of_two(u32 n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

/*
 * Initialise a ring over caller-provided storage.
 * Returns 0 on success, -1 if capacity is not a non-zero power of two.
 * Storage ownership stays with the caller: in-kernel it is kmalloc'd by the
 * module, in tests it is a stack or heap array.
 */
static inline int ring_init(struct ring *r, u8 *storage, u32 capacity)
{
	if (!r || !storage || !ring_is_power_of_two(capacity))
		return -1;
	r->buf = storage;
	r->capacity = capacity;
	r->mask = capacity - 1;
	r->head = 0;
	r->tail = 0;
	r->overruns = 0;
	r->underruns = 0;
	r->bytes_in = 0;
	r->bytes_out = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/* Bytes currently readable. Safe to call from either side. */
static inline u32 ring_used(const struct ring *r)
{
	u32 head = RING_ACQUIRE(&r->head);
	u32 tail = RING_ACQUIRE(&r->tail);
	return head - tail;
}

static inline u32 ring_free(const struct ring *r)
{
	return r->capacity - ring_used(r);
}

static inline int ring_empty(const struct ring *r)
{
	return ring_used(r) == 0;
}

static inline int ring_full(const struct ring *r)
{
	return ring_used(r) == r->capacity;
}

/* ------------------------------------------------------------------ */
/* Producer side                                                       */
/* ------------------------------------------------------------------ */

/*
 * Copy up to @len bytes in. Returns the number accepted, which is less than
 * @len when the buffer fills -- a short write, not an error, mirroring what
 * write(2) is allowed to do.
 *
 * The copy is split into at most two memcpy calls at the wrap point rather
 * than looping per byte; on the benchmark this is what separates a few hundred
 * MB/s from several GB/s.
 */
static inline u32 ring_write(struct ring *r, const u8 *src, u32 len)
{
	u32 head = r->head;                     /* producer owns head */
	u32 tail = RING_ACQUIRE(&r->tail);      /* consumer publishes tail */
	u32 space = r->capacity - (head - tail);
	u32 first, offset;

	if (len > space) {
		len = space;
		r->overruns++;
	}
	if (len == 0)
		return 0;

	offset = head & r->mask;
	first = r->capacity - offset;
	if (first > len)
		first = len;

	memcpy(r->buf + offset, src, first);
	if (len > first)
		memcpy(r->buf, src + first, len - first);

	/* Publish only after the payload is in place. */
	RING_RELEASE(&r->head, head + len);
	r->bytes_in += len;
	return len;
}

/* ------------------------------------------------------------------ */
/* Consumer side                                                       */
/* ------------------------------------------------------------------ */

/*
 * Copy up to @len bytes out and consume them. Returns the number produced,
 * zero when empty.
 */
static inline u32 ring_read(struct ring *r, u8 *dst, u32 len)
{
	u32 tail = r->tail;                     /* consumer owns tail */
	u32 head = RING_ACQUIRE(&r->head);      /* producer publishes head */
	u32 used = head - tail;
	u32 first, offset;

	if (len > used) {
		len = used;
		r->underruns++;
	}
	if (len == 0)
		return 0;

	offset = tail & r->mask;
	first = r->capacity - offset;
	if (first > len)
		first = len;

	memcpy(dst, r->buf + offset, first);
	if (len > first)
		memcpy(dst + first, r->buf, len - first);

	RING_RELEASE(&r->tail, tail + len);
	r->bytes_out += len;
	return len;
}

/*
 * Copy out without consuming. Used by the driver's non-destructive peek ioctl,
 * which lets a reader inspect framing before committing to a read.
 */
static inline u32 ring_peek(const struct ring *r, u8 *dst, u32 len)
{
	u32 tail = r->tail;
	u32 head = RING_ACQUIRE(&r->head);
	u32 used = head - tail;
	u32 first, offset;

	if (len > used)
		len = used;
	if (len == 0)
		return 0;

	offset = tail & r->mask;
	first = r->capacity - offset;
	if (first > len)
		first = len;

	memcpy(dst, r->buf + offset, first);
	if (len > first)
		memcpy(dst + first, r->buf, len - first);
	return len;
}

/*
 * Discard up to @len bytes. Returns the number discarded.
 * Separate from ring_read so a consumer can drop stale data without paying for
 * a copy it will throw away -- the overrun-recovery path in the driver.
 */
static inline u32 ring_discard(struct ring *r, u32 len)
{
	u32 tail = r->tail;
	u32 head = RING_ACQUIRE(&r->head);
	u32 used = head - tail;

	if (len > used)
		len = used;
	if (len == 0)
		return 0;
	RING_RELEASE(&r->tail, tail + len);
	return len;
}

/*
 * Reset to empty. NOT safe against a concurrent producer or consumer; the
 * driver holds its mutex and requires no open readers before calling this.
 */
static inline void ring_reset(struct ring *r)
{
	r->head = 0;
	r->tail = 0;
}

static inline void ring_reset_stats(struct ring *r)
{
	r->overruns = 0;
	r->underruns = 0;
	r->bytes_in = 0;
	r->bytes_out = 0;
}

#endif /* RINGCHAR_RING_H */
