/*
 * =====================================================================================
 *
 * Filename:  kernel_monitor.h
 *
 * Description:  Header file for the KernelMonitor library.
 * This library provides a simple interface to monitor kernel-mode
 * performance counters for a specific thread.
 *
 * =====================================================================================
 */
#ifndef KERNEL_MONITOR_H
#define KERNEL_MONITOR_H

#include <optional>     // For std::optional, used for graceful failure
#include <cstdint>      // For fixed-width integers
#include <stdexcept>    // For std::runtime_error
#include <sys/types.h>  // For pid_t

/**
 * @brief A simple struct to hold the returned performance counts.
 */
struct KernelCounts {
    long long instructions;
    long long cache_accesses; // Combined L1 loads and stores
    long long cycles;         // **NEW**: CPU cycles in kernel mode
};

/**
 * @brief A class to manage kernel-mode performance monitoring for a single thread.
 */
class KernelMonitor {
public:
    /**
     * @brief Constructs a KernelMonitor and starts monitoring the given thread ID.
     * @param tid The thread ID to monitor.
     * @throws std::runtime_error if the counters cannot be set up (e.g., due to
     * permissions or an invalid TID).
     */
    explicit KernelMonitor(pid_t tid);

    /**
     * @brief Destructor that cleans up and closes file descriptors.
     */
    ~KernelMonitor();

    /**
     * @brief Gets the number of events since the last call to this function.
     * @return An std::optional containing KernelCounts on success.
     * Returns std::nullopt if the monitored thread has terminated.
     */
    std::optional<KernelCounts> get_deltas();

    // Disable copy and assignment to prevent issues with file descriptors.
    KernelMonitor(const KernelMonitor&) = delete;
    KernelMonitor& operator=(const KernelMonitor&) = delete;

private:
    // Helper function to create and configure a single counter.
    int setup_counter(pid_t tid, uint64_t type, uint64_t config);

    int fd_instr = -1;
    int fd_l1_loads = -1;
    int fd_l1_stores = -1;
    int fd_cycles = -1; // **NEW**

    long long last_instr = 0;
    long long last_l1_loads = 0;
    long long last_l1_stores = 0;
    long long last_cycles = 0; // **NEW**
};

#endif // KERNEL_MONITOR_H
