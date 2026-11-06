/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bench_ring.c - Throughput and latency of the driver's buffer core.
 *
 * Two things are measured, because they answer different questions:
 *
 *   1. Single-threaded throughput vs. transfer size. This isolates the cost of
 *      the split-copy arithmetic from any thread interaction, and shows where
 *      per-call overhead stops dominating.
 *
 *   2. Two-thread throughput and per-operation latency percentiles. A mean
 *      alone is useless for a real-time path: the number that decides whether
 *      an audio or sensor pipeline underruns is the tail, so p99 and p99.9 are
 *      reported alongside the median.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/ring.h"

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static u64 now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return (x > y) - (x < y);
}

/* ------------------------------------------------------------------ */

static void bench_single_thread(void)
{
	const u32 cap = 1u << 16;          /* 64 KiB */
	const u64 total = 512ull << 20;    /* 512 MiB per size */
	u8 *storage = malloc(cap);
	u8 *payload = malloc(65536);
	u8 *sink = malloc(65536);
	const u32 sizes[] = { 1, 8, 64, 256, 1024, 4096, 16384, 65536 };
	size_t i;

	memset(payload, 0xa5, 65536);
	printf("Single-threaded round-trip (64 KiB ring, 512 MiB per size)\n");
	printf("  %10s %12s %14s\n", "chunk", "MB/s", "ns/op");

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		u32 chunk = sizes[i];
		struct ring r;
		u64 moved = 0, ops = 0;
		double t0, dt;

		if (ring_init(&r, storage, cap) != 0) {
			fprintf(stderr, "ring_init failed\n");
			continue;
		}
		t0 = now_sec();
		while (moved < total) {
			/* Fill then drain, so both split-copy paths are hit
			 * and the ring genuinely wraps. */
			u32 n;

			while ((n = ring_write(&r, payload, chunk)) > 0) {
				moved += n;
				ops++;
				if (ring_full(&r))
					break;
			}
			while (ring_read(&r, sink, chunk) > 0)
				ops++;
		}
		dt = now_sec() - t0;
		printf("  %10u %12.1f %14.2f\n", chunk,
		       moved / dt / 1e6, dt * 1e9 / (double)ops);
	}
	free(storage);
	free(payload);
	free(sink);
}

/* ------------------------------------------------------------------ */

#define LAT_SAMPLES 200000u

struct prod_args {
	struct ring *r;
	u64 total;
	u32 chunk;
	u64 *lat;          /* per-write latency samples, may be NULL */
	u32 lat_n;
};

static void *bench_producer(void *arg)
{
	struct prod_args *a = arg;
	u8 *chunk = malloc(a->chunk);
	u64 sent = 0;
	u32 sample = 0;

	memset(chunk, 0x5a, a->chunk);
	while (sent < a->total) {
		u64 t0 = a->lat ? now_ns() : 0;
		u32 n = ring_write(a->r, chunk, a->chunk);

		if (n > 0) {
			if (a->lat && sample < a->lat_n)
				a->lat[sample++] = now_ns() - t0;
			sent += n;
		}
	}
	a->lat_n = sample;
	free(chunk);
	return NULL;
}

struct cons_args {
	struct ring *r;
	u64 total;
	u32 chunk;
};

static void *bench_consumer(void *arg)
{
	struct cons_args *a = arg;
	u8 *chunk = malloc(a->chunk);
	u64 got = 0;

	while (got < a->total)
		got += ring_read(a->r, chunk, a->chunk);
	free(chunk);
	return NULL;
}

static void bench_two_thread(void)
{
	const u32 caps[] = { 1024, 4096, 65536 };
	const u32 chunk = 256;
	const u64 total = 256ull << 20;   /* 256 MiB */
	u64 *lat = malloc(LAT_SAMPLES * sizeof(u64));
	size_t i;

	printf("\nTwo-thread SPSC (256-byte writes, 256 MiB)\n");
	printf("  %10s %12s %10s %10s %10s %10s\n",
	       "ring", "MB/s", "p50 ns", "p90 ns", "p99 ns", "p99.9 ns");

	for (i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
		u32 cap = caps[i];
		u8 *storage = malloc(cap);
		struct ring r;
		struct prod_args pa;
		struct cons_args ca;
		pthread_t prod, cons;
		double t0, dt;

		if (ring_init(&r, storage, cap) != 0) {
			fprintf(stderr, "ring_init failed\n");
			free(storage);
			continue;
		}
		pa.r = &r; pa.total = total; pa.chunk = chunk;
		pa.lat = lat; pa.lat_n = LAT_SAMPLES;
		ca.r = &r; ca.total = total; ca.chunk = chunk;

		t0 = now_sec();
		pthread_create(&prod, NULL, bench_producer, &pa);
		pthread_create(&cons, NULL, bench_consumer, &ca);
		pthread_join(prod, NULL);
		pthread_join(cons, NULL);
		dt = now_sec() - t0;

		qsort(lat, pa.lat_n, sizeof(u64), cmp_u64);
		printf("  %10u %12.1f %10llu %10llu %10llu %10llu\n",
		       cap, total / dt / 1e6,
		       (unsigned long long)lat[(u32)(pa.lat_n * 0.50)],
		       (unsigned long long)lat[(u32)(pa.lat_n * 0.90)],
		       (unsigned long long)lat[(u32)(pa.lat_n * 0.99)],
		       (unsigned long long)lat[(u32)(pa.lat_n * 0.999)]);
		free(storage);
	}
	free(lat);
}

/* ------------------------------------------------------------------ */

static void bench_overrun_behaviour(void)
{
	/*
	 * A real-time capture path cannot block, so when the consumer falls
	 * behind the driver must choose: refuse the write, or drop the oldest
	 * data and keep the newest. This measures the cost of the
	 * drop-oldest recovery path (discard then write), which is what the
	 * module's RINGCHAR_IOC_SET_OVERWRITE mode uses.
	 */
	const u32 cap = 4096;
	u8 *storage = malloc(cap);
	u8 payload[256];
	struct ring r;
	u64 ops = 0;
	double t0, dt;

	memset(payload, 0x11, sizeof(payload));
	if (ring_init(&r, storage, cap) != 0) {
		fprintf(stderr, "ring_init failed\n");
		free(storage);
		return;
	}
	/* Deliberately fill it and never read: every write must recover. */
	while (ring_write(&r, payload, sizeof(payload)) > 0)
		;

	printf("\nOverrun recovery (drop-oldest on a full ring)\n");
	t0 = now_sec();
	while (ops < 2000000) {
		ring_discard(&r, sizeof(payload));
		ring_write(&r, payload, sizeof(payload));
		ops++;
	}
	dt = now_sec() - t0;
	printf("  %.1f M recoveries/s, %.1f ns each\n",
	       ops / dt / 1e6, dt * 1e9 / (double)ops);
	free(storage);
}

int main(void)
{
	bench_single_thread();
	bench_two_thread();
	bench_overrun_behaviour();
	return 0;
}
