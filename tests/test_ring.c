/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_ring.c - Unit and concurrency tests for the driver's buffer core.
 *
 * This is the file that justifies the kernel-agnostic split. Every case below
 * would be a kernel panic or a silent data corruption if it were found after
 * insmod instead of here.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/ring.h"

static int tests_run, tests_failed;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			tests_failed++;                                        \
			printf("  FAIL %s:%d: " fmt "\n", __func__, __LINE__,  \
			       ##__VA_ARGS__);                                 \
		}                                                              \
	} while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b), "expected %ld, got %ld", (long)(b),   \
			     (long)(a))

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static void test_init_rejects_bad_capacity(void)
{
	struct ring r;
	u8 buf[64];

	CHECK_EQ(ring_init(&r, buf, 0), -1);
	CHECK_EQ(ring_init(&r, buf, 3), -1);
	CHECK_EQ(ring_init(&r, buf, 100), -1);
	CHECK_EQ(ring_init(&r, NULL, 64), -1);
	CHECK_EQ(ring_init(&r, buf, 64), 0);
	CHECK_EQ(ring_init(&r, buf, 1), 0);
}

static void test_fresh_ring_is_empty(void)
{
	struct ring r;
	u8 buf[16];

	ring_init(&r, buf, 16);
	CHECK(ring_empty(&r), "fresh ring should be empty");
	CHECK(!ring_full(&r), "fresh ring should not be full");
	CHECK_EQ(ring_used(&r), 0);
	CHECK_EQ(ring_free(&r), 16);
}

/* ------------------------------------------------------------------ */
/* Basic transfer                                                      */
/* ------------------------------------------------------------------ */

static void test_roundtrip_preserves_bytes(void)
{
	struct ring r;
	u8 buf[16], out[16];
	const u8 in[] = "hello";

	ring_init(&r, buf, 16);
	CHECK_EQ(ring_write(&r, in, 5), 5);
	CHECK_EQ(ring_used(&r), 5);
	CHECK_EQ(ring_read(&r, out, 5), 5);
	CHECK_EQ(memcmp(in, out, 5), 0);
	CHECK(ring_empty(&r), "should be empty after draining");
}

static void test_capacity_is_fully_usable(void)
{
	/* A masked-index implementation wastes one slot here. Free-running
	 * indices must not: all 8 bytes have to fit. */
	struct ring r;
	u8 buf[8], out[8];
	const u8 in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	ring_init(&r, buf, 8);
	CHECK_EQ(ring_write(&r, in, 8), 8);
	CHECK(ring_full(&r), "8 bytes into an 8-byte ring should be full");
	CHECK_EQ(ring_free(&r), 0);
	CHECK_EQ(ring_read(&r, out, 8), 8);
	CHECK_EQ(memcmp(in, out, 8), 0);
}

static void test_write_to_full_is_a_short_write(void)
{
	struct ring r;
	u8 buf[4];
	const u8 in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	ring_init(&r, buf, 4);
	CHECK_EQ(ring_write(&r, in, 8), 4);   /* only 4 fit */
	CHECK_EQ(r.overruns, 1);
	CHECK_EQ(ring_write(&r, in, 1), 0);   /* nothing fits now */
	CHECK_EQ(r.overruns, 2);
}

static void test_read_from_empty_yields_nothing(void)
{
	struct ring r;
	u8 buf[8], out[8];

	ring_init(&r, buf, 8);
	CHECK_EQ(ring_read(&r, out, 8), 0);
	CHECK_EQ(r.underruns, 1);
}

static void test_partial_reads_consume_in_order(void)
{
	struct ring r;
	u8 buf[8], out[8];
	const u8 in[6] = { 10, 20, 30, 40, 50, 60 };

	ring_init(&r, buf, 8);
	ring_write(&r, in, 6);
	CHECK_EQ(ring_read(&r, out, 2), 2);
	CHECK_EQ(out[0], 10);
	CHECK_EQ(out[1], 20);
	CHECK_EQ(ring_read(&r, out, 10), 4);  /* short read of the remainder */
	CHECK_EQ(out[0], 30);
	CHECK_EQ(out[3], 60);
}

/* ------------------------------------------------------------------ */
/* Wrap-around: where ring buffers actually break                      */
/* ------------------------------------------------------------------ */

static void test_write_spanning_the_wrap_point(void)
{
	struct ring r;
	u8 buf[8], out[8];
	const u8 a[6] = { 1, 2, 3, 4, 5, 6 };
	const u8 b[5] = { 7, 8, 9, 10, 11 };

	ring_init(&r, buf, 8);
	ring_write(&r, a, 6);
	ring_read(&r, out, 6);          /* tail now at 6, buffer empty */
	CHECK_EQ(ring_write(&r, b, 5), 5);  /* must straddle the end */
	CHECK_EQ(ring_read(&r, out, 5), 5);
	CHECK_EQ(memcmp(b, out, 5), 0);
}

