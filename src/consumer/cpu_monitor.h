/*
 * =====================================================================================
 *
 * Filename:  cpu_monitor.h
 *
 * Description:  Header file for the CPU_Monitor library.
 * This library provides a simple interface to monitor user- and kernel-mode
 * instructions and cycles for a specific thread.
 *
 * =====================================================================================
 */
#ifndef CPU_MONITOR_H
#define CPU_MONITOR_H

#include <optional>
#include <cstdint>
#include <stdexcept>
#include <sys/types.h>

/**
 * @brief A struct to hold the returned performance counts, split by domain.
 */
struct CPU_Counts {
    long long user_instructions;
    long long user_cycles;
    long long kernel_instructions;
    long long kernel_cycles;
    bool multiplexing_detected; // Flag to indicate if multiplexing occurred.
};

/**
 * @brief Struct to hold the full read format from a perf event fd,
 * including time values needed to detect multiplexing.
 */
struct perf_read_format {
    uint64_t value;         /* The counter value */
    uint64_t time_enabled;  /* Time the counter was enabled */
    uint64_t time_running;  /* Time the counter was running */
    uint64_t id;            /* A unique id for the counter (unused by us) */
};


/**
 * @brief A class to manage performance monitoring for a single thread.
 */
class CPU_Monitor {
public:
    /**
     * @brief Constructs a CPU_Monitor and starts monitoring the given thread ID.
     * @param tid The thread ID to monitor.
     * @throws std::runtime_error if the counters cannot be set up.
     */
    explicit CPU_Monitor(pid_t tid);

    /**
     * @brief Destructor that cleans up and closes file descriptors.
     */
    ~CPU_Monitor();

    /**
     * @brief Gets the number of events since the last call to this function.
     * @return An std::optional containing CpuCounts on success.
     * The 'multiplexing_detected' field will be true if multiplexing occurred.
     * Returns std::nullopt if the monitored thread has terminated.
     */
    std::optional<CPU_Counts> get_deltas();

    // Disable copy and assignment.
    CPU_Monitor(const CPU_Monitor&) = delete;
    CPU_Monitor& operator=(const CPU_Monitor&) = delete;

private:
    // Helper function to create and configure a single counter.
    int setup_counter(pid_t tid, uint64_t type, uint64_t config, bool kernel_only);

    int fd_user_instr = -1;
    int fd_kernel_instr = -1;
    int fd_user_cycles = -1;
    int fd_kernel_cycles = -1;

    // Store the full read format to track multiplexing times.
    perf_read_format last_user_instr = {};
    perf_read_format last_kernel_instr = {};
    perf_read_format last_user_cycles = {};
    perf_read_format last_kernel_cycles = {};
};

#endif // CPU_MONITOR_H