// Key data structures for BulletTime
#include <unordered_map>
#include <set>
#include <list>
#include <vector>
#include <string>
#include "pin.H"
#include "common.h"
#include "buffers.h"
#include "statistics.h"
#include "pretty_table.h"

using std::unordered_map;
using std::list;
using std::set;
using std::string;
using std::ofstream;

// Time start global
UINT64 start_time_us;

// Process PID global
NATIVE_PID native_pid;

// Buffer-manager singleton (kept for the free-buffer infrastructure in app_representative.cpp)
BUFFER_LIST_MANAGER* fullBuffersListManager = NULL;

// Buffer Labels — BulletTime uses a single MEMREF buffer type
BUFFER_ID buf_ids[NUM_BUF_TYPES];
size_t bufSizes[NUM_BUF_TYPES] = {sizeof(MEMREF)};
UINT64 buf_entry_limit;  // Number of MEMREFs that fit in one buffer

// Statistics
OVERALL_STATISTICS overallStatistics;

// Track total memory accesses profiled
UINT64 profiledAccesses = 0;
UINT64 profileLimit;
BOOL trace_started = FALSE;
PIN_MUTEX trace_mutex;

// Boolean indicating if we're tracking number of times a function is called
BOOL track_fcalls = false;
std::vector<std::string> fcall_names;

// Indicates we're tracking pthread mutex activity
BOOL pthread_tracking = false;

// Helpful declarations
static VOID PrepareForFini(VOID* v);
static VOID EndTrace(VOID* arg);
static BOOL CheckSkip(UINT64 numElements);

// --- High-Resolution Timer ---
// Uses the RDTSCP instruction to get a high-resolution timestamp and processor ID.
static inline UINT64 Rdtscp() {
    UINT32 lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((UINT64)hi << 32) | lo;
}

// ============================================================================
// Pin-Specific Data
// ============================================================================
// The Pin TLS slot that an application-thread uses to hold its APP_THREAD_REPRESENTITVE.
TLS_KEY appThreadRepresentitiveKey;

// ============================================================================
// Buffer filling callbacks
// ============================================================================
#define BufFull(TYPE) \
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive = \
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid)); \
    ASSERTX(appThreadRepresentitive != NULL); \
    VOID* nextBuffToFill = appThreadRepresentitive->EnqueueFullAndGetNextToFill(buf, numElements, TYPE, buf_ids[TYPE]); \
    return (nextBuffToFill)
VOID* BufferFull_PC(BUFFER_ID id, THREADID tid, const CONTEXT* ctxt, VOID* buf, UINT64 numElements, VOID* v)
{
    if (CheckSkip(numElements)) return buf;
    BufFull(MEMREF_BUF);
}


// ============================================================================
// Instruction Distributions and Stack Frame Collection
// ============================================================================
std::map<ADDRINT, string> pc_ins_string;
std::map<ADDRINT, string> pc_ins_function;
std::map<ADDRINT, double> pc_ins_ratio;

/*
 * Callback to record frequency of memory-accessing instructions
 */
VOID PIN_FAST_ANALYSIS_CALL RecordFrequency(THREADID tid, ADDRINT pc){
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive =
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    ASSERTX(appThreadRepresentitive != NULL);
    appThreadRepresentitive->Statistics()->IncrementInsFreq(pc);
}
VOID PIN_FAST_ANALYSIS_CALL RecordInsExecuted(THREADID tid, UINT64 ins, UINT64 mem_ins){
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive =
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    ASSERTX(appThreadRepresentitive != NULL);
    appThreadRepresentitive->Statistics()->IncrementInsExecuted(ins, mem_ins);
}

// Callback to increment number of pause instructions executed by this thread
VOID PIN_FAST_ANALYSIS_CALL ThreadPause(THREADID tid)
{
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive =
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    if (appThreadRepresentitive) {
        appThreadRepresentitive->Statistics()->IncPauseInstructions();
    }
}

// Timer Callbacks
VOID PIN_FAST_ANALYSIS_CALL TimerStart(THREADID tid) {
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive =
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    if (appThreadRepresentitive) {
        appThreadRepresentitive->Statistics()->timer = Rdtscp();
    }
}
VOID PIN_FAST_ANALYSIS_CALL TimerEnd(THREADID tid) {
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive =
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    if (appThreadRepresentitive) {
        UINT64 end_time = Rdtscp();
        UINT64 elapsed = end_time - appThreadRepresentitive->Statistics()->timer;
        appThreadRepresentitive->Statistics()->AddPthreadLockTime(elapsed);
    }
}

// Callback to count number of calls to a function
VOID PIN_FAST_ANALYSIS_CALL CountFunctionCalls(THREADID tid) {
    APP_THREAD_REPRESENTITVE* appThreadRepresentitive =
        static_cast< APP_THREAD_REPRESENTITVE* >(PIN_GetThreadData(appThreadRepresentitiveKey, tid));
    if (appThreadRepresentitive) {
        appThreadRepresentitive->fcalls_executed++;
    }
}

// ============================================================================
// Output Buffer Manager (for the free-buffer list shared with app_representative)
// ============================================================================
BUFFER_LIST_MANAGER* GetFullBuffersListManager()
{
    static BUFFER_LIST_MANAGER buffersListManager(TRUE);
    return &buffersListManager;
}
