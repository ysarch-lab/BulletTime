#include "statistics.h"

VOID OVERALL_STATISTICS::Init(string outfile)
{
    if (outfile.compare("") != 0)
        _fp = fopen(outfile.c_str(), "w");
    else
        _fp = stderr;

    _numElementsProcessed           = 0;
    _numBuffersFilled               = 0;
    _numBuffersProcessedInAppThread = 0;
    _pause_instructions             = 0;
    _pthread_lock_time              = 0;
    _timeSet                        = FALSE;
    ins_freqs                       = std::map<ADDRINT, UINT64>();
    per_thread_ins_freqs            = std::vector<std::map<ADDRINT, UINT64>>();
    OS_Time(&_progTime);
    PIN_MutexInit(&_mutex);
}

VOID OVERALL_STATISTICS::Dump(){
    double data_rate = (double)(_numElementsProcessed * sizeof(MEMREF)) / _progTime / 1000; // GB/s
    double total_time = (double)_progTime / 1000000; // s
    double pthread_lock_e9 = (double)_pthread_lock_time / 1000000000;

    fprintf(_fp, "\n\nBulletTime: OVERALL STATISTICS\n");
    fprintf(_fp, "  Total Time Profiled (s)            %lf\n", total_time);
    fprintf(_fp, "  Memory Accesses Processed          %lu\n", _numElementsProcessed);
    fprintf(_fp, "  Raw Data Rate (GB/s)               %f\n", data_rate);
    fprintf(_fp, "  Non-Memory Instructions Executed   %lu\n", _ins_executed);
    fprintf(_fp, "  Memory Instructions Executed       %lu\n", _numElementsProcessed);
    fprintf(_fp, "  Pause Instructions                 %lu\n", _pause_instructions);
    fprintf(_fp, "  Pthread Lock Time (1e9 cycles)     %lf\n", pthread_lock_e9);
    fclose(_fp);
    PIN_MutexFini(&_mutex);
}

VOID OVERALL_STATISTICS::AccumulateAppThreadStatistics(APP_THREAD_STATISTICS* statistics)
{
    PIN_MutexLock(&_mutex);
    _ins_executed += statistics->InsExecuted();
    _mem_ins_executed += statistics->MemInsExecuted();
    _numElementsProcessed += statistics->NumElementsProcessed();
    _numBuffersFilled += statistics->NumBuffersFilled();
    _numBuffersProcessedInAppThread += statistics->NumBuffersProcessedInAppThread();
    _pause_instructions += statistics->PauseInstructions();
    _pthread_lock_time += statistics->PthreadLockTime();

    // Accumulate instruction frequencies
    std::map<ADDRINT, UINT64> thread_ins_freqs = statistics->InsFreqs();
    if (!thread_ins_freqs.empty()) {
        per_thread_ins_freqs.push_back(thread_ins_freqs);
        for (auto it = thread_ins_freqs.begin(); it != thread_ins_freqs.end(); ++it)
        {
            ADDRINT pc = it->first;
            UINT64 count = it->second;
            if (ins_freqs.find(pc) == ins_freqs.end())
                ins_freqs[pc] = count;
            else
                ins_freqs[pc] += count;
        }
    }
    PIN_MutexUnlock(&_mutex);
}

VOID OVERALL_STATISTICS::StartTime()
{
    OS_Time(&_progTime);
}
VOID OVERALL_STATISTICS::EndTime()
{
    if (_timeSet)
        return;
    UINT64 endTime;
    OS_Time(&endTime);
    _progTime = endTime - _progTime;
    _timeSet = TRUE;
}


// App thread statistics
APP_THREAD_STATISTICS::APP_THREAD_STATISTICS()
{
    _numBuffersFilled               = 0;
    _numBuffersProcessedInAppThread = 0;
    _numElementsProcessed           = 0;
    _ins_freqs                      = std::map<ADDRINT, UINT64>();
    _pause_instructions             = 0;
    _pthread_lock_time              = 0;
    _ins_executed                   = 0;
    _mem_ins_executed               = 0;
    PIN_MutexInit(&_mutex);
}
VOID APP_THREAD_STATISTICS::AddNumElementsProcessed(UINT64 numElementsProcessed)
{
    PIN_MutexLock(&_mutex);
    _numElementsProcessed += numElementsProcessed;
    PIN_MutexUnlock(&_mutex);
}

// No locking — only called from the owning thread
VOID APP_THREAD_STATISTICS::IncPauseInstructions()
{
    _pause_instructions++;
}
VOID APP_THREAD_STATISTICS::AddPthreadLockTime(UINT64 time)
{
    _pthread_lock_time += time;
}
VOID APP_THREAD_STATISTICS::IncrementInsFreq(ADDRINT insAddr)
{
    if (_ins_freqs.find(insAddr) == _ins_freqs.end())
        _ins_freqs[insAddr] = 1;
    else
        _ins_freqs[insAddr]++;
}
VOID APP_THREAD_STATISTICS::IncrementInsExecuted(UINT64 ins, UINT64 mem_ins)
{
    _ins_executed += ins;
    _mem_ins_executed += mem_ins;
}
