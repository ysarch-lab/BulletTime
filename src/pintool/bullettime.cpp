/*
 * Copyright (c) 2026 BulletTime contributors.
 * SPDX-License-Identifier: MIT
 */

/*
 * BulletTime: Pin-based memory access tracing tool with time dilation support.
 *  - Collects the IP of memory accessing instructions, address of memory access, size
 *    of memory access, and whether the access is a read or write.
 *  - Each application thread uses the Pin buffering API to allocate a set of buffers to fill with
 *    memory references (a full MEMREF struct per access).
 *  - When a buffer is full, its contents are handed to the consumer process via shared memory.
 *    The consumer handles compression (optional) and disk I/O, and also drives the time-dilation
 *    control plane.
 *  - Pin blocks on a ready signal from the consumer after each buffer, so injected dilation delays
 *    on the consumer side are experienced by the application thread.
 *  - All functions use the generic Pin API.
 */

#include <cstdio>
#include <iostream>
#include <fstream>
#include <sstream>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <atomic>


// Globals, functions, etc.
#include "bullettime.h"

// Shared with consumer
#include "../include/consumer.h"

// Required: output directory (for shared-memory IPC with consumer)
KNOB< string > KnobOutPrefix(KNOB_MODE_WRITEONCE, "pintool", "outprefix", "", "directory for shared-memory IPC with consumer");

// Buffer size (must match consumer's CONSUMER_BUFFER_SIZE)
KNOB< UINT32 > KnobNumPagesInBuffer(KNOB_MODE_WRITEONCE, "pintool", "bpages", CONSUMER_BUFFER_PAGES, "number of 4KiB pages per buffer");

// Optional profiling knobs
KNOB< string > KnobCountFCalls(KNOB_MODE_WRITEONCE, "pintool", "fcalls", "", "comma-separated function names to count per-thread (identifies key threads)");
KNOB< INT32  > KnobFCallsExact(KNOB_MODE_WRITEONCE, "pintool", "fcalls_exact", "0", "require exact match on -fcalls names (0: substring match, 1: exact match)");
KNOB< INT32  > KnobProfile(KNOB_MODE_WRITEONCE, "pintool", "prof", "-1", "# of memory accesses to profile (millions), -1 for all");
KNOB< string > KnobRecordFreq(KNOB_MODE_WRITEONCE, "pintool", "record_file", "", "record frequency of each memory-accessing instruction");
KNOB< string > KnobProfileOutFile(KNOB_MODE_WRITEONCE, "pintool", "stat_file", "", "stats output file");
KNOB< INT32  > KnobProfSync(KNOB_MODE_WRITEONCE, "pintool", "prof_sync", "0", "profile synchronization operations (0: no, 1: yes)");

// Help message
INT32 Usage()
{
    printf("BulletTime: Pin-based memory access tracing with time dilation\n");
    printf("The following command line options are available:\n");
    printf("-outprefix <prefix>     :shared-memory IPC directory (required)\n");
    printf("-bpages <num>           :number of 4KiB pages per buffer (must match consumer)\n");
    printf("-fcalls <name[,name...]> :comma-separated function names, counted per-thread (identifies key threads for dilation)\n");
    printf("-fcalls_exact <0-1>     :require exact match on -fcalls names (0: substring match)\n");
    printf("-prof <num>             :number of memory accesses to trace in millions (-1 for all)\n");
    printf("-record_file <filename> :per-PC instruction frequency output\n");
    printf("-stat_file <filename>   :stats output file\n");
    printf("-prof_sync <0-1>        :profile pthread_mutex pause instructions\n");
    return -1;
}


