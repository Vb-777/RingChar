# Running ringchar on your machine

The kernel module compiles and is sparse-clean, but has NOT been loaded — the
build container had no module-loading privileges and its running kernel did not
match the available headers. These are the steps that produce the in-kernel
numbers the README leaves blank.

## 1. Buffer core (no kernel needed, works anywhere)

    gcc -O2 -Wall -Wextra -pthread -o test_ring  tests/test_ring.c  && ./test_ring
    gcc -O2 -Wall -Wextra -pthread -o bench_ring bench/bench_ring.c && ./bench_ring

    # memory-ordering and memory-safety verification
    gcc -O1 -g -fsanitize=thread            -pthread -o tr_tsan tests/test_ring.c && ./tr_tsan
    gcc -O1 -g -fsanitize=address,undefined -pthread -o tr_asan tests/test_ring.c && ./tr_asan

## 2. Kernel module (needs Linux + headers)

    sudo apt install linux-headers-$(uname -r) build-essential sparse
    cd kernel
    make
    make check                 # sparse: verifies __user annotations
    sudo insmod ringchar.ko capacity=4096
    ls -l /dev/ringchar0
    cat /sys/class/ringchar/ringchar0/capacity

## 3. Device test harness

    cd ../userspace
    gcc -O2 -Wall -Wextra -pthread -o test_device test_device.c
    sudo ./test_device /dev/ringchar0

Expect 15 functional tests plus a throughput figure. Put that figure in the
README where it currently says the module has not been run.

## 4. Quick manual check

    echo "hello" | sudo tee /dev/ringchar0
    sudo cat /dev/ringchar0
    cat /sys/class/ringchar/ringchar0/bytes
    sudo rmmod ringchar

## Notes

- WSL2 works, but needs headers matching the WSL kernel; `uname -r` will show
  something like 5.15.x-microsoft-standard-WSL2. If no matching headers package
  exists, use a VM instead.
- `make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-`
  cross-builds for a target board.