static void test_many_wraps_preserve_order(void)
{
	/* 10,000 bytes through a 16-byte ring: ~625 wraps. Any off-by-one in
	 * the split-copy arithmetic shows up as a value mismatch. */
	struct ring r;
	u8 buf[16];
	u32 produced = 0, consumed = 0;
	int ok = 1;

	ring_init(&r, buf, 16);
	while (consumed < 10000) {
		u8 chunk[7], out[7];
		u32 n, i, got;

		for (i = 0; i < sizeof(chunk) && produced + i < 10000; i++)
			chunk[i] = (u8)((produced + i) & 0xff);
		n = ring_write(&r, chunk, i);
		produced += n;

		got = ring_read(&r, out, sizeof(out));
		for (i = 0; i < got; i++) {
			if (out[i] != (u8)((consumed + i) & 0xff))
				ok = 0;
		}
		consumed += got;
		if (n == 0 && got == 0)
			break;
	}
	CHECK(ok, "byte sequence corrupted across wraps");
	CHECK_EQ(consumed, 10000);
}

static void test_index_wraparound_at_u32_limit(void)
{
	/* Free-running indices are allowed to overflow. Force head and tail
	 * close to UINT32_MAX and confirm the unsigned difference still gives
	 * the right occupancy once they wrap past zero. */
	struct ring r;
	u8 buf[16], out[16];
	const u8 in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	ring_init(&r, buf, 16);
	r.head = 0xfffffffcu;
	r.tail = 0xfffffffcu;
	CHECK_EQ(ring_used(&r), 0);
	CHECK_EQ(ring_write(&r, in, 8), 8);
	CHECK_EQ(ring_used(&r), 8);           /* head wrapped past zero */
	CHECK_EQ(ring_read(&r, out, 8), 8);
	CHECK_EQ(memcmp(in, out, 8), 0);
	CHECK_EQ(ring_used(&r), 0);
}

/* ------------------------------------------------------------------ */
/* Peek and discard                                                    */
/* ------------------------------------------------------------------ */

static void test_peek_does_not_consume(void)
{
	struct ring r;
	u8 buf[8], out[8];
	const u8 in[4] = { 9, 8, 7, 6 };

	ring_init(&r, buf, 8);
	ring_write(&r, in, 4);
	CHECK_EQ(ring_peek(&r, out, 4), 4);
	CHECK_EQ(memcmp(in, out, 4), 0);
	CHECK_EQ(ring_used(&r), 4);            /* still there */
	CHECK_EQ(ring_read(&r, out, 4), 4);
	CHECK_EQ(ring_used(&r), 0);
}

static void test_peek_across_the_wrap(void)
{
	struct ring r;
	u8 buf[8], out[8];
	const u8 a[6] = { 1, 1, 1, 1, 1, 1 };
	const u8 b[4] = { 2, 3, 4, 5 };

	ring_init(&r, buf, 8);
	ring_write(&r, a, 6);
	ring_read(&r, out, 6);
	ring_write(&r, b, 4);
	CHECK_EQ(ring_peek(&r, out, 4), 4);
	CHECK_EQ(memcmp(b, out, 4), 0);
}

static void test_discard_drops_without_copying(void)
{
	struct ring r;
	u8 buf[8], out[8];
	const u8 in[6] = { 1, 2, 3, 4, 5, 6 };

	ring_init(&r, buf, 8);
	ring_write(&r, in, 6);
	CHECK_EQ(ring_discard(&r, 4), 4);
	CHECK_EQ(ring_used(&r), 2);
	CHECK_EQ(ring_read(&r, out, 2), 2);
	CHECK_EQ(out[0], 5);
	CHECK_EQ(ring_discard(&r, 99), 0);     /* empty: nothing to drop */
}

/* ------------------------------------------------------------------ */
/* Statistics and reset                                                */
/* ------------------------------------------------------------------ */

static void test_byte_counters_track_traffic(void)
{
	struct ring r;
	u8 buf[16], out[16];
	const u8 in[8] = { 0 };

	ring_init(&r, buf, 16);
	ring_write(&r, in, 8);
	ring_read(&r, out, 3);
	CHECK_EQ(r.bytes_in, 8);
	CHECK_EQ(r.bytes_out, 3);
	ring_reset_stats(&r);
	CHECK_EQ(r.bytes_in, 0);
	CHECK_EQ(r.overruns, 0);
}

static void test_reset_empties_the_ring(void)
{
	struct ring r;
	u8 buf[16];
	const u8 in[8] = { 0 };

	ring_init(&r, buf, 16);
	ring_write(&r, in, 8);
	ring_reset(&r);
	CHECK(ring_empty(&r), "reset must leave the ring empty");
}

static void test_zero_length_operations_are_no_ops(void)
{
	struct ring r;
	u8 buf[16], out[16];
	const u8 in[4] = { 1, 2, 3, 4 };

	ring_init(&r, buf, 16);
	CHECK_EQ(ring_write(&r, in, 0), 0);
	CHECK_EQ(ring_used(&r), 0);
	ring_write(&r, in, 4);
	CHECK_EQ(ring_read(&r, out, 0), 0);
	CHECK_EQ(ring_used(&r), 4);           /* unchanged */
	CHECK_EQ(ring_peek(&r, out, 0), 0);
}

/* ------------------------------------------------------------------ */
/* Concurrency: one producer thread, one consumer thread               */
/* ------------------------------------------------------------------ */

#define CONC_BYTES (4u * 1024 * 1024)
#define CONC_CAP   4096u

struct conc_ctx {
	struct ring *r;
	int corrupt;
};

static void *producer_thread(void *arg)
{
	struct conc_ctx *ctx = arg;
	u8 chunk[512];
	u32 sent = 0;

	while (sent < CONC_BYTES) {
		u32 i, n, want = sizeof(chunk);

		if (CONC_BYTES - sent < want)
			want = CONC_BYTES - sent;
		for (i = 0; i < want; i++)
			chunk[i] = (u8)((sent + i) & 0xff);
		n = ring_write(ctx->r, chunk, want);
		sent += n;
		/* Spin when full: exercises the full boundary hard, which is
		 * where a missing barrier would surface. */
	}
	return NULL;
}

static void *consumer_thread(void *arg)
{
	struct conc_ctx *ctx = arg;
	u8 chunk[512];
	u32 got_total = 0;

	while (got_total < CONC_BYTES) {
		u32 i, n = ring_read(ctx->r, chunk, sizeof(chunk));

		for (i = 0; i < n; i++) {
			if (chunk[i] != (u8)((got_total + i) & 0xff))
				ctx->corrupt = 1;
		}
		got_total += n;
	}
	return NULL;
}

static void test_concurrent_producer_consumer(void)
{
	/* 4 MiB through a 4 KiB ring with two real threads. The payload is a
	 * known byte sequence, so a torn or reordered transfer is detected
	 * rather than merely suspected. */
	struct ring r;
	u8 *storage = malloc(CONC_CAP);
	struct conc_ctx ctx = { .r = &r, .corrupt = 0 };
	pthread_t prod, cons;

	ring_init(&r, storage, CONC_CAP);
	pthread_create(&prod, NULL, producer_thread, &ctx);
	pthread_create(&cons, NULL, consumer_thread, &ctx);
	pthread_join(prod, NULL);
	pthread_join(cons, NULL);

	CHECK(!ctx.corrupt, "data corrupted under concurrent access");
	CHECK_EQ(r.bytes_in, CONC_BYTES);
	CHECK_EQ(r.bytes_out, CONC_BYTES);
	CHECK(ring_empty(&r), "ring should be drained");
	free(storage);
}

/* ------------------------------------------------------------------ */
/* Randomised differential test against a simple model                 */
/* ------------------------------------------------------------------ */

static void test_random_ops_match_a_fifo_model(void)
{
	/* The ring must behave exactly like an unbounded FIFO truncated to
	 * capacity. A model comparison catches the cases hand-written tests
	 * miss, especially unusual interleavings of short reads and writes. */
	struct ring r;
	u8 buf[64];
	u8 model[4096];
	u32 model_head = 0, model_tail = 0;
	u8 next_value = 0;
	int ok = 1, i;
	unsigned seed = 12345;

	ring_init(&r, buf, 64);
	for (i = 0; i < 20000; i++) {
		seed = seed * 1103515245u + 12345u;
		if ((seed >> 16) & 1) {
			u8 chunk[40];
			u32 want = ((seed >> 8) % 40) + 1, j, n;

			for (j = 0; j < want; j++)
				chunk[j] = (u8)(next_value + j);
			n = ring_write(&r, chunk, want);
			for (j = 0; j < n; j++) {
				if (model_head < sizeof(model))
					model[model_head++] = chunk[j];
			}
			next_value = (u8)(next_value + n);
		} else {
			u8 out[40];
			u32 want = ((seed >> 8) % 40) + 1, j, n;

			n = ring_read(&r, out, want);
			for (j = 0; j < n; j++) {
				if (model_tail >= model_head ||
				    out[j] != model[model_tail++])
					ok = 0;
			}
		}
		if (ring_used(&r) != model_head - model_tail)
			ok = 0;
		/* Compact the model array so the run can be long. */
		if (model_tail > sizeof(model) / 2) {
			memmove(model, model + model_tail,
				model_head - model_tail);
			model_head -= model_tail;
			model_tail = 0;
		}
	}
	CHECK(ok, "ring diverged from the FIFO model");
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printf("ringchar core tests\n");

	test_init_rejects_bad_capacity();
	test_fresh_ring_is_empty();
	test_roundtrip_preserves_bytes();
	test_capacity_is_fully_usable();
	test_write_to_full_is_a_short_write();
	test_read_from_empty_yields_nothing();
	test_partial_reads_consume_in_order();
	test_write_spanning_the_wrap_point();
	test_many_wraps_preserve_order();
	test_index_wraparound_at_u32_limit();
	test_peek_does_not_consume();
	test_peek_across_the_wrap();
	test_discard_drops_without_copying();
	test_byte_counters_track_traffic();
	test_reset_empties_the_ring();
	test_zero_length_operations_are_no_ops();
	test_concurrent_producer_consumer();
	test_random_ops_match_a_fifo_model();

	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