// Updates profiled access counters and calls EndTrace if the limit is reached
//  - Used in BufferFull callbacks (check header file)
BOOL CheckSkip(UINT64 numElements){
    // Track when tracing begins
    if (!trace_started){
        if (PIN_MutexTryLock(&trace_mutex)){
            overallStatistics.StartTime();
            trace_started = TRUE;
            printf("BulletTime: Starting trace\n");
            fflush(stdout);
            PIN_MutexUnlock(&trace_mutex);
        }
    }

    // If profiling a finite number of memory accesses, check if the limit is reached --> if so, return early
    if (KnobProfile > 0) {
        if (profiledAccesses >= profileLimit){
            // Do not unlock to stop other threads from also printing the exit message
            if (PIN_MutexTryLock(&trace_mutex)){
                printf("BulletTime: %lu memory accesses profiled, exiting\n", profiledAccesses);
                fflush(stdout);

                // Spawn internal thread to call EndTrace
                PIN_THREAD_UID uid;
                PIN_SpawnInternalThread(EndTrace, NULL, 0, &uid);
            }
        }
        profiledAccesses += numElements;
    }
    return FALSE;
}

// Copy data to shared memory with socket communication protocol
VOID CopyToSharedMemory(VOID* buf, UINT64 numBytes, APP_THREAD_REPRESENTITVE* associatedAppThread,
                        UINT32 buf_type) {
    int sock_rdy = associatedAppThread->_sock_rdy;
    int sock_size = associatedAppThread->_sock_size;
    if (sock_rdy == -1 || sock_size == -1) {
        fprintf(stderr, "Socket file descriptors not initialized\n");
        fflush(stderr);
        exit(-1);
    }

    VOID* shared_mem = associatedAppThread->_shared_mem;
    UINT64 shared_mem_size = associatedAppThread->_shared_mem_size;
    if (shared_mem == NULL || shared_mem_size == 0) {
        fprintf(stderr, "Shared memory not initialized\n");
        fflush(stderr);
        exit(-1);
    }

    // Measure the time it took to generate the trace data
    UINT64 gen_time;
    OS_Time(&gen_time);
    gen_time -= associatedAppThread->_buf_gen_time[buf_type];

    // 1) receive ready signal from sock_rdy to start writing
    UINT64 rdy_wait_start, rdy_wait_end;
    OS_Time(&rdy_wait_start);
    size_t rdy_buf[1];
    int rdy_bytes = read(sock_rdy, rdy_buf, sizeof(rdy_buf));
    OS_Time(&rdy_wait_end);
    UINT64 pin_wait_us = rdy_wait_end - rdy_wait_start;
    if (rdy_bytes == -1){
        std::cerr << "Failed to receive ready signal" << std::endl;
        return;
    }

    // 2) Write to shared memory
    UINT64 mcpy_start, mcpy_end;
    OS_Time(&mcpy_start);
    memcpy(shared_mem, buf, numBytes);
    std::atomic_thread_fence(std::memory_order_release); // Ensure writes are visible
    OS_Time(&mcpy_end);

    // 3) Send the following information:
    //      1) Number of bytes copied
    //      2) Time it took to generate trace data (wall clock)
    //      3) Amount of "work" performed
    //      4) # Instructions executed creating this buffer
    //      5) # Memory instructions executed
    //      6) Units of user-defined work (fcalls), -1 if not counting
    UINT64 mem_ins_executed = associatedAppThread->Statistics()->MemInsExecuted();
    UINT64 ins_executed = associatedAppThread->Statistics()->InsExecuted();
    UINT64 fcalls_executed = associatedAppThread->fcalls_executed - associatedAppThread->last_fcalls_executed;
    mem_ins_executed -= associatedAppThread->last_mem_ins_executed;
    ins_executed -= associatedAppThread->last_ins_executed;
    associatedAppThread->last_mem_ins_executed += mem_ins_executed;
    associatedAppThread->last_ins_executed += ins_executed;
    associatedAppThread->last_fcalls_executed = associatedAppThread->fcalls_executed;

    size_t send_data[7];
    send_data[0] = numBytes; // Size of the data
    send_data[1] = gen_time + (mcpy_end - mcpy_start); // Time taken to generate and send the data
    if (track_fcalls) {
        send_data[2] = fcalls_executed; // Number of function calls executed
    } else {
        send_data[2] = ins_executed;
    }

    send_data[3] = ins_executed; // Number of non-memory instructions executed creating this buffer
    send_data[4] = mem_ins_executed; // Number of memory instructions executed
    send_data[5] = fcalls_executed; // Number of function calls executed
    send_data[6] = pin_wait_us; // Time Pin waited for consumer ready signal (us)
    
    int size_bytes = send(sock_size, send_data, sizeof(send_data), 0);
    if (size_bytes == -1){
        std::cerr << "Thread " << associatedAppThread->_myTid << " failed to send size " << numBytes << " bytes" << std::endl;
    }

    // Reset buffer gen timer
    associatedAppThread->_buf_gen_time[buf_type] = mcpy_end;
}

// Hand the buffer off to the consumer via shared memory.
// In BulletTime mode, this is the only output path — the consumer handles any compression and disk I/O.
VOID ProcessBuffer(VOID* buf, UINT64 numElements, APP_THREAD_REPRESENTITVE* associatedAppThread, UINT32 index, BOOL in_worker,
                   VOID* lzo_mem, UINT32 buf_type)
{
    size_t numBytes = numElements * bufSizes[buf_type];
    if (KnobOutPrefix.Value().compare("") != 0) {
        CopyToSharedMemory(buf, numBytes, associatedAppThread, buf_type);
    }
}

/*
 * Trace instrumentation routine invoked by Pin when jitting a trace
 * Insert code to write data to a thread-specific buffer for instructions
 * that access memory.
 */
VOID Trace(TRACE trace, VOID* v)
{
    // Insert a call to record memory addresses
    //  - Coarse grained memory access ratios
    UINT64 trace_ins = 0;
    UINT64 trace_mem_ins = 0;
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        // Iterate over instructions in basic block
        //  - Count the ratio of instrumented instructions
        UINT64 num_ins = 0;
        UINT64 num_mem_ins = 0;
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
            num_ins++;

            // Record frequency of memory-accessing instructions
            if ((INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins))) {
                num_mem_ins++;
                if (KnobRecordFreq.Value().compare("") != 0) {
                    // Get function name
                    string fname;
                    RTN rtn = INS_Rtn(BBL_InsHead(TRACE_BblHead(trace)));
                    if (RTN_Valid(rtn)){
                        fname = RTN_Name(rtn);
                    }
                    else fname = "unknown";

                    std::string disassembled = INS_Disassemble(ins);
                    pc_ins_string[INS_Address(ins)] = disassembled;
                    pc_ins_function[INS_Address(ins)] = fname;

                    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordFrequency, IARG_FAST_ANALYSIS_CALL,
                                IARG_THREAD_ID, IARG_INST_PTR, IARG_END);
                }
            }

            // Record frequency of pause instructions
            if (KnobProfSync.Value() == 1) {
                std::string disassembled = INS_Disassemble(ins);
                if (disassembled.find("pause") != std::string::npos) {
                    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)ThreadPause, IARG_THREAD_ID, IARG_END);
                }
            }

            // Instrument memory accesses — single full MEMREF struct per access
            if (INS_IsMemoryRead(ins)) {
                INS_InsertFillBuffer(ins, IPOINT_BEFORE, buf_ids[0],
                        IARG_INST_PTR, offsetof(struct MEMREF, pc),
                        IARG_MEMORYREAD_EA, offsetof(struct MEMREF, ea),
                        IARG_MEMORYREAD_SIZE, offsetof(struct MEMREF, sz),
                        IARG_UINT32, 1, offsetof(struct MEMREF, is_read), IARG_END);
            }

            if (INS_IsMemoryWrite(ins)) {
                INS_InsertFillBuffer(ins, IPOINT_BEFORE, buf_ids[0],
                        IARG_INST_PTR, offsetof(struct MEMREF, pc),
                        IARG_MEMORYWRITE_EA, offsetof(struct MEMREF, ea),
                        IARG_MEMORYWRITE_SIZE, offsetof(struct MEMREF, sz),
                        IARG_UINT32, 0, offsetof(struct MEMREF, is_read), IARG_END);
            }
        }

        // End of basic block - record executed instructions
        ASSERTX(num_ins >= num_mem_ins);
        BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)RecordInsExecuted, IARG_FAST_ANALYSIS_CALL,
                        IARG_THREAD_ID, IARG_UINT64, num_ins, IARG_UINT64, num_mem_ins, IARG_END);
        trace_ins += num_ins;
        trace_mem_ins += num_mem_ins;
    }

    // Attach trace instruction ratios to trace
    if (KnobRecordFreq.Value().compare("") != 0) {
        for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
            for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
                pc_ins_ratio[INS_Address(ins)] = (double)trace_mem_ins / trace_mem_ins;
            }
        }
    }
}

// This function is called by Pin for every image loaded by the application.
// Find functions related to pthread_mutex and time their execution
VOID ImageLoad(IMG img, VOID *v) {
    std::string img_name = IMG_Name(img);

    // Iterate over all routines in the image.
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn)) {
            std::string rtn_name = RTN_Name(rtn);

            // Find pthread_mutex stats
            if (rtn_name.find("pthread_mutex_lock")!= std::string::npos && pthread_tracking) {
                RTN_Open(rtn);

                // Instrument at the beginning of the function.
                RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)TimerStart, IARG_THREAD_ID, IARG_END);

                // Instrument at every return point of the function.
                RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)TimerEnd, IARG_THREAD_ID, IARG_END);

                RTN_Close(rtn);
            }

            // Find the function(s) we're counting calls of
            if (track_fcalls) {
                const bool exact = (KnobFCallsExact.Value() != 0);
                bool matched = false;
                for (const auto& n : fcall_names) {
                    if (exact ? (rtn_name == n) : (rtn_name.find(n) != std::string::npos)) {
                        matched = true;
                        break;
                    }
                }
                if (matched) {
                    RTN_Open(rtn);
                    // Instrument at the beginning of the function.
                    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)CountFunctionCalls, IARG_THREAD_ID, IARG_END);
                    RTN_Close(rtn);
                    std::cerr << "BulletTime: Found function " << rtn_name << std::endl;
                }
            }
        }
    }
}

/**************************************************************************
 *
 *  Callback Routines
 *
 **************************************************************************/
VOID ThreadStart(THREADID tid, CONTEXT* ctxt, INT32 flags, VOID* v)
{
    // There is a new APP_THREAD_REPRESENTITVE for every thread.
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive = new APP_THREAD_REPRESENTITVE(tid);
    // printf("BulletTime: Application thread %d starting\n", tid);
    // fflush(stdout);

    // A thread will need to look up its APP_THREAD_REPRESENTITVE, so save pointer in TLS
    PIN_SetThreadData(appThreadRepresentitiveKey, appThreadRepresentitive, tid);

    // Get pointers to thread-specific buffers
    for (UINT32 i = 0; i < NUM_BUF_TYPES; i++){
        VOID* buf = PIN_GetBufferPointer(ctxt, buf_ids[i]);
        appThreadRepresentitive->_currentBufs[i] = buf;
        ASSERTX(appThreadRepresentitive->_currentBufs[i] != NULL);

        // Add execute permissions to buffer to differentiate mapping
        size_t buf_size = bufSizes[i] * buf_entry_limit;
        OS_ProtectMemory(native_pid, buf, buf_size,
            OS_PAGE_PROTECTION_TYPE_READ | OS_PAGE_PROTECTION_TYPE_WRITE | OS_PAGE_PROTECTION_TYPE_EXECUTE);

        // Force physical allocation of buffer
        memset(buf, 0, buf_size);

        // Allocate other app thread buffers
        appThreadRepresentitive->AllocateBuffers(i, buf_ids[i]);
    }

    // Initialize buffer generation timers
    UINT64 start;
    OS_Time(&start);
    for (UINT32 i = 0; i < NUM_BUF_TYPES; i++){
        appThreadRepresentitive->_buf_gen_time[i] = start;
    }


    // Set up shared-memory IPC with the consumer process:
    //  - Write a file <prefix_dir>/pids/<prefix>-<native_tid>.<tid> so the consumer notices this thread
    //  - Connect to two Unix-domain sockets the consumer has created:
    //      - `rdy_<tid>`:  consumer -> pin, "ready for next buffer"
    //      - `size_<tid>`: pin -> consumer, metadata about the buffer just written
    //  - mmap the consumer's /dev/hugepages/shared_mem_<tid> (or /dev/shm fallback)
    if (KnobOutPrefix.Value().compare("") != 0){
        NATIVE_TID native_tid;
        OS_GetTid(&native_tid);

        // Split output prefix into parent directory and file prefix
        size_t found = KnobOutPrefix.Value().find_last_of("/");
        string dir = KnobOutPrefix.Value().substr(0, found);
        string prefix = KnobOutPrefix.Value().substr(found + 1);
        string pid_dir = dir + "/pids/";
        prefix = prefix + "-" + decstr(native_tid) + "." + decstr(tid);
        string fname = pid_dir + prefix;

        // std::cerr << "Creating file " << fname << std::endl;
        std::ofstream out(fname);
        out << prefix;
        out.close();

        // Connect to sockets created by the consumer process
        int sock_rdy, sock_size;
        sock_rdy = socket(AF_UNIX, SOCK_STREAM, 0);
        sock_size = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_rdy == -1 || sock_size == -1){
            std::cerr << "Failed to create sockets for thread " << tid << std::endl;
            exit(-1);
        }

        string rdy_name = dir + "/rdy_" + decstr(tid);
        string size_name = dir + "/size_" + decstr(tid);
        struct sockaddr_un rdy_addr, size_addr;
        memset(&rdy_addr, 0, sizeof(rdy_addr));
        memset(&size_addr, 0, sizeof(size_addr));
        rdy_addr.sun_family = AF_UNIX;
        size_addr.sun_family = AF_UNIX;
        strncpy(rdy_addr.sun_path, rdy_name.c_str(), sizeof(rdy_addr.sun_path) - 1);
        strncpy(size_addr.sun_path, size_name.c_str(), sizeof(size_addr.sun_path) - 1);
        // std::cerr << "Connecting to sockets " << rdy_name << " and " << size_name << std::endl;
        while (connect(sock_rdy, (struct sockaddr*)&rdy_addr, sizeof(struct sockaddr_un)) == -1 ||
            connect(sock_size, (struct sockaddr*)&size_addr, sizeof(struct sockaddr_un)) == -1){
        }

        // Map shared memory file
        string shared_mem = "/dev/hugepages/shared_mem_" + decstr(tid);
        NATIVE_FD shm_fd;
        OS_RETURN_CODE srdy = OS_OpenFD(shared_mem.c_str(), 0b111, 0777, &shm_fd);
        
        // Fallback to standard shared memory if hugepages isn't used or fails
        if (srdy.generic_err != OS_RETURN_CODE_NO_ERROR) {
            shared_mem = "/dev/shm/shared_mem_" + decstr(tid);
            srdy = OS_OpenFD(shared_mem.c_str(), 0b111, 0777, &shm_fd);
            
            if (srdy.generic_err != OS_RETURN_CODE_NO_ERROR) {
                std::cerr << "Failed to open shared memory file for thread " << tid 
                          << " in both /dev/hugepages/ and /dev/shm/" << std::endl;
                exit(-1);
            }
        }
        // std::cerr << "Mapping shared memory file " << shared_mem << std::endl;
        VOID* mem_area = NULL;
        size_t mem_size = ALLOCATION_SIZE;
        srdy = OS_MapFileToMemory(native_pid, 0b010, mem_size, OS_MEMORY_FLAGS_SHARED, shm_fd, 0, &mem_area);
        if (srdy.generic_err != OS_RETURN_CODE_NO_ERROR){
            std::cerr << "Failed to map shared memory file for thread " << tid << std::endl;
            exit(-1);
        }
        // std::cerr << "Map successful" << std::endl;

        // Set thread data
        appThreadRepresentitive->_sock_rdy = sock_rdy;
        appThreadRepresentitive->_sock_size = sock_size;
        appThreadRepresentitive->_shared_mem = mem_area;
        appThreadRepresentitive->_shared_mem_size = mem_size;
        appThreadRepresentitive->_shm_fd = shm_fd;
    }
}

VOID ThreadFini(THREADID tid, const CONTEXT* ctxt, INT32 code, VOID* v)
{
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive =
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    ASSERTX(appThreadRepresentitive != NULL);

    // wait for all my buffers to be processed
    // std::cerr << "BulletTime: Waiting for application thread " << tid << " to finish processing buffers" << std::endl;
    BOOL ok = appThreadRepresentitive->AllBuffersProcessed();
    ASSERTX(ok);

    // Close sockets and unmap shared memory
    if (KnobOutPrefix.Value().compare("") != 0){
        shutdown(appThreadRepresentitive->_sock_rdy, SHUT_WR);
        shutdown(appThreadRepresentitive->_sock_size, SHUT_WR);
        close(appThreadRepresentitive->_sock_rdy);
        close(appThreadRepresentitive->_sock_size);
        OS_CloseFD(appThreadRepresentitive->_shm_fd);
        OS_FreeMemory(native_pid, appThreadRepresentitive->_shared_mem, appThreadRepresentitive->_shared_mem_size);
    }

    // Stats
    overallStatistics.AccumulateAppThreadStatistics(appThreadRepresentitive->Statistics());
    UINT64 memrefs = appThreadRepresentitive->Statistics()->NumElementsProcessed();
    delete appThreadRepresentitive;

    std::cout << "BulletTime: Application thread " << tid << " exiting. Memory references made: " << memrefs << std::endl;
    fflush(stdout);
    PIN_SetThreadData(appThreadRepresentitiveKey, 0, tid);
}

// Function to end application early when profiling a limited number of memory accesses
// - Must be called by a newly spawned internal thread
static VOID EndTrace(VOID* arg){
    // Signal all internal threads to exit and block extra buffers
    PrepareForFini(NULL);

    // Signal process exit
    PIN_ExitApplication(0);
}

// Process exit callback (unlocked).
static VOID PrepareForFini(VOID* v)
{
    overallStatistics.EndTime();
}

static VOID Fini(INT32 code, VOID* v)
{
    if (KnobProfile != 0)
        overallStatistics.Dump();
    
    // Dump instruction distribution
    if (KnobRecordFreq.Value().compare("") != 0) {
        std::ofstream out(KnobRecordFreq.Value());
        std::map<ADDRINT, UINT64> freq = overallStatistics.ins_freqs;

        // Sort by frequency in descending order
        std::vector<std::pair<ADDRINT, UINT64>> freq_vec(freq.begin(), freq.end());
        std::sort(freq_vec.begin(), freq_vec.end(), [](const std::pair<ADDRINT, UINT64>& a, const std::pair<ADDRINT, UINT64>& b){
            return a.second > b.second;
        });

        // Print all instructions of the most frequent instruction's routine
        // std::vector<std::string> ins_vec = rtn_ins[pc_ins_function[freq_vec[0].first]];
        // out << "Printing most frequent instruction's routine: " << pc_ins_function[freq_vec[0].first]
        //         << ", " << ins_vec.size() << " instructions" << std::endl;
        // for (auto it = ins_vec.begin(); it != ins_vec.end(); ++it){
        //     out << "\t" << *it << std::endl;
        // }
        // out << std::endl;

        // Log of all memory accessing instructions
        // Print: [PC: hex], [Frequency: decimal], [Function Name], [Bbl ratio: float], [Instruction string]
        int printed = 0;
        PrettyTable vt({"PC", "Frequency", "Function", "Bbl Ratio", "Instruction"});
        for (auto it = freq_vec.begin(); it != freq_vec.end(); ++it){
            ADDRINT pc = it->first;
            UINT64 count = it->second;

            if (count == 1 || printed++ == 10000) break;
            std::string ins = pc_ins_string[pc];
            std::string fname = pc_ins_function[pc];
            double bbl_ratio = pc_ins_ratio[pc];

            // Cap function name length
            UINT64 max_len = 50;
            if (fname.length() > max_len){
                fname = fname.substr(0, max_len) + "...";
            }

            std::ostringstream ratio_ss;
            ratio_ss << std::fixed << std::setprecision(3) << bbl_ratio;
            vt.add_row({hexstr(pc), std::to_string(count), fname, ratio_ss.str(), ins});
        }
        vt.print(out);
        out.close();
    }

    // Signal the consumer process to exit by creating a "stop" file in the pids directory
    if (KnobOutPrefix.Value().compare("") != 0){
        size_t found = KnobOutPrefix.Value().find_last_of("/");
        string dir = KnobOutPrefix.Value().substr(0, found);
        string stop = dir + "/pids/stop";
        std::ofstream out(stop);
        out << "stop";
        out.close();
    }

    PIN_Detach();
}

/*!
 * The main procedure of the tool.
 * This function is called when the application image is loaded but not yet started.
 * @param[in]   argc            total number of elements in the argv array
 * @param[in]   argv            array of command line arguments,
 *                              including pin -t <toolname> -- ...
 */
int main(int argc, char* argv[])
{
    // Initialize PIN library. Print help message if -h(elp) is specified
    // in the command line or the command line is invalid
    PIN_InitSymbols();
    if (PIN_Init(argc, argv))
    {
        return Usage();
    }

    // Disassembly syntax
    PIN_SetSyntaxIntel();

    // Get the PID of the process
    native_pid = PIN_GetPid();

    // Track times a function is called (optional — identifies key threads for dilation)
    if (KnobCountFCalls.Value().compare("") != 0) {
        track_fcalls = TRUE;
        std::stringstream ss(KnobCountFCalls.Value());
        std::string name;
        while (std::getline(ss, name, ',')) {
            if (!name.empty()) fcall_names.push_back(name);
        }
        const char* mode = (KnobFCallsExact.Value() != 0) ? "exact" : "substring";
        std::cout << "BulletTime: Counting calls to " << fcall_names.size()
                  << " function name(s) [" << mode << " match]" << std::endl;
    }

    // Track pause instructions in user mutex code paths
    if (KnobProfSync.Value() > 0) {
        pthread_tracking = TRUE;
    }

    // Define a single trace buffer holding full MEMREF structs.
    // The buffer is implicitly allocated to each application thread by Pin when the
    // thread starts; when it fills, the BufferFull_PC callback hands it to the consumer.
    buf_ids[0] = PIN_DefineTraceBuffer(sizeof(MEMREF), KnobNumPagesInBuffer, BufferFull_PC, 0);
    bufSizes[0] = sizeof(MEMREF);
    buf_entry_limit = (UINT64)KnobNumPagesInBuffer.Value() * 4096 / sizeof(MEMREF);

    // Initialize Pin TLS slot used by the application threads to store and
    // retrieve the APP_THREAD_REPRESENTITVE object that they own
    appThreadRepresentitiveKey = PIN_CreateThreadDataKey(0);

    // Callbacks
    TRACE_AddInstrumentFunction(Trace, 0);
    IMG_AddInstrumentFunction(ImageLoad, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_AddPrepareForFiniFunction(PrepareForFini, 0);

    // Profile limit (optional — cap tracing at a given number of memory accesses)
    if (KnobProfile.Value() > 0){
        printf("BulletTime: Profiling at least %d million memory accesses\n", KnobProfile.Value());
        profileLimit = (unsigned long) KnobProfile.Value() * 1000000;
    }
    else {
        profileLimit = 0;
    }
    PIN_MutexInit(&trace_mutex);

    // Create the <outprefix_dir>/pids directory that signals thread presence to the consumer
    if (KnobOutPrefix.Value().compare("") != 0){
        size_t found = KnobOutPrefix.Value().find_last_of("/\\");
        string dir = KnobOutPrefix.Value().substr(0, found) + "/pids";
        OS_RETURN_CODE err = OS_MkDir(dir.c_str(), 0777);
        if (err.generic_err != OS_RETURN_CODE_NO_ERROR && err.generic_err != OS_RETURN_CODE_FILE_EXIST){
            std::cerr << "Failed to create directory " << dir << std::endl;
            exit(-1);
        }
    }

    overallStatistics.Init(KnobProfileOutFile.Value().c_str());

    // Get time and start the program, never returns
    OS_Time(&start_time_us);
    PIN_StartProgram();

    return 0;
}
