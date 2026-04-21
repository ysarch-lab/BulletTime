// Buffer size information shared between BulletTime and the consumer at compile time
#ifndef CONSUMER_H
#define CONSUMER_H

// Shared-memory buffer size (1 MB per buffer)
#define CONSUMER_BUFFER_SIZE (size_t) 1 * 1024 * 1024
#define CONSUMER_BUFFER_PAGES std::to_string(CONSUMER_BUFFER_SIZE / 4096)
#define CONSUMER_BUFFER_MIN (size_t) 2 * 1024 * 1024 // Must allocate at least 2MB for hugepages

// Effective shared-memory allocation (at least CONSUMER_BUFFER_MIN for hugepage alignment)
#define ALLOCATION_SIZE std::max(CONSUMER_BUFFER_SIZE, CONSUMER_BUFFER_MIN)

// ZSTD output buffer: match input size so compressed data is flushed to disk on every buffer
#define ZSTD_BUFFER_SIZE ALLOCATION_SIZE

// Control thread period (consumer side)
unsigned long CONTROL_SLEEP_SEC = 30;

// Sleep-dilation kernel module sysfs knob
#define DILATION_KNOB "/sys/kernel/sleep_dilation/dilation_factor"

// Approximate kernel-mode instructions incurred by the pin tool per buffer handoff;
// subtracted from measured kernel instructions when computing per-thread progress.
#define WORK_OFFSET 60000

#endif
