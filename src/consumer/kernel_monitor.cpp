/*
 * =====================================================================================
 *
 * Filename:  kernel_monitor.cpp
 *
 * Description:  Implementation of the KernelMonitor library.
 *
 * =====================================================================================
 */
#include "kernel_monitor.h" // Should be in the same directory or in include path
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <cerrno>

// Wrapper for the syscall
static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Private helper to set up one counter
int KernelMonitor::setup_counter(pid_t tid, uint64_t type, uint64_t config) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = type;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = config;
    pe.disabled = 1;       // Start disabled
    pe.exclude_user = 1;   // KERNEL-MODE ONLY
    pe.exclude_hv = 1;     // Exclude hypervisor

    int fd = perf_event_open(&pe, tid, -1, -1, 0);
    if (fd == -1) {
        throw std::runtime_error("perf_event_open failed: " + std::string(strerror(errno)));
    }
    return fd;
}

// Constructor
KernelMonitor::KernelMonitor(pid_t tid) {
    try {
        // Configure and create the instructions counter
        fd_instr = setup_counter(tid, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);

        // Configure and create the cycles counter
        fd_cycles = setup_counter(tid, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);

        // Configure and create the L1 data cache loads counter
        uint64_t l1d_load_config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
        fd_l1_loads = setup_counter(tid, PERF_TYPE_HW_CACHE, l1d_load_config);

        // Configure and create the L1 data cache stores counter
        uint64_t l1d_store_config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
        fd_l1_stores = setup_counter(tid, PERF_TYPE_HW_CACHE, l1d_store_config);

        // Enable all counters
        ioctl(fd_instr, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_instr, PERF_EVENT_IOC_ENABLE, 0);

        ioctl(fd_cycles, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_cycles, PERF_EVENT_IOC_ENABLE, 0);

        ioctl(fd_l1_loads, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_l1_loads, PERF_EVENT_IOC_ENABLE, 0);

        ioctl(fd_l1_stores, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd_l1_stores, PERF_EVENT_IOC_ENABLE, 0);

    } catch (...) {
        // If any setup fails, clean up any FDs that were opened before re-throwing.
        if (fd_instr != -1) close(fd_instr);
        if (fd_cycles != -1) close(fd_cycles);
        if (fd_l1_loads != -1) close(fd_l1_loads);
        if (fd_l1_stores != -1) close(fd_l1_stores);
        throw; // Re-throw the exception
    }
}

// Destructor
KernelMonitor::~KernelMonitor() {
    if (fd_instr != -1) close(fd_instr);
    if (fd_cycles != -1) close(fd_cycles);
    if (fd_l1_loads != -1) close(fd_l1_loads);
    if (fd_l1_stores != -1) close(fd_l1_stores);
}

// The main API function to get deltas
std::optional<KernelCounts> KernelMonitor::get_deltas() {
    long long current_instr, current_l1_loads, current_l1_stores, current_cycles;

    // Read all counters. If any read fails because the thread is gone,
    // gracefully fail by returning an empty optional.
    if (read(fd_instr, &current_instr, sizeof(long long)) == -1 && errno == ESRCH) return std::nullopt;
    if (read(fd_cycles, &current_cycles, sizeof(long long)) == -1 && errno == ESRCH) return std::nullopt;
    if (read(fd_l1_loads, &current_l1_loads, sizeof(long long)) == -1 && errno == ESRCH) return std::nullopt;
    if (read(fd_l1_stores, &current_l1_stores, sizeof(long long)) == -1 && errno == ESRCH) return std::nullopt;

    // Calculate deltas
    KernelCounts counts;
    counts.instructions = current_instr - last_instr;
    counts.cycles = current_cycles - last_cycles;
    counts.cache_accesses = (current_l1_loads - last_l1_loads) + (current_l1_stores - last_l1_stores);

    // Update last known values for the next call
    last_instr = current_instr;
    last_cycles = current_cycles;
    last_l1_loads = current_l1_loads;
    last_l1_stores = current_l1_stores;

    return counts;
}