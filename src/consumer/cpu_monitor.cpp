/*
 * =====================================================================================
 *
 * Filename:  cpu_monitor.cpp
 *
 * Description:  Implementation of the CPU_Monitor library.
 *
 * =====================================================================================
 */
#include "cpu_monitor.h"
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <cerrno>
#include <string>
#include <iostream>

// Syscall wrapper
static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Private helper to set up one counter
int CPU_Monitor::setup_counter(pid_t tid, uint64_t type, uint64_t config, bool kernel_only) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = type;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_hv = 1;
    
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    if (kernel_only) {
        pe.exclude_user = 1;
    } else {
        pe.exclude_kernel = 1;
    }

    int fd = perf_event_open(&pe, tid, -1, -1, 0);
    if (fd == -1) {
        throw std::runtime_error("perf_event_open failed: " + std::string(strerror(errno)));
    }
    return fd;
}

// Constructor
CPU_Monitor::CPU_Monitor(pid_t tid) {
    try {
        // Setup the four required counters
        fd_user_instr = setup_counter(tid, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, false);
        fd_kernel_instr = setup_counter(tid, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, true);
        fd_user_cycles = setup_counter(tid, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, false);
        fd_kernel_cycles = setup_counter(tid, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, true);

        // Enable all counters
        ioctl(fd_user_instr, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_user_instr, PERF_EVENT_IOC_ENABLE, 0);
        ioctl(fd_kernel_instr, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_kernel_instr, PERF_EVENT_IOC_ENABLE, 0);
        ioctl(fd_user_cycles, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_user_cycles, PERF_EVENT_IOC_ENABLE, 0);
        ioctl(fd_kernel_cycles, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_kernel_cycles, PERF_EVENT_IOC_ENABLE, 0);

    } catch (...) {
        // Cleanup on failure
        if (fd_user_instr != -1) close(fd_user_instr);
        if (fd_kernel_instr != -1) close(fd_kernel_instr);
        if (fd_user_cycles != -1) close(fd_user_cycles);
        if (fd_kernel_cycles != -1) close(fd_kernel_cycles);
        throw;
    }
}

// Destructor
CPU_Monitor::~CPU_Monitor() {
    if (fd_user_instr != -1) close(fd_user_instr);
    if (fd_kernel_instr != -1) close(fd_kernel_instr);
    if (fd_user_cycles != -1) close(fd_user_cycles);
    if (fd_kernel_cycles != -1) close(fd_kernel_cycles);
}

// Main API function to get deltas
std::optional<CPU_Counts> CPU_Monitor::get_deltas() {
    perf_read_format current_user_instr, current_kernel_instr, current_user_cycles, current_kernel_cycles;
    
    // Read all counters, checking for thread termination.
    if (read(fd_user_instr, &current_user_instr, sizeof(perf_read_format)) == -1 && errno == ESRCH) return std::nullopt;
    if (read(fd_kernel_instr, &current_kernel_instr, sizeof(perf_read_format)) == -1 && errno == ESRCH) return std::nullopt;
    if (read(fd_user_cycles, &current_user_cycles, sizeof(perf_read_format)) == -1 && errno == ESRCH) return std::nullopt;
    if (read(fd_kernel_cycles, &current_kernel_cycles, sizeof(perf_read_format)) == -1 && errno == ESRCH) return std::nullopt;

    bool multiplexing_detected = false;

    // Helper lambda to calculate the scaled delta for a single counter.
    // It updates the `last` value and the multiplexing flag.
    auto calculate_scaled_delta = 
        [&](const perf_read_format& current, perf_read_format& last, bool& multiplex_flag) -> long long {
        
        long long value_delta = current.value - last.value;
        long long time_enabled_delta = current.time_enabled - last.time_enabled;
        long long time_running_delta = current.time_running - last.time_running;

        // Update the last value for the next call *before* returning.
        last = current;

        // Check for multiplexing and scale if necessary.
        if (time_running_delta > 0 && time_enabled_delta > time_running_delta) {
            multiplex_flag = true;
            double ratio = static_cast<double>(time_enabled_delta) / time_running_delta;
            return static_cast<long long>(value_delta * ratio);
        }

        return value_delta;
    };

    CPU_Counts counts;
    counts.user_instructions = calculate_scaled_delta(current_user_instr, last_user_instr, multiplexing_detected);
    counts.kernel_instructions = calculate_scaled_delta(current_kernel_instr, last_kernel_instr, multiplexing_detected);
    counts.user_cycles = calculate_scaled_delta(current_user_cycles, last_user_cycles, multiplexing_detected);
    counts.kernel_cycles = calculate_scaled_delta(current_kernel_cycles, last_kernel_cycles, multiplexing_detected);
    counts.multiplexing_detected = multiplexing_detected;

    return counts;
}
