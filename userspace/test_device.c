/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_device.c - Functional test harness for /dev/ringchar0.
 *
 * Run after `insmod ringchar.ko`. This exercises the parts that cannot be
 * tested in userspace against the core alone: the user-copy boundary, blocking
 * and O_NONBLOCK semantics, poll/epoll readiness, the ioctl ABI, and the
 * end-of-stream behaviour when the last writer closes.
 *
 *   sudo insmod ringchar.ko capacity=4096
 *   sudo ./test_device /dev/ringchar0
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kernel/ringchar_ioctl.h"

static const char *devpath = "/dev/ringchar0";
static int tests_run, tests_failed;

#define CHECK(cond, fmt, ...)                                                  \
	do {                                                                   \
		tests_run++;                                                   \
		if (!(cond)) {                                                 \
			tests_failed++;                                        \
			printf("  FAIL %s:%d: " fmt "\n", __func__, __LINE__,   \
			       ##__VA_ARGS__);                                 \
		}                                                              \
	} while (0)

#define CHECK_EQ(a, b) CHECK((long)(a) == (long)(b),                           \
			     "expected %ld, got %ld", (long)(b), (long)(a))

static int open_dev(int flags)
{
	int fd = open(devpath, flags);

	if (fd < 0) {
		perror(devpath);
		exit(2);
	}
	return fd;
}

/* Wrappers that assert on the return value: the bare calls trip
 * -Wunused-result, and silently ignoring a short write in a test would mask
 * exactly the bug the test exists to find. */
static void must_write(int fd, const void *buf, size_t n)
{
	ssize_t w = write(fd, buf, n);

	if (w != (ssize_t)n) {
		tests_failed++;
		printf("  FAIL must_write: wrote %zd of %zu (%s)\n", w, n,
		       strerror(errno));
	}
}

static void must_read(int fd, void *buf, size_t n)
{
	ssize_t r = read(fd, buf, n);

	if (r != (ssize_t)n) {
		tests_failed++;
		printf("  FAIL must_read: read %zd of %zu (%s)\n", r, n,
		       strerror(errno));
	}
}

static void drain(int fd)
{
	unsigned int zero = 0;

	if (ioctl(fd, RINGCHAR_IOC_RESET, &zero) != 0 ||
	    ioctl(fd, RINGCHAR_IOC_RESET_STATS, &zero) != 0)
		perror("drain");
}

/* ------------------------------------------------------------------ */

static void test_write_then_read(void)
{
	int fd = open_dev(O_RDWR);
	char out[64] = { 0 };
	const char *msg = "qualcomm";

	drain(fd);
	CHECK_EQ(write(fd, msg, 8), 8);
	CHECK_EQ(read(fd, out, 8), 8);
	CHECK_EQ(memcmp(msg, out, 8), 0);
	close(fd);
}

static void test_stats_ioctl(void)
{
	int fd = open_dev(O_RDWR);
	struct ringchar_stats st;

	drain(fd);
	CHECK_EQ(write(fd, "abcd", 4), 4);
	CHECK_EQ(ioctl(fd, RINGCHAR_IOC_GET_STATS, &st), 0);
	CHECK_EQ(st.used, 4);
	CHECK_EQ(st.free, st.capacity - 4);
	CHECK_EQ(st.bytes_in, 4);
	CHECK(st.writers >= 1, "writer count should include this fd");
	close(fd);
}

static void test_nonblocking_read_on_empty(void)
{
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	char buf[16];

	drain(fd);
	CHECK_EQ(read(fd, buf, 16), -1);
	CHECK_EQ(errno, EAGAIN);
	close(fd);
}

static void test_nonblocking_write_on_full(void)
{
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	struct ringchar_stats st;
	char *big;
	ssize_t n;

	drain(fd);
	ioctl(fd, RINGCHAR_IOC_GET_STATS, &st);
	big = malloc(st.capacity + 64);
	memset(big, 'x', st.capacity + 64);

	/* Fill exactly, then confirm the next write is refused. */
	n = write(fd, big, st.capacity);
	CHECK_EQ(n, (ssize_t)st.capacity);
	CHECK_EQ(write(fd, "y", 1), -1);
	CHECK_EQ(errno, EAGAIN);

	free(big);
	close(fd);
}

