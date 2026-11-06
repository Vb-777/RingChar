# ringchar

**A Linux character device driver whose buffer logic you can actually test.**

`ringchar` exposes a lock-free ring buffer as `/dev/ringchar0`, with blocking and non-blocking I/O, `poll`/`epoll` readiness, an `ioctl` control interface, sysfs statistics, and a selectable overrun policy for real-time capture paths.

```console
$ sudo insmod ringchar.ko capacity=65536
$ echo "hello" > /dev/ringchar0
$ cat /dev/ringchar0
hello
$ cat /sys/class/ringchar/ringchar0/bytes
in=6 out=6
```

---

## The design decision this project is built around

The hard part of a character driver is not the `file_operations` table. It is the buffer discipline underneath it: wrap-around arithmetic, the full-versus-empty ambiguity, partial reads and writes, and the memory ordering that makes a concurrent producer and consumer safe without a lock.

Those are also the parts that are worst to debug in kernel space, where a mistake is a hung machine rather than a failed assertion.

So the buffer core lives in **`core/ring.h`, which has no kernel dependencies at all.** It compiles both in-kernel and in userspace. That single decision is what makes the rest possible:

| | with a kernel-only buffer | with a portable core |
|---|---|---|
| unit tests | none practical | **70 assertions** |
| concurrency verification | hope | **ThreadSanitizer, clean** |
| memory safety | hope | **ASAN + UBSAN, clean** |
| throughput measurement | needs a loaded module | **ordinary benchmark binary** |
| a wrap-around bug costs | a reboot | a red test |

The kernel module reduces to thin glue over an already-verified core, and the glue is the only part that needs a machine to test.

### Memory model

Single producer, single consumer, no locks. Correctness rests on three things:

- `head` is written only by the producer, `tail` only by the consumer.
- Each side reads the other's index with **acquire** semantics and publishes its own with **release** semantics, so payload writes cannot be reordered past the index update that advertises them. The same code uses `smp_load_acquire`/`smp_store_release` in-kernel and C11 `memory_order_acquire`/`release` in userspace.
- **Indices are free-running and masked only on use.** Empty is `head == tail`; full is `head - tail == capacity`. Storing masked indices instead would make those two states identical and force a wasted slot — `test_capacity_is_fully_usable` pins this down, and `test_index_wraparound_at_u32_limit` confirms the unsigned arithmetic survives the 32-bit index overflow.

Capacity is a power of two so masking replaces the modulo, which matters on the per-byte path.

---

## Measured results

### Buffer core

Single-threaded round trip, 64 KiB ring, 512 MiB per size:

| chunk | throughput | per-op |
|---|---|---|
| 1 B | 109 MB/s | 4.60 ns |
| 64 B | 6.9 GB/s | 4.67 ns |
| 1 KiB | 21.4 GB/s | 23.9 ns |
| **4 KiB** | **26.7 GB/s** | 76.8 ns |
| 64 KiB | 16.9 GB/s | 1935 ns |

Per-operation cost is flat at ~4.6 ns up to 64 bytes, which is the call and index overhead; beyond that `memcpy` bandwidth dominates and peaks at 4 KiB before falling off as the working set leaves L2.

Two-thread SPSC, 256-byte writes, 256 MiB:

| ring | throughput | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1 KiB | 982 MB/s | 133 ns | 263 ns | 351 ns | 475 ns |
| 4 KiB | **1.17 GB/s** | 121 ns | 259 ns | 424 ns | 797 ns |
| 64 KiB | 1.13 GB/s | 131 ns | 295 ns | 540 ns | 973 ns |

Percentiles are reported rather than a mean because a mean is useless for a real-time path: the figure that decides whether an audio or sensor pipeline underruns is the tail.

Overrun recovery (drop-oldest on a full ring): **54.6 M recoveries/s, 18.3 ns each.**

Measured on 2 cores. Absolute figures will differ on other hardware; the shape of the curves is the point.

### Verification

```console
$ gcc -O2 -pthread -o test_ring tests/test_ring.c && ./test_ring
70 checks, 0 failed

$ gcc -fsanitize=thread  -pthread ... && ./tr_tsan    # 70 checks, 0 failed
$ gcc -fsanitize=address,undefined -pthread ... && ./tr_asan   # 70 checks, 0 failed
```

The concurrency test pushes 4 MiB through a 4 KiB ring with two real threads and a known byte sequence, so a torn or reordered transfer is *detected* rather than merely suspected. A separate randomised test runs 20,000 mixed operations against a simple FIFO model and asserts the ring never diverges — that is what catches the interleavings hand-written cases miss.

### Kernel module

```console
$ make KDIR=/lib/modules/6.8.0-100-generic/build
  CC [M]  ringchar.o
  LD [M]  ringchar.ko          # zero warnings with -Wextra

$ make check                   # sparse static analysis
  CHECK   ringchar.c           # zero warnings
```

**Compiles clean and sparse-clean against kernel 6.8 headers.** Sparse is the only tool that verifies the `__user` pointer annotations are respected across the copy boundary; no compiler warning catches that.

**What has not been run:** the module has not been `insmod`-ed. It was built in a container whose running kernel (6.18) differs from the available headers (6.8) and which has no module-loading privileges. So there are **no measured in-kernel numbers in this README**, and the `userspace/test_device.c` harness — 15 functional tests plus a throughput measurement — is written and compiles, but its first real run is yours:

```console
cd kernel && make && sudo insmod ringchar.ko capacity=4096
cd ../userspace && make && sudo ./test_device /dev/ringchar0
```

---

## Driver design notes

**Two mutexes, not one.** The core is lock-free for exactly one producer and one consumer, but nothing stops two processes opening the node and writing concurrently. Each side gets its own mutex — `read_lock` for consumers, `write_lock` for producers — which preserves the core's SPSC precondition while still letting a reader and a writer proceed in parallel. A single device-wide mutex would serialise reader against writer and throw away the lock-free property the core provides. Both are taken with `mutex_lock_interruptible`, so a blocked process stays killable.

**Overrun policy belongs to the application.** A reliable stream wants drop-newest: a full buffer gives a short write or `EAGAIN`, and the producer retries. A real-time capture path cannot stall, and would rather lose the oldest sample than the newest — so `RINGCHAR_IOC_SET_OVERWRITE` selects drop-oldest, discarding only the shortfall rather than clearing the whole buffer. This is an `ioctl` and a sysfs attribute rather than a module parameter because the right answer depends on the reader, not on the driver.

**`poll` registers both wait queues before sampling state.** Sampling first would race: a change between the check and `poll_wait` would be missed and the caller would sleep with data available.

**EOF is defined.** With an empty ring and no writer open, `read()` returns 0 rather than blocking, which is what lets `cat /dev/ringchar0` terminate. `release` wakes both queues so a reader blocked on an empty buffer observes the last writer leaving instead of hanging forever. `poll` reports `EPOLLHUP` in the same state.

**The ioctl ABI needs no compat handler.** Every field in `struct ringchar_stats` is explicitly sized and the struct is padded to a multiple of 8, so a 32-bit process on a 64-bit kernel sees an identical layout and `compat_ptr_ioctl` suffices.

**User copies go through a per-device bounce buffer.** `copy_from_user` can fault, so it cannot be called while holding a spinlock or with the ring in an inconsistent state. One page is the staging size: large enough that syscall overhead dominates, small enough to `kmalloc` reliably.

---

## Bugs the tests and tools found

Worth listing, because they are the argument for the whole structure:

1. **`ring_init` rejected a legitimate capacity of 1** — caught by a parametrised construction test.
2. **A compiler warning for `r.head` possibly uninitialised** in the benchmark, because the `ring_init` return value was ignored. Fixed by checking it, which is correct practice anyway rather than suppressing the warning.
3. **The ioctl ABI header re-typedefed `__u32`/`__u64`**, which collides with `<linux/types.h>` — reachable transitively from `<sys/stat.h>` via `statx`. On LP64 both are 64 bits wide but they are *different types* to the compiler, so any userspace program including both failed to build. Fixed by including `<linux/types.h>` in userspace too.
4. **`write()` and `read()` return values ignored** in the device harness, tripping `-Wunused-result`. A test that silently ignores a short write masks exactly the bug it exists to find, so they are now routed through checked wrappers.

---

## Build and use

```bash
# buffer core: tests and benchmark, no kernel needed
gcc -O2 -Wall -Wextra -pthread -o test_ring  tests/test_ring.c  && ./test_ring
gcc -O2 -Wall -Wextra -pthread -o bench_ring bench/bench_ring.c && ./bench_ring

# kernel module
cd kernel
make                      # or: make KDIR=/lib/modules/<version>/build
make check                # sparse
sudo insmod ringchar.ko capacity=65536 n_devices=1
sudo rmmod ringchar

# cross-compile for a target board
make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

Module parameters: `capacity` (bytes, rounded up to a power of two) and `n_devices` (number of `/dev/ringchar*` nodes).

## Layout

```
core/ring.h              portable lock-free SPSC ring: the tested core
kernel/ringchar.c        file_operations, ioctl, poll, sysfs, lifetime
kernel/ringchar_ioctl.h  userspace ABI, shared verbatim with the module
kernel/Makefile          kbuild, sparse target, cross-compile support
tests/test_ring.c        70 assertions: wrap-around, concurrency, FIFO model
bench/bench_ring.c       throughput and latency percentiles
userspace/test_device.c  15 device tests: ioctl, poll/epoll, blocking, EOF
```

## License

GPL-2.0 (kernel module), as required for `MODULE_LICENSE("GPL v2")`.
