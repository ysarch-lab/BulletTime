// Consumer process that receives data from BulletTime and compresses it
//  - Must create pipes to communicate with BulletTime
//  - Creates one listening worker thread per BulletTime application thread to collect data
//  - Listening threads send data to a processing (compression) threadpool
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <atomic>
#include <unordered_set>
#include <cassert>
#include <cmath>
#include <shared_mutex>

// Thread pool library
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <deque>
#include <functional>

// ZSTD compression library
#include <zstd.h>

// Shared with the Pin tool
#include "../include/consumer.h"

// Perf monitoring library
#include "cpu_monitor.h"

// C headers
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <signal.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
using namespace std;

// Lock for nice printing
mutex print_lock;

// Global variable to indicate when to stop the consumer
bool stop = false;

// Compression flag
bool use_compression = false;

// Ablation-specific flags (True by default so standard execution is unaffected)
bool app_dilation = true;
bool kernel_dilation = true;
bool use_direct_io = true;
bool use_hugepages = true;

// Number of control periods
uint32_t control_periods = 0;
double last_in_BW = 0.0;

// Atomic unsigned longs for in and out data sizes
atomic_ulong in_size(0);
atomic_ulong out_size(0);

// List of locks for thread safety
deque<shared_mutex> data_locks;
shared_mutex insertion_lock;

// Lists of data tracking buffer in/out speeds
vector<ulong> in_buf_time;
vector<ulong> in_buffers;
vector<ulong> out_buf_time;
vector<ulong> out_buffers;

// Track recent ratios of application instructions to all userspace instructions
//  - used to approximate instrumentation cost
vector<ulong> application_ins;
vector<ulong> userspace_ins;

// Track the peak overhead experienced by any thread
shared_mutex peak_lock;
bool overhead_balancing = false;
vector<double> n_buffers;
vector<double> n_work;
vector<double> total_work;
vector<double> last_work;
vector<double> last_buffers;
vector<double> last_dummy;
double max_buffer_work_ratio = 0.0;
double peak_work_whole_execution = 0.0;
int max_ratio_owner = -1;

// Shadow time-based counters for evaluation (under peak_lock)
vector<double> n_io_time;            // Windowed real I/O time (us)
vector<double> n_injected_time;      // Windowed injected sleep time (us)
vector<double> total_io_time;        // Cumulative real I/O time (us)
vector<double> total_injected_time;  // Cumulative injected sleep time (us)
double max_time_work_ratio = 0.0;
int max_time_ratio_owner = -1;
vector<double> n_gen_time;           // Windowed buffer generation time (us)

// Total statistics
shared_mutex total_lock;
vector<size_t> total_fcalls;
vector<size_t> total_instructions;
vector<size_t> total_mem_ins;
vector<size_t> k_ins;
vector<size_t> k_cycles;
vector<size_t> u_ins;
vector<size_t> u_cycles;
vector<unsigned long> total_buffers_processed;
vector<double> total_dummy_buffers;

// Timing real vs dummy bandwidth
vector<size_t> real_writes;
vector<size_t> real_write_latency;
vector<size_t> dummy_writes;
vector<size_t> dummy_write_latency;
vector<size_t> total_recv_wait;    // Cumulative consumer recv wait time (us)
vector<size_t> total_pin_wait;     // Cumulative Pin ready-signal wait time (us)

// Function to initialize vector entries for a new thread
void InitThreadData() {
    {
        unique_lock<shared_mutex> read_lock(insertion_lock);
        in_buf_time.push_back(0);
        in_buffers.push_back(0);
        out_buf_time.push_back(0);
        out_buffers.push_back(0);
        application_ins.push_back(0);
        userspace_ins.push_back(0);
        data_locks.emplace_back();
    }
    {
        unique_lock<shared_mutex> lock(peak_lock);
        n_buffers.push_back(0.0);
        n_work.push_back(0.0);
        last_work.push_back(0.0);
        last_buffers.push_back(0.0);
        last_dummy.push_back(0.0);
        total_work.push_back(0.0);
        n_io_time.push_back(0.0);
        n_injected_time.push_back(0.0);
        total_io_time.push_back(0.0);
        total_injected_time.push_back(0.0);
        n_gen_time.push_back(0.0);
    }
    {
        unique_lock<shared_mutex> lock(total_lock);
        total_fcalls.push_back(0);
        total_instructions.push_back(0);
        total_mem_ins.push_back(0);
        k_ins.push_back(0);
        k_cycles.push_back(0);
        u_ins.push_back(0);
        u_cycles.push_back(0);
        total_buffers_processed.push_back(0);
        total_dummy_buffers.push_back(0.0);
        real_writes.push_back(0);
        real_write_latency.push_back(0);
        dummy_writes.push_back(0);
        dummy_write_latency.push_back(0);
        total_recv_wait.push_back(0);
        total_pin_wait.push_back(0);
    }
}

void LogThreadStats(int thread_id) {
    double t_io_time, t_injected_time;
    {
        shared_lock<shared_mutex> peak_read_lock(peak_lock);
        t_io_time = total_io_time[thread_id];
        t_injected_time = total_injected_time[thread_id];
    }
    shared_lock<shared_mutex> read_lock(total_lock);
    double total = total_buffers_processed[thread_id] + total_dummy_buffers[thread_id];
    double app_ins_e9 = (double)total_instructions[thread_id] / 1e9;
    double k_ins_e9 = (double)k_ins[thread_id] / 1e9;
    double k_cycles_e9 = (double)k_cycles[thread_id] / 1e9;
    double u_ins_e9 = (double)u_ins[thread_id] / 1e9;
    double u_cycles_e9 = (double)u_cycles[thread_id] / 1e9;
    {
        std::lock_guard<std::mutex> lock(print_lock);
        std::cerr << endl
                  << "Thread " << thread_id << " wrote "
                  << total << " buffers ("
                  << total_buffers_processed[thread_id] << " data, "
                  << total_dummy_buffers[thread_id] << " dummy), "
                  << "total fcalls: " << total_fcalls[thread_id] << ", "
                  << "ins/fcall: " << (double)total_instructions[thread_id] / total_fcalls[thread_id]
                  << ", real BW: " << (double)real_writes[thread_id] / real_write_latency[thread_id]
                  << ", dummy BW: " << (double)dummy_writes[thread_id] / dummy_write_latency[thread_id]
                  << ", mem ratio: " << (double)total_mem_ins[thread_id] / total_instructions[thread_id]
                  << endl << "\t"
                  << ", app ins (1e9): " << app_ins_e9
                  << ", kernel ins (1e9): " << k_ins_e9
                  << endl << "\t"
                  << ", app ins/buffer (1e9): " << app_ins_e9 / total_buffers_processed[thread_id]
                  << ", kernel ins/buffer (1e9): " << k_ins_e9 / total_buffers_processed[thread_id]
                  << endl << "\t"
                  << ", kernel ipc: " << k_ins_e9 / k_cycles_e9
                  << ", app ipc: " << u_ins_e9 / u_cycles_e9
                  << endl << "\t"
                  << ", work/buffer: " << (double)total_work[thread_id] / total_buffers_processed[thread_id]
                  << endl << "\t"
                  << ", total_io_time_us: " << t_io_time
                  << ", total_injected_time_us: " << t_injected_time
                  << ", time/buffer: " << (t_io_time + t_injected_time) / total_buffers_processed[thread_id]
                  << endl << "\t"
                  << ", recv_wait_s: " << (double)total_recv_wait[thread_id] / 1e6
                  << ", pin_wait_s: " << (double)total_pin_wait[thread_id] / 1e6
                  << endl;
    }
}

// MEMREF struct
struct MEMREF {
    unsigned long pc;
    unsigned long ea;
    int sz;
    int is_read;
};

// Indicate if this thread should skip overhead balancing
bool skip_overhead_balancing(double t_work, double buf_work_ratio, size_t fcalls) {
    bool skip = false
        //    || (t_work * 10 < peak_work_whole_execution)
           || std::isnan(buf_work_ratio)
           || std::isinf(buf_work_ratio)
           || (fcalls == 0)
           ;
    return skip;
}

//==============================================================================
// Control Thread (Optional)
//  - Periodically examines buffer processing statistics
//  - Adjusts CPU usage to balance incoming data rate with I/O bandwidth
//  - Scales system by the same factor as CPU usage (/proc/sys/time_dilation)
//==============================================================================
void ControlThread() {
    unsigned int dilation_factor = 1000; // Default dilation factor (1.0x)
    double max_slowdown = 1.0;
    double instrumentation_cost = 1.0;
    while (!stop) {
        // Periodic sleep
        this_thread::sleep_for(chrono::seconds(CONTROL_SLEEP_SEC));

        // Reset overhead balancing counters
        string work_log = "\t";
        if (overhead_balancing) {
            const double SCALE_FACTOR = 0.25;
            unique_lock<shared_mutex> lock(peak_lock);
            double new_max_ratio = 0.0;
            double new_max_time_ratio = 0.0;
            max_slowdown = 1.0;
            for (size_t i = 0; i < n_buffers.size(); ++i) {
                // If this thread did no work during this period, reset completely
                if (n_work[i] == last_work[i]) {
                    n_buffers[i] = 0;
                    n_work[i] = 0;
                    n_io_time[i] = 0;
                    n_injected_time[i] = 0;
                    n_gen_time[i] = 0;
                }
                else {
                    double t_work = total_work[i];
                    double buf_work_ratio = n_buffers[i] / n_work[i];
                    if (!skip_overhead_balancing(t_work, buf_work_ratio, total_fcalls[i])
                        && buf_work_ratio > new_max_ratio
                    ) {
                        new_max_ratio = buf_work_ratio;
                        max_ratio_owner = i;
                    }
                    // Shadow time-based ratio tracking
                    double time_work_ratio = (n_io_time[i] + n_injected_time[i]) / n_work[i];
                    if (!skip_overhead_balancing(t_work, buf_work_ratio, total_fcalls[i])
                        && time_work_ratio > new_max_time_ratio
                    ) {
                        new_max_time_ratio = time_work_ratio;
                        max_time_ratio_owner = i;
                    }
                    // Compute per-thread slowdown for kernel dilation
                    if (n_gen_time[i] > 0) {
                        double slowdown = (n_gen_time[i] + n_io_time[i]) / n_gen_time[i];
                        if (slowdown > max_slowdown) max_slowdown = slowdown;
                    }

                    n_buffers[i] *= SCALE_FACTOR;
                    n_work[i] *= SCALE_FACTOR;
                    n_io_time[i] *= SCALE_FACTOR;
                    n_injected_time[i] *= SCALE_FACTOR;
                    n_gen_time[i] *= SCALE_FACTOR;

                    // Logging
                    // if (!std::isnan(buf_work_ratio) && !std::isinf(buf_work_ratio)){
                    //     work_log += "(" + to_string(i) + ": work/min work ratio: " + to_string(max_buffer_work_ratio/buf_work_ratio);
                    //     work_log += ", work/buf: " + to_string(1.0/buf_work_ratio);
                    //     work_log += ", dummy/buf ratio: " + to_string(dummy_buffers_this_period / buffers_this_period);
                    //     work_log += ")\t";
                    // }
                }
                last_work[i] = n_work[i];
                last_buffers[i] = n_buffers[i];
            }
            // std::cerr << "\tMin Work per Buffer: " << 1/max_buffer_work_ratio << " (Owner: " << max_ratio_owner << ")" << std::endl;
            max_buffer_work_ratio = new_max_ratio;
            max_time_work_ratio = new_max_time_ratio;
            // Scale max_slowdown by instrumentation cost (user_ins / app_ins)
            ulong total_app_ins = 0, total_user_ins = 0;
            {
                shared_lock<shared_mutex> read_lock(insertion_lock);
                for (size_t i = 0; i < application_ins.size(); ++i) {
                    shared_lock<shared_mutex> lock(data_locks[i]);
                    total_app_ins += application_ins[i];
                    total_user_ins += userspace_ins[i];
                }
            }
            instrumentation_cost = (total_app_ins > 0) ? (double)total_user_ins / total_app_ins : 1.0;
            dilation_factor = (unsigned int)(instrumentation_cost * max_slowdown * 1000);
            dilation_factor = std::max(dilation_factor, 1000u);

            // Log shadow time-based ratios alongside buffer-based ratios
            if (n_buffers.size() > 1) {
                std::ostringstream shadow_log;
                shadow_log << "SHADOW_TIME\tperiod=" << control_periods
                           << "\tmax_buf_ratio=" << new_max_ratio
                           << "\tmax_buf_owner=" << max_ratio_owner
                           << "\tmax_time_ratio=" << new_max_time_ratio
                           << "\tmax_time_owner=" << max_time_ratio_owner;
                for (size_t i = 0; i < n_buffers.size(); ++i) {
                    double bwr = (n_work[i] > 0) ? n_buffers[i] / n_work[i] : 0.0;
                    double twr = (n_work[i] > 0) ? (n_io_time[i] + n_injected_time[i]) / n_work[i] : 0.0;
                    shadow_log << "\t[" << i << "]"
                               << " bwr=" << bwr
                               << " twr=" << twr
                               << " io_us=" << total_io_time[i]
                               << " inj_us=" << total_injected_time[i]
                               << " recv_w=" << total_recv_wait[i]
                               << " pin_w=" << total_pin_wait[i];
                }
                std::lock_guard<std::mutex> plock(print_lock);
                std::cerr << shadow_log.str() << std::endl;
            }
        }

        // Write kernel dilation factor to sysfs
        if (kernel_dilation) {
            std::ofstream dilation_file(DILATION_KNOB);
            if (dilation_file.is_open()) {
                dilation_file << dilation_factor << std::endl;
                dilation_file.close();
            } else {
                std::cerr << "ControlThread: Failed to open dilation factor file " << DILATION_KNOB
                    << " (Error: " << strerror(errno) << ")" << endl;
            }
            std::cerr << "Dilation Factor: " << dilation_factor << " / 1000"
                      << " (slowdown: " << max_slowdown << "x, instr cost: " << instrumentation_cost << "x)" << std::endl;
        }

        control_periods++;
    }
}

// Helper for dummy writes
//  - injects sleep as if we were writing dummy I/O at the same speed of this thread's I/O
// Units:
//  - extra_io : bytes
//  - target_BW: bytes/us
size_t WriteDummy(int thread_id, size_t extra_io, double target_BW, int ratio_owner) {
    if (!app_dilation) return 0; // Ablation check: skip dummy writes if app dilation is disabled

    // Get this thread's regular I/O bandwidth
    double bw_owner, bw_this;
    {
        shared_lock<shared_mutex> lock(total_lock);
        bw_owner = (double)real_writes[ratio_owner] / real_write_latency[ratio_owner]; // B/us
        bw_this = (double)real_writes[thread_id] / real_write_latency[thread_id]; // B/us
    }
    target_BW = (bw_owner + bw_this) / 2; // B/us
    // target_BW = bw_this;

    // Sleep as if we are matching the regular write bandwidth of the ratio owner
    double sleep_time_us = static_cast<double>(extra_io) / target_BW;

    // Do sleep, with max period of 10ms
    size_t start_time = chrono::duration_cast<chrono::microseconds>(
        chrono::high_resolution_clock::now().time_since_epoch()).count();
    size_t timer = start_time;
    while (sleep_time_us > 10000 && !stop) {
        std::this_thread::sleep_for(std::chrono::microseconds(10000));
        size_t curr = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now().time_since_epoch()).count();
        sleep_time_us -= curr - timer;
        timer = curr;
    }
    if (sleep_time_us > 0 && !stop){
        std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::microseconds>
            (std::chrono::duration<double, std::micro>(sleep_time_us)));
    }

    // Statistics
    size_t dummy_time = chrono::duration_cast<chrono::microseconds>(
        chrono::high_resolution_clock::now().time_since_epoch()).count() - start_time;
    dummy_writes[thread_id] += extra_io;
    dummy_write_latency[thread_id] += dummy_time;
    return dummy_time;
}

//==============================================================================
// Listening function
//  - Loops on sockets to continuously receive data from BulletTime
//  - Ends on stop signal
//==============================================================================
void Listen(pid_t pid, int thread_id, int rdy_fd, int size_fd, int out_fd, string output_file,
            void* shared_mem, void* out_buffer, uint64_t& in_data, uint64_t& out_data) {
    // Initialize ZSTD compression context and positions
    ZSTD_CCtx* cctx = NULL;
    ZSTD_outBuffer z_out;
    ZSTD_inBuffer z_in;
    if (use_compression) {
        cctx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, -7);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
        z_out.dst = out_buffer;
        z_out.size = ZSTD_BUFFER_SIZE;
        z_out.pos = 0;
        z_in.src = (char*)shared_mem;
        z_in.size = ALLOCATION_SIZE;
        z_in.pos = 0;
    }

    // Initialize monitor to track kernel-mode activity
    CPU_Monitor cpu_monitor(pid);

    // Communication protocol:
    // 1) Consumer sends ready signal to BulletTime
    // 2) BulletTime writes to shared memory --> requires release fence
    // 3) BulletTime sends size of data to consumer
    // 4) Consumer waits for shared memory to be synced --> requires acquire fence
    // 5) Consumer reads data from shared memory
    size_t data_written = 0;
    size_t dummy_written = 0;
    size_t rdy_buf[1] = {0};    // Ready signal
    size_t recv_data[7] = {0};  // Metadata buffer for received data
    size_t extra_io = 0;        // Extra I/O operations for overhead balancing
    double extra_buffers = 0.0;
    while (!stop) {
        // Send ready signal to BulletTime
        rdy_buf[0] = extra_io;
        ssize_t err = send(rdy_fd, rdy_buf, sizeof(rdy_buf), MSG_NOSIGNAL);
        if (err < 0) {
            if (errno == EPIPE) {
                break;
            }
            std::cerr << "Consumer: Failed to send ready signal" << endl;
            return;
        }
        extra_io = 0;
        extra_buffers = 0.0;

        // Receive metadata from BulletTime:
        //  1) Size of data written
        //  2) Time taken to generate the data (us)
        //  3) Amount of "work" performed by the application thread
        //      - Defined by the tool (e.g., # instructions or # calls to a critical function)
        //  4) # Instructions executed creating this buffer
        //  5) # Memory instructions executed
        //  6) # Function calls executed
        size_t recv_start = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now().time_since_epoch()).count();
        if (recv(size_fd, recv_data, sizeof(recv_data), 0) < 0) {
            std::cerr << "Consumer: Failed to receive data size and generation time" << endl;
            return;
        }
        size_t recv_wait = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now().time_since_epoch()).count() - recv_start;
        size_t data_size = recv_data[0];
        size_t gen_time = recv_data[1];
        size_t pin_work = recv_data[2];
        size_t ins_exec = recv_data[3];
        size_t mem_ins_exec = recv_data[4];
        size_t fcalls_exec = recv_data[5];
        size_t pin_wait = recv_data[6];  // Time Pin waited for consumer ready signal (us)

        // Get the kernel-level instructions and memory accesses since the last buffer
        auto counts = cpu_monitor.get_deltas();
        if (counts == std::nullopt) {
            std::cerr << "Consumer: Monitored thread " << pid << " has terminated" << endl;
            break;
        }
        auto [user_ins, user_cycles, kernel_ins, kernel_cycles, multiplexing_detected] = *counts;

        if (data_size == 0) {
            std::cerr << "Consumer: Received stop signal (no data)" << endl;
            break;
        }
        atomic_thread_fence(memory_order_acquire); // Ensure data is written before reading

        // Time output processing (us)
        size_t start_time = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now().time_since_epoch()).count();

        //==============================================================================
        // Processing Data
        //==============================================================================
        void* data_out = shared_mem;
        size_t out_size = data_size;

        // out_buffer is only present if compression is enabled
        if (out_buffer != NULL) {
            data_out = out_buffer;
            in_data += data_size;
            
            // Compress
            z_in.size = data_size;
            z_in.pos = 0;
            size_t remaining = ZSTD_compressStream2(cctx, &z_out, &z_in, ZSTD_e_flush);
            if (ZSTD_isError(remaining)) {
                std::cerr << "Consumer: ZSTD_compressStream2 failed: " << ZSTD_getErrorName(remaining) << endl;
                return;
            }

            // Round down output position to 4KB boundary
            size_t new_size = (z_out.pos / 4096) * 4096;
            if (new_size > 0) {
                ssize_t written = write(out_fd, out_buffer, new_size);
                if ((size_t) written != new_size) {
                    std::cerr << "Consumer: Failed to write compressed data to disk, wrote " << written << " bytes" << endl;
                    if (written < 0) {
                        std::cerr << "Consumer: Error: " << strerror(errno) << endl;
                    }
                    return;
                }
                data_written += written;
                out_data += written;

                // Copy remaining data to beginning of output buffer
                size_t remaining_size = z_out.pos - new_size;
                memcpy(out_buffer, (char*)out_buffer + new_size, remaining_size);
                z_out.pos = remaining_size;
            }
        }

        // Raw data
        else {
            // If data_size is less than buffer size, pad with zeroes
            if (data_size < CONSUMER_BUFFER_SIZE) {
                memset((char*)data_out + data_size, 0, CONSUMER_BUFFER_SIZE - data_size);
            }
            data_size = CONSUMER_BUFFER_SIZE;

            // Flush to disk
            ssize_t written = write(out_fd, data_out, data_size);
            if ((size_t) written != data_size) {
                std::cerr << "Consumer: Failed to write data to disk, wrote " << written << " bytes" << endl;
                // print errno
                if (written < 0) {
                    std::cerr << "Consumer: Error: " << strerror(errno) << endl;
                }
                return;
            }
            data_written += written;
        }
        size_t real_w_time = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now().time_since_epoch()).count() - start_time;
        {
            shared_lock<shared_mutex> read_lock(total_lock);
            real_write_latency[thread_id] += real_w_time;
            real_writes[thread_id] += data_size;
            total_buffers_processed[thread_id] += 1.0;
        }

        // Overhead balancing logic
        //  - Only run this logic if there are multiple threads
        //  - Don't even start profiling until there's a chance for behavior to stabilize
        if (overhead_balancing && n_buffers.size() > 1 
            // && control_periods * CONTROL_SLEEP_SEC >= 5
            ) {
            // Calculate work as the combined instructions executed in the application
            // and in kernel mode.
            // Scale the application instructions by the ratio kernel_ipc / user_ipc
            //      - APP_INS != USER_INS     (because of instrumentation)
            double kernel_ipc = double(kernel_ins) / double(kernel_cycles);
            double user_ipc = double(user_ins) / double(user_cycles);
            double app_ins = (double) ins_exec;
            kernel_ins -= WORK_OFFSET;
            kernel_ins = (kernel_ins < 0) ? 0 : kernel_ins;
            double work = kernel_ins + (app_ins * (kernel_ipc / user_ipc));

            // Track and update how much work this thread executes per buffer
            double buf_work_ratio, max_ratio_snapshot, ratio_owner_snapshot, t_work;
            {
                shared_lock<shared_mutex> lock(peak_lock);
                n_work[thread_id] += work;
                n_buffers[thread_id] += 1.0;
                total_work[thread_id] += work;
                n_io_time[thread_id] += (double)real_w_time;
                total_io_time[thread_id] += (double)real_w_time;
                n_gen_time[thread_id] += (double)gen_time;
                buf_work_ratio = n_buffers[thread_id] / n_work[thread_id];

                max_ratio_snapshot = max_buffer_work_ratio;
                ratio_owner_snapshot = max_ratio_owner;
                t_work = total_work[thread_id];
            }
            // Update stat totals
            size_t curr_fcalls = 0;
            {
                shared_lock<shared_mutex> read_lock(total_lock);
                total_fcalls[thread_id] += fcalls_exec;
                total_instructions[thread_id] += ins_exec;
                total_mem_ins[thread_id] += mem_ins_exec;
                k_ins[thread_id] += kernel_ins;
                k_cycles[thread_id] += kernel_cycles;
                u_ins[thread_id] += user_ins;
                u_cycles[thread_id] += user_cycles;
                total_recv_wait[thread_id] += recv_wait;
                total_pin_wait[thread_id] += pin_wait;
                curr_fcalls = total_fcalls[thread_id];
            }
            // Filter out non-key threads
            //  - this thread's total work << peak_work if using instructions
            // Also skip if:
            //  - if buf_work_ratio is nan or inf
            //  - we're early into the program to let behavior stabilize first
            bool skip = skip_overhead_balancing(t_work, buf_work_ratio, curr_fcalls)
                        // || (control_periods * CONTROL_SLEEP_SEC < 10)
                        ;
            if (!skip) {
                // Check if we should update the current highest buffer / work ratio
                //  1) We have a higher work ratio
                //  2) This thread "owns" the highest ratio
                if (buf_work_ratio > max_ratio_snapshot) {
                    unique_lock<shared_mutex> lock(peak_lock);
                    if (buf_work_ratio > max_buffer_work_ratio) {
                        max_buffer_work_ratio = buf_work_ratio;
                        max_ratio_owner = thread_id;
                    }
                    max_ratio_snapshot = max_buffer_work_ratio;
                    ratio_owner_snapshot = max_ratio_owner;

                    // Also update peak_work
                    if (total_work[thread_id] > peak_work_whole_execution) {
                        peak_work_whole_execution = total_work[thread_id];
                    }
                }

                // If we do more work per buffer, we progress faster than the
                // "slowest" thread (the thread most slowed by tracing)
                // So we should inject overheads to balance overheads
                if (max_ratio_snapshot > buf_work_ratio) {
                    // Calculate extra IO overhead to inject
                    extra_buffers = (max_ratio_snapshot / buf_work_ratio) - 1.0;

                    // Only inject delay if it's greater than 1/4 a buffer
                    if (extra_buffers > 0.25) {
                        // Log and perform the dummy IO
                        //  - Sleep duration targets the bandwidth of the current thread
                        extra_io = (size_t)(extra_buffers * CONSUMER_BUFFER_SIZE);
                        dummy_written += extra_io;
                        double target_BW = (double) data_size / real_w_time; // B/us
                        size_t actual_sleep_us = WriteDummy(thread_id, extra_io, target_BW, ratio_owner_snapshot);
                        {
                            shared_lock<shared_mutex> lock(peak_lock);
                            n_injected_time[thread_id] += (double)actual_sleep_us;
                            total_injected_time[thread_id] += (double)actual_sleep_us;
                        }

                        // Stats
                        {
                            shared_lock<shared_mutex> read_lock(total_lock);
                            total_dummy_buffers[thread_id] += extra_buffers;
                        }
                    }
                }
            }
        }

        // End timer
        size_t end_time = chrono::duration_cast<chrono::microseconds>(
            chrono::high_resolution_clock::now().time_since_epoch()).count();
        ssize_t elapsed_time = end_time - start_time;

        // Update all buffer information
        // Get lock for thread_id
        {
            shared_lock<shared_mutex> read_lock(insertion_lock);
            unique_lock<shared_mutex> lock(data_locks[thread_id]);
            in_buf_time[thread_id] += gen_time;
            in_buffers[thread_id] += data_size;
            out_buf_time[thread_id] += elapsed_time;
            out_buffers[thread_id] += data_size;
            application_ins[thread_id] += ins_exec;
            userspace_ins[thread_id] += user_ins;
        }

        // Overwrite if exceeding 64GB
        if (data_written >= ((size_t)1 << 36)) {
            close(out_fd);
            int flags = O_WRONLY | (use_direct_io ? O_DIRECT : 0);
            out_fd = open(output_file.c_str(), flags, 0666);
            data_written = 0;
        }
    }

    //=============================================================================
    // POST-LOOP
    //=============================================================================
    // Finish ZSTD frame
    if (use_compression) {
        // Stop signal received, re-open file without O_DIRECT (use append mode)
        close(out_fd);
        out_fd = open(output_file.c_str(), O_WRONLY | O_APPEND, 0666);
        if (out_fd < 0) {
            std::cerr << "Consumer: Failed to open output file " << output_file << " for append";
            std::cerr << " (Error: " << strerror(errno) << ")" << endl;
            return;
        }

        // Create a dummy memref to force ZSTD to finish the frame
        // z_in.size = sizeof(MEMREF);
        // z_in.pos = 0;
        // memset(shared_mem, 0, sizeof(MEMREF));

        // Write all data currently in the output buffer
        ssize_t written = write(out_fd, out_buffer, z_out.pos);
        if ((size_t) written != z_out.pos) {
            std::cerr << "Consumer: Failed to write remaining data to disk, wrote " << written << " bytes" << endl;
            if (written < 0) {
                std::cerr << "Consumer: Error: " << strerror(errno) << endl;
            }
            return;
        }
        out_data += written;
        z_out.pos = 0;

        // Finish compression
        size_t remaining = ZSTD_compressStream2(cctx, &z_out, &z_in, ZSTD_e_end);
        if (remaining != 0) {
            std::cerr << "Consumer: ZSTD_compressStream2 failed to complete frame" << endl;
            return;
        }
        written = write(out_fd, out_buffer, z_out.pos);
        out_data += written;
        ZSTD_freeCCtx(cctx);
    }
    close(out_fd);

    // Rename file with compression extension
    if (use_compression) {
        string comp_file = output_file + ".zst";
        if (rename(output_file.c_str(), comp_file.c_str()) != 0) {
            std::cerr << "Consumer: Failed to rename output file " << output_file << " to " << comp_file;
            std::cerr << " (Error: " << strerror(errno) << ")" << endl;
        }
    }
}

//==============================================================================
// Receiver Threads
//  - Setups sockets and shared memory for receiving data from BulletTime
//  - Processes data and writes to disk with direct IO
//==============================================================================
void ReceiverThread(const string& target_dir, const string& thread_name, int thread_id) {
    // Thread name in form <arbitrary prefix>-<pid>.<tid>, parse
    int pid, tid;
    size_t last_period = thread_name.find_last_of(".");
    size_t last_dash = thread_name.find_last_of("-");
    if (last_period == string::npos || last_dash == string::npos) {
        std::cerr << "Consumer: Failed to parse thread name " << thread_name << endl;
        return;
    }
    pid = stoi(thread_name.substr(last_dash + 1, last_period - last_dash - 1));
    tid = stoi(thread_name.substr(last_period + 1));
    std::cerr << "Consumer: Listening on socket for BulletTime thread " << pid << "_" << tid << endl;

    // Create output file for this thread
    //  - Open with direct IO flag
    int flags = O_CREAT | O_WRONLY | (use_direct_io ? O_DIRECT : 0);
    string output_file = target_dir + "/output_" + to_string(tid);
    int out_fd = open(output_file.c_str(), flags, 0666);
    if (out_fd < 0) {
        std::cerr << "Consumer: Failed to open output file " << output_file << endl;
        return;
    }

    // Create file from which to make mmap'd shared memory
    //  - 128MB, conditionally use huge pages based on ablation mode
    size_t shm_size = ALLOCATION_SIZE;
    string base_dir = use_hugepages ? "/dev/hugepages/" : "/dev/shm/";
    string file_name = base_dir + "shared_mem_" + to_string(tid);
    int fd = open(file_name.c_str(), O_RDWR | O_CREAT, 0777);
    if (fd < 0) {
        std::cerr << "Consumer: Failed to create shared memory file " << file_name;
        std::cerr << " (Error: " << strerror(errno) << ")" << endl;
        return;
    }
    if (ftruncate(fd, shm_size) < 0) {
        std::cerr << "Consumer: Failed to truncate shared memory file " << file_name << endl;
        return;
    }
    int mmap_flags = MAP_SHARED | (use_hugepages ? MAP_HUGETLB : 0);
    void* shared_mem = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);
    if (shared_mem == MAP_FAILED) {
        std::cerr << "Consumer: Failed to mmap shared memory file " << file_name;
        std::cerr << " (Error: " << strerror(errno) << ")" << endl;
        return;
    }

    // If using compression, mmap a second buffer for output
    void* out_buffer = NULL;
    if (use_compression) {
        out_buffer = mmap(NULL, ZSTD_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
        if (out_buffer == MAP_FAILED) {
            std::cerr << "Consumer: Failed to mmap output buffer";
            std::cerr << " (Error: " << strerror(errno) << ")" << endl;
            return;
        }
    }

    // Create two sockets
    //  - 1) For this consumer thread to tell BulletTime it's ready to receive data
    //  - 2) For BulletTime to indicate the size of the data it's sending (data will be written to mmap'd shared memory)
    string rdy_name = target_dir + "/rdy_" + to_string(tid);
    string size_name = target_dir + "/size_" + to_string(tid);
    int rdy_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    int size_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (rdy_sock < 0 || size_sock < 0) {
        std::cerr << "Consumer: Failed to create sockets";
        std::cerr << " (Error: " << strerror(errno) << ")" << endl;
        return;
    }
    // Port number is magic + 2*tid
    struct sockaddr_un rdy_addr, size_addr;
    memset(&rdy_addr, 0, sizeof(struct sockaddr_un));
    memset(&size_addr, 0, sizeof(struct sockaddr_un));
    rdy_addr.sun_family = AF_UNIX;
    size_addr.sun_family = AF_UNIX;
    strncpy(rdy_addr.sun_path, rdy_name.c_str(), sizeof(rdy_addr.sun_path) - 1);
    strncpy(size_addr.sun_path, size_name.c_str(), sizeof(size_addr.sun_path) - 1);
    if (bind(rdy_sock, (struct sockaddr*)&rdy_addr, sizeof(rdy_addr)) < 0
        || bind(size_sock, (struct sockaddr*)&size_addr, sizeof(size_addr)) < 0) {
        std::cerr << "Consumer: Failed to bind sockets" << endl;
        return;
    }

    // Listen on the sockets
    // std::cerr << "Consumer: Listening on sockets " << rdy_name << " and " << size_name << endl;
    if (listen(rdy_sock, 1) < 0 || listen(size_sock, 1) < 0) {
        std::cerr << "Consumer: Failed to listen on sockets" << endl;
        return;
    }
    // Accept connections
    int rdy_fd = accept(rdy_sock, NULL, NULL);
    int size_fd = accept(size_sock, NULL, NULL);
    if (rdy_fd < 0 || size_fd < 0) {
        std::cerr << "Consumer: Failed to accept connections" << endl;
        return;
    }
    // std::cerr << "Consumer: Accepted connections on sockets " << rdy_name << " and " << size_name << endl;

    // Start listening for data
    uint64_t in_data = 0;
    uint64_t out_data = 0;
    Listen(pid, thread_id, rdy_fd, size_fd, out_fd, output_file,
           shared_mem, out_buffer, in_data, out_data);

    // Atomic add for in and out data sizes
    in_size += in_data;
    out_size += out_data;

    // Delete sockets and shared memory
    close(rdy_fd);
    close(size_fd);
    close(rdy_sock);
    close(size_sock);
    close(fd);
    munmap(shared_mem, ALLOCATION_SIZE);
    remove(file_name.c_str());
    remove(rdy_addr.sun_path);
    remove(size_addr.sun_path);
    if (use_compression) {
        munmap(out_buffer, ZSTD_BUFFER_SIZE);
    }

    // Print thread statistics to log
    LogThreadStats(thread_id);
}

//==============================================================================
// Main - Setup pipes and threads
//  - Takes a target directory to create named pipes in
//      - Check <TARGET>/pids for number of BulletTime application threads
//      - Create a named pipe for each application thread
//  - Create a thread pool to handle compression
//==============================================================================
int main(int argc, char* argv[]) {
    // Required arguments: target directory
    // Optional arguments: use_compression (0 or 1), --dynamic, -s <sleep_sec>
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <target directory> [use_compression]"
                  << " [-s <sleep_seconds>]"
                  << " [--direct-io {0|1}] [--hugepages {0|1}]"
                  << " [--app-dilation {0|1}] [--kernel-dilation {0|1}]"
                  << " [--compression {0|1}]" << std::endl;
        return 1;
    }
    string target_dir = argv[1];
    bool dynamic_control = false;

    // Track whether individual feature flags were explicitly set
    bool compression_flag_set = false;

    try {
        for (int i = 2; i < argc; ++i) {
            string arg = argv[i];
            if (arg == "-s") {
                if (i + 1 < argc) {
                    CONTROL_SLEEP_SEC = stoul(argv[++i]);
                } else {
                    std::cerr << "Error: -s flag requires a value." << std::endl;
                    return 1;
                }
            } else if (arg == "--direct-io") {
                if (i + 1 < argc) {
                    use_direct_io = (stoi(argv[++i]) == 1);
                } else {
                    std::cerr << "Error: --direct-io requires a value (0 or 1)." << std::endl;
                    return 1;
                }
            } else if (arg == "--hugepages") {
                if (i + 1 < argc) {
                    use_hugepages = (stoi(argv[++i]) == 1);
                } else {
                    std::cerr << "Error: --hugepages requires a value (0 or 1)." << std::endl;
                    return 1;
                }
            } else if (arg == "--app-dilation") {
                if (i + 1 < argc) {
                    app_dilation = (stoi(argv[++i]) == 1);
                } else {
                    std::cerr << "Error: --app-dilation requires a value (0 or 1)." << std::endl;
                    return 1;
                }
            } else if (arg == "--kernel-dilation") {
                if (i + 1 < argc) {
                    kernel_dilation = (stoi(argv[++i]) == 1);
                } else {
                    std::cerr << "Error: --kernel-dilation requires a value (0 or 1)." << std::endl;
                    return 1;
                }
            } else if (arg == "--compression") {
                if (i + 1 < argc) {
                    use_compression = (stoi(argv[++i]) == 1);
                    compression_flag_set = true;
                } else {
                    std::cerr << "Error: --compression requires a value (0 or 1)." << std::endl;
                    return 1;
                }
            } else if (i == 2 && arg[0] != '-') {
                // Legacy positional argument for use_compression (backward compat)
                // --compression flag takes precedence if both are present
                if (!compression_flag_set) {
                    use_compression = (stoi(arg) == 1);
                }
            } else {
                std::cerr << "Warning: Ignoring unknown argument '" << arg << "'" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing arguments: " << e.what() << std::endl;
        return 1;
    }

    // Auto-enable prerequisites based on feature flags
    if (app_dilation) {
        overhead_balancing = true;
        dynamic_control = true;
    }
    if (kernel_dilation) {
        dynamic_control = true;
    }

    // Print settings summary
    std::cerr << "Consumer starting with settings:" << std::endl;
    std::cerr << "\tDirect IO: " << (use_direct_io ? "Enabled" : "Disabled") << std::endl;
    std::cerr << "\tHugepages: " << (use_hugepages ? "Enabled" : "Disabled") << std::endl;
    std::cerr << "\tApp Dilation (Balancing): " << (app_dilation ? "Enabled" : "Disabled") << std::endl;
    std::cerr << "\tKernel Dilation (Dynamic): " << (kernel_dilation ? "Enabled" : "Disabled") << std::endl;
    std::cerr << "\tCompression: " << (use_compression ? "Enabled" : "Disabled") << std::endl;
    std::cerr << "\tControl sleep period: " << CONTROL_SLEEP_SEC << "s" << std::endl;

    // Preflight: if kernel dilation was requested, the sleep_dilation module
    // must be loaded. Fail fast with a clear pointer to the setup script.
    if (kernel_dilation) {
        std::ifstream knob(DILATION_KNOB);
        if (!knob.good()) {
            std::cerr << "ERROR: kernel dilation requested but " << DILATION_KNOB
                      << " is not accessible." << std::endl;
            std::cerr << "       Has setup_sleep_dilation.sh been run to load the module?"
                      << std::endl;
            return 1;
        }
    }

    // Turn off SIG_PIPE
    signal(SIGPIPE, SIG_IGN);

    // Create IPC files for each BulletTime application thread
    string pid_dir = target_dir + "/pids";
    struct stat st;
    if (stat(pid_dir.c_str(), &st) != 0) {
        std::cerr << "Consumer: Waiting for BulletTime pids directory " << pid_dir << endl;
        while (stat(pid_dir.c_str(), &st) != 0);
    }

    // Use inotify to sleep until the directory is non-empty
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        std::cerr << "Consumer: Failed to initialize inotify" << endl;
        return 1;
    }
    int watch_fd = inotify_add_watch(inotify_fd, pid_dir.c_str(), IN_CREATE);
    if (watch_fd < 0) {
        std::cerr << "Consumer: Failed to add inotify watch" << endl;
        return 1;
    }
    std::cerr << "Consumer: Monitoring " << pid_dir << " for new BulletTime pids" << endl;

    // Wait for files to appear in the directory
    unordered_set<string> seen;
    char buffer[4096];
    vector<thread> threads;
    bool control_thread_started = false;
    thread control_thread;
    while (!stop) {
        // Event indicates new files were created - create named pipes and start listening
        // - File name is the name the pipe should have (once the directory prefix is removed)
        int bytes_read = read(inotify_fd, buffer, 4096);
        if (bytes_read < 0) {
            std::cerr << "Consumer: Failed to read inotify event" << endl;
            return 1;
        }

        // Start control thread if not already started and dynamic flag is set
        if (dynamic_control && !control_thread_started) {
            // Start control thread to monitor buffer processing statistics
            control_thread = thread(ControlThread);
            std::cerr << "Consumer: Started dynamic control thread" << endl;
            control_thread_started = true;
        }

        // Loop through all files in the directory
        DIR* dir = opendir(pid_dir.c_str());
        if (dir == NULL) {
            std::cerr << "Consumer: Failed to open BulletTime pids directory " << pid_dir << endl;
            return 1;
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            string file_name = entry->d_name;
            if (file_name == "." || file_name == ".." || seen.find(file_name) != seen.end()) {
                continue;
            }
            if (file_name == "stop") {
                stop = true;
                std::cerr << "Consumer: Received stop signal" << endl;
                remove((pid_dir + "/stop").c_str());
                std::atomic_thread_fence(std::memory_order_release); // Ensure memory writes are visible
                break;
            }
            std::cerr << "Consumer: Found new BulletTime pid file " << file_name << endl;

            // Remove the directory prefix to get the pipe name
            string pipe_name = target_dir + "/" + file_name;

            // Create listener thread, add entry to atomic vectors
            InitThreadData();
            threads.push_back(thread(ReceiverThread, target_dir, file_name, threads.size()));

            // Remove the file from the directory
            string full_path = pid_dir + "/" + file_name;
            if (remove(full_path.c_str()) != 0) {
                std::cerr << "Consumer: Failed to remove file " << full_path;
                std::cerr << " (Error: " << strerror(errno) << ")" << endl;
                // return 1;
                seen.insert(file_name);
            }
        }
    }
    // Join all threads
    for (thread& t : threads) {
        t.join();
    }
    if (control_thread_started) {
        control_thread.join();
    }

    // Remove transient IPC bookkeeping (pids dir). Trace outputs (output_<tid>)
    // and hugepage files (/dev/hugepages/shared_mem_<tid>) are handled elsewhere.
    std::error_code ec;
    std::filesystem::remove_all(pid_dir, ec);
    if (ec) {
        std::cerr << "Consumer: Warning: failed to clean " << pid_dir
                  << ": " << ec.message() << endl;
    }

    std::cerr << "Consumer: All threads finished" << endl;

    // Final compression ratio
    if (use_compression) {
        std::cerr << "Consumer: Final compression ratio: " << (double)in_size / out_size << endl;
    }
    return 0;
}