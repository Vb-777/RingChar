# Qualcomm SW Engineer — two new project entries

Both in your IITH template (title → GitHub link → bullets → `Tech:` last), bullet lengths matched to your existing entries.

---

## Entry 1 — the Linux driver

> **• ringchar: Linux Character Device Driver with a Testable Lock-Free Core** `[Personal Project : Nov 2026]`
> `[ github.com/Vb-777/ringchar ]`
> – Built a char driver (blocking/non-blocking I/O, poll/epoll, ioctl, sysfs) over a lock-free SPSC ring buffer
> – Kept the buffer core kernel-agnostic so it unit-tests in userspace: 70 assertions, clean under ThreadSanitizer and ASAN/UBSAN
> – Core hits 26.7 GB/s single-threaded and 1.17 GB/s two-thread SPSC at p99 424 ns; 18.3 ns drop-oldest overrun recovery
> – Module builds warning-free and sparse-clean against kernel 6.8; 15-test device harness for ioctl, epoll and EOF paths
> **Tech:** C, Linux kernel 6.8 (kbuild, cdev, sysfs, waitqueues), sparse, pthreads, TSan/ASan, GCC

**Three-bullet version** (drop bullet 4):
> – Built a char driver (blocking/non-blocking I/O, poll/epoll, ioctl, sysfs) over a lock-free SPSC ring buffer
> – Kernel-agnostic buffer core unit-tests in userspace: 70 assertions, clean under ThreadSanitizer and ASAN/UBSAN
> – 26.7 GB/s single-threaded, 1.17 GB/s two-thread SPSC at p99 424 ns; builds sparse-clean against kernel 6.8

---

## Entry 2 — the codec and protocol stack

> **• rtpaudio: Real-Time ADPCM Codec and RTP/UDP Streaming Stack** `[Personal Project : Nov 2026]`
> `[ github.com/Vb-777/rtpaudio ]`
> – Wrote an IMA ADPCM codec from scratch (exact 4:1, 32.2 dB SNR) and packetised it into RFC 3550 RTP over UDP
> – Carrying codec state in a 4-byte payload preamble beat per-packet reset by 14 to 22 dB for 9.1% header overhead
> – Jitter buffer with modular sequence handling, loss detection and 3 concealment modes; depth N+1 absorbs reorder distance N
> – Measured 123K pkt/s, p99 29 us loopback latency; 93 assertions asserting exact RTP wire bytes, clean under ASAN/UBSAN
> **Tech:** C, UDP sockets, RFC 3550 RTP, ADPCM, POSIX threads, clock_nanosleep, ASan/UBSan, GNU Make

**Three-bullet version** (drop bullet 3):
> – Wrote an IMA ADPCM codec from scratch (exact 4:1, 32.2 dB SNR), packetised into RFC 3550 RTP over UDP
> – Carrying codec state in a 4-byte preamble beat per-packet reset by 14 to 22 dB for 9.1% header overhead
> – Jitter buffer (reordering, loss concealment): 123K pkt/s, p99 29 us; 93 assertions on exact RTP wire bytes

---

## What to remove to make room

Your current Qualcomm CV has five projects, three of which are clustering work. Drop these two:

- **Parallelised Sequential k-means++ Clustering**
- **Adaptive Distributed K-means Clustering (C & DC)**

They're solid, but together with the PDW project they demonstrate the same skill three times, and neither touches drivers, kernel, multimedia or protocols. Keep **PDW Emitter Deinterleaving** as your research credential and **Concurrency Control (MVCC-SV & SSI)** — that one earns its place, since OS concepts and data structures are explicitly named in the JD.

## Resulting project order

1. **ringchar** — device drivers, Linux kernel, real-time, C
2. **rtpaudio** — multimedia codecs, TCP/UDP/RTP protocol stack, real-time
3. **Concurrency Control (MVCC-SV & SSI)** — OS concepts, data structures, C++
4. **AI/ML-based PDW Emitter Deinterleaving on FPGA** — embedded, HPC, research
5. **SarDet** — imaging, if you keep a fifth

That order front-loads the three JD areas you previously had nothing for.

## JD coverage, before and after

| JD requirement | times named | before | after |
|---|---|---|---|
| Device drivers / Linux kernel / BSP | 3× | none | **ringchar** |
| Multimedia: audio/video codecs | 3× | SarDet (detection only) | **rtpaudio** |
| Protocols TCP/UDP/IP/SIP/RTP | 2× | none | **rtpaudio** |
| Real-time embedded software | 2× | PDW (partial) | **both** |
| C/C++, OOP, OS concepts, data structures | 2× | strong | strong |

---

## Interview preparation — the honest parts are your best material

Both READMEs document bugs the tests and benchmarks found, and in an interview those are worth more than the clean results.

**For ringchar**, lead with *why the buffer core has no kernel headers*. It's a 20-second explanation with an obvious payoff: a wrap-around bug becomes a red test instead of a reboot. Be ready on free-running versus masked indices (masked indices waste a slot and make full indistinguishable from empty), on acquire/release ordering, and on why there are two mutexes rather than one.

**For rtpaudio**, lead with the ADPCM state problem — a lost packet desynchronises the decoder and corrupts everything after it — and the 14–22 dB measurement that settled the design. Then, if the conversation allows, raise the concealment result: silence measured better than fade on both of my metrics, which contradicts my own default, and I can tell you exactly why my benchmark can't settle it. Saying that out loud demonstrates more engineering judgement than any passing test.

Also be ready for: why SNR is corpus-level, why RTP timestamps count samples not packets, and why 16-bit sequence wraparound breaks a naive comparison after 22 minutes.

## Two things to do before you push

1. **Run `sudo insmod` and the device harness on real Linux or WSL.** The module compiles clean and sparse-clean against 6.8 headers, but I could not load it — the container had no module-loading privileges and a mismatched running kernel. The README says so explicitly. Run it, then replace that paragraph with your measured numbers; until you do, don't claim in an interview that you've run it in-kernel.
2. **Commit both with dates in Nov 2026** so the repo history matches the CV.