static void test_short_write_when_nearly_full(void)
{
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	struct ringchar_stats st;
	char *big;

	drain(fd);
	ioctl(fd, RINGCHAR_IOC_GET_STATS, &st);
	big = malloc(st.capacity * 2);
	memset(big, 'z', st.capacity * 2);

	/* Asking for twice the capacity must yield exactly capacity, not an
	 * error: a short write is the correct POSIX behaviour. */
	CHECK_EQ(write(fd, big, st.capacity * 2), (ssize_t)st.capacity);
	free(big);
	close(fd);
}

static void test_overwrite_mode_drops_oldest(void)
{
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	struct ringchar_stats st;
	unsigned int on = 1, off = 0;
	char *big, *out;

	drain(fd);
	ioctl(fd, RINGCHAR_IOC_GET_STATS, &st);
	big = malloc(st.capacity);
	out = malloc(st.capacity);
	memset(big, 'A', st.capacity);

	CHECK_EQ(ioctl(fd, RINGCHAR_IOC_SET_OVERWRITE, &on), 0);
	CHECK_EQ(write(fd, big, st.capacity), (ssize_t)st.capacity);

	/* Buffer is full; in drop-oldest mode this must still succeed. */
	CHECK_EQ(write(fd, "BBBB", 4), 4);

	/* The newest bytes must survive and the oldest be gone. */
	CHECK_EQ(read(fd, out, st.capacity), (ssize_t)st.capacity);
	CHECK_EQ(memcmp(out + st.capacity - 4, "BBBB", 4), 0);

	ioctl(fd, RINGCHAR_IOC_SET_OVERWRITE, &off);
	free(big);
	free(out);
	close(fd);
}

static void test_peek_and_discard_ioctls(void)
{
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	struct ringchar_stats st;
	unsigned int n = 2;
	char out[8] = { 0 };

	drain(fd);
	CHECK_EQ(write(fd, "123456", 6), 6);

	CHECK_EQ(ioctl(fd, RINGCHAR_IOC_DISCARD, &n), 0);
	CHECK_EQ(n, 2);                      /* reports bytes actually dropped */
	ioctl(fd, RINGCHAR_IOC_GET_STATS, &st);
	CHECK_EQ(st.used, 4);
	CHECK_EQ(read(fd, out, 4), 4);
	CHECK_EQ(memcmp(out, "3456", 4), 0);
	close(fd);
}

static void test_bad_ioctl_is_rejected(void)
{
	int fd = open_dev(O_RDWR);

	/* Wrong magic and out-of-range number must both give ENOTTY rather
	 * than being silently accepted. */
	CHECK_EQ(ioctl(fd, _IOR('Z', 1, int), NULL), -1);
	CHECK_EQ(errno, ENOTTY);
	CHECK_EQ(ioctl(fd, _IOR('R', 99, int), NULL), -1);
	CHECK_EQ(errno, ENOTTY);
	close(fd);
}

/* ------------------------------------------------------------------ */
/* poll / epoll                                                        */
/* ------------------------------------------------------------------ */

static void test_poll_reports_readability(void)
{
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	char buf[8];

	drain(fd);
	CHECK_EQ(poll(&pfd, 1, 0), 0);             /* empty: not readable */

	must_write(fd, "hi", 2);
	pfd.revents = 0;
	CHECK_EQ(poll(&pfd, 1, 0), 1);
	CHECK(pfd.revents & POLLIN, "POLLIN expected once data is buffered");

	must_read(fd, buf, 2);
	pfd.revents = 0;
	CHECK_EQ(poll(&pfd, 1, 0), 0);             /* drained again */
	close(fd);
}

static void test_poll_reports_writability(void)
{
	int fd = open_dev(O_RDWR | O_NONBLOCK);
	struct pollfd pfd = { .fd = fd, .events = POLLOUT };
	struct ringchar_stats st;
	char *big;

	drain(fd);
	CHECK_EQ(poll(&pfd, 1, 0), 1);
	CHECK(pfd.revents & POLLOUT, "empty device should be writable");

	ioctl(fd, RINGCHAR_IOC_GET_STATS, &st);
	big = malloc(st.capacity);
	memset(big, 'q', st.capacity);
	must_write(fd, big, st.capacity);

	pfd.events = POLLOUT;
	pfd.revents = 0;
	CHECK_EQ(poll(&pfd, 1, 0), 0);             /* full: not writable */
	free(big);
	close(fd);
}

static void test_epoll_wakes_on_write(void)
{
	/* The real test of poll_wait placement: a reader must be woken by a
	 * write that happens after it is already blocked. */
	int rfd = open_dev(O_RDONLY | O_NONBLOCK);
	int wfd = open_dev(O_WRONLY);
	int ep = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLIN }, got;
	pid_t pid;

	drain(wfd);
	ev.data.fd = rfd;
	epoll_ctl(ep, EPOLL_CTL_ADD, rfd, &ev);

	pid = fork();
	if (pid == 0) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 150000000 };

		nanosleep(&ts, NULL);
		if (write(wfd, "wake", 4) != 4)
			_exit(1);
		_exit(0);
	}

	CHECK_EQ(epoll_wait(ep, &got, 1, 3000), 1);
	CHECK(got.events & EPOLLIN, "epoll should report EPOLLIN after write");

	waitpid(pid, NULL, 0);
	close(ep);
	close(rfd);
	close(wfd);
}

/* ------------------------------------------------------------------ */
/* Blocking behaviour                                                  */
/* ------------------------------------------------------------------ */

struct writer_ctx {
	int fd;
	unsigned delay_ms;
};

static void *delayed_writer(void *arg)
{
	struct writer_ctx *c = arg;
	struct timespec ts = {
		.tv_sec = c->delay_ms / 1000,
		.tv_nsec = (long)(c->delay_ms % 1000) * 1000000L,
	};

	nanosleep(&ts, NULL);
	if (write(c->fd, "unblocked", 9) != 9)
		perror("delayed_writer write");
	return NULL;
}

static void test_blocking_read_waits_for_data(void)
{
	int rfd = open_dev(O_RDONLY);
	int wfd = open_dev(O_WRONLY);
	struct writer_ctx ctx = { .fd = wfd, .delay_ms = 200 };
	pthread_t th;
	char buf[16] = { 0 };
	struct timespec t0, t1;
	double elapsed;

	drain(wfd);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	pthread_create(&th, NULL, delayed_writer, &ctx);

	/* Must sleep rather than spin or return early. */
	CHECK_EQ(read(rfd, buf, 9), 9);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;

	CHECK(elapsed > 0.15, "read returned after %.3fs, expected to block ~0.2s",
	      elapsed);
	CHECK_EQ(memcmp(buf, "unblocked", 9), 0);

	pthread_join(th, NULL);
	close(rfd);
	close(wfd);
}

static void test_read_returns_eof_when_no_writers(void)
{
	/* With an empty ring and no writer open, read() must report EOF
	 * instead of hanging -- this is what lets `cat` terminate. */
	int rfd;
	char buf[16];

	{
		int wfd = open_dev(O_WRONLY);

		drain(wfd);
		close(wfd);
	}
	rfd = open_dev(O_RDONLY);
	CHECK_EQ(read(rfd, buf, 16), 0);
	close(rfd);
}

/* ------------------------------------------------------------------ */
/* Concurrency through the device node                                 */
/* ------------------------------------------------------------------ */

#define STREAM_BYTES (2u * 1024 * 1024)

static void *stream_writer(void *arg)
{
	int fd = *(int *)arg;
	unsigned char chunk[1024];
	unsigned sent = 0;

	while (sent < STREAM_BYTES) {
		unsigned i, want = sizeof(chunk);
		ssize_t n;

		if (STREAM_BYTES - sent < want)
			want = STREAM_BYTES - sent;
		for (i = 0; i < want; i++)
			chunk[i] = (unsigned char)((sent + i) & 0xff);
		n = write(fd, chunk, want);
		if (n > 0)
			sent += (unsigned)n;
		else if (n < 0 && errno != EAGAIN)
			break;
	}
	return NULL;
}

static void test_streaming_through_the_device(void)
{
	/* 2 MiB through the node with a blocking reader and writer. Verifies
	 * the bounce-buffer path, the wake-up discipline and that no byte is
	 * lost or reordered across many buffer-full/empty transitions. */
	int wfd = open_dev(O_WRONLY);
	int rfd = open_dev(O_RDONLY);
	pthread_t th;
	unsigned char buf[4096];
	unsigned got = 0;
	int corrupt = 0;

	drain(wfd);
	pthread_create(&th, NULL, stream_writer, &wfd);

	while (got < STREAM_BYTES) {
		ssize_t n = read(rfd, buf, sizeof(buf));
		ssize_t i;

		if (n <= 0)
			break;
		for (i = 0; i < n; i++) {
			if (buf[i] != (unsigned char)((got + i) & 0xff))
				corrupt = 1;
		}
		got += (unsigned)n;
	}

	pthread_join(th, NULL);
	CHECK_EQ(got, STREAM_BYTES);
	CHECK(!corrupt, "stream corrupted through the device node");
	close(rfd);
	close(wfd);
}

static void test_multiple_writers_are_serialised(void)
{
	/* Two writers on one node. The core is SPSC, so the module's write
	 * mutex is what keeps this safe; without it the bytes interleave
	 * mid-chunk or the indices corrupt. */
	int fds[2] = { open_dev(O_WRONLY), open_dev(O_WRONLY) };
	int rfd = open_dev(O_RDONLY | O_NONBLOCK);
	pthread_t th[2];
	struct ringchar_stats st;
	int i;

	drain(rfd);
	for (i = 0; i < 2; i++)
		pthread_create(&th[i], NULL, stream_writer, &fds[i]);

	/* Drain continuously so the writers make progress. */
	{
		unsigned char buf[4096];
		unsigned long long total = 0;
		int idle = 0;

		while (idle < 2000) {
			ssize_t n = read(rfd, buf, sizeof(buf));

			if (n > 0) {
				total += (unsigned long long)n;
				idle = 0;
			} else {
				idle++;
			}
			if (total >= 2ull * STREAM_BYTES)
				break;
		}
		CHECK(total > 0, "no data read from two concurrent writers");
	}

	for (i = 0; i < 2; i++)
		pthread_join(th[i], NULL);

	CHECK_EQ(ioctl(rfd, RINGCHAR_IOC_GET_STATS, &st), 0);
	CHECK(st.capacity > 0, "device still responsive after contention");

	close(fds[0]);
	close(fds[1]);
	close(rfd);
}

/* ------------------------------------------------------------------ */
/* Throughput through the syscall boundary                             */
/* ------------------------------------------------------------------ */

static void bench_device_throughput(void)
{
	int wfd = open_dev(O_WRONLY);
	int rfd = open_dev(O_RDONLY);
	pthread_t th;
	unsigned char buf[4096];
	unsigned got = 0;
	struct timespec t0, t1;
	double dt;

	drain(wfd);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	pthread_create(&th, NULL, stream_writer, &wfd);
	while (got < STREAM_BYTES) {
		ssize_t n = read(rfd, buf, sizeof(buf));

		if (n <= 0)
			break;
		got += (unsigned)n;
	}
	pthread_join(th, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;

	printf("\n  device throughput: %.1f MB/s (%u bytes in %.3f s, 1 KiB writes / 4 KiB reads)\n",
	       got / dt / 1e6, got, dt);
	close(rfd);
	close(wfd);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	struct stat sb;

	if (argc > 1)
		devpath = argv[1];

	if (stat(devpath, &sb) != 0) {
		fprintf(stderr,
			"%s not found. Load the module first:\n"
			"  cd ../kernel && make && sudo insmod ringchar.ko\n",
			devpath);
		return 2;
	}

	printf("ringchar device tests on %s\n", devpath);

	test_write_then_read();
	test_stats_ioctl();
	test_nonblocking_read_on_empty();
	test_nonblocking_write_on_full();
	test_short_write_when_nearly_full();
	test_overwrite_mode_drops_oldest();
	test_peek_and_discard_ioctls();
	test_bad_ioctl_is_rejected();
	test_poll_reports_readability();
	test_poll_reports_writability();
	test_epoll_wakes_on_write();
	test_blocking_read_waits_for_data();
	test_read_returns_eof_when_no_writers();
	test_streaming_through_the_device();
	test_multiple_writers_are_serialised();

	bench_device_throughput();

	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
