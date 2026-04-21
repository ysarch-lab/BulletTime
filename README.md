## BulletTime Overview

BulletTime is a memory-reference tracing framework built on Intel Pin. It preserves the behavior of the traced application relative to untraced execution by *time-dilating* faster threads to match the slowest thread's I/O-induced delay, and by extending kernel-daemon sleep timers so system threads keep pace as well.

The repository contains three components:

- **pintool** — a Pin client that instruments memory loads/stores and hands per-thread buffers off to the consumer.
- **consumer** — a userspace process that persists trace data to disk (optionally zstd-compressed), tracks per-thread progress, and computes the delays injected into application and kernel threads.
- **kernel_dilation** — a small Linux kernel module that kprobes `schedule_timeout` and `hrtimer_nanosleep` to stretch sleeps by a sysfs-controlled factor.

## Repository Layout

```
BulletTime/
├── setup_pintool.sh           downloads Pin, builds pintool + consumer
├── setup_sleep_dilation.sh    builds and insmods the kernel module
├── pin/                       populated by setup_pintool.sh (not tracked)
└── src/
    ├── include/
    │   └── consumer.h         shared IPC + sysfs definitions
    ├── pintool/               Pin tool sources + makefile
    ├── consumer/              consumer process sources + Makefile
    └── kernel_dilation/       sleep_dilation.c + kbuild Makefile
```

## Requirements

- Linux x86_64, kernel ≥ 2.6.9 (for kprobes). Tested on 6.13.
- `gcc`/`g++` with C++17, `make`, `curl`, `tar`
- Kernel headers for the running kernel: `kernel-devel` on Fedora/RHEL, `linux-headers-$(uname -r)` on Debian/Ubuntu
- `libzstd` development headers: `libzstd-devel` on Fedora/RHEL, `libzstd-dev` on Debian/Ubuntu
- Root privileges for `insmod`/`rmmod` and for the hugetlbfs allocation below

Pin itself is fetched automatically by [setup_pintool.sh](setup_pintool.sh).

## Quick Start

Build everything:

```sh
./setup_pintool.sh           # downloads Pin 3.30 into ./pin/, builds pintool + consumer
./setup_sleep_dilation.sh    # builds the kernel module and insmods it
```

Reserve hugepages for the trace buffers. BulletTime allocates its internal buffers from `hugetlbfs`, so 2 MB hugepages must be reserved in advance — the allocator will fail if they are not available at runtime. A reasonable default (2 GB = 1024 pages) is:

```sh
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
cat /proc/meminfo | grep HugePages_Free   # sanity check
```

On most distributions `/dev/hugepages` is already mounted; verify with `mount | grep hugetlbfs`, and if needed:

```sh
sudo mount -t hugetlbfs none /dev/hugepages
```

The exact number of pages to reserve scales with the number of traced threads and the configured buffer size (see `CONSUMER_BUFFER_SIZE` in [src/include/consumer.h](src/include/consumer.h)).

Run a trace with [collect_trace.sh](collect_trace.sh), which orchestrates the consumer and the pintool:

```sh
./collect_trace.sh --help                          # full option list
./collect_trace.sh -o /tmp/my_trace -- ./my_app    # trace ./my_app; outputs in /tmp/my_trace
```

## Citation

```bibtex
@inproceedings{wu2026bullettime,
  title     = {BulletTime: Time Dilation for High-Fidelity Tracing},
  author    = {Wu, Michael and Isaacman, Sibren and Bhattacharjee, Abhishek and Khandelwal, Anurag},
  booktitle = {Proceedings of the 53rd Annual International Symposium on Computer Architecture (ISCA)},
  year      = {2026}
}
```
