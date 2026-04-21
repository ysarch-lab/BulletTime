#ifndef STATISTICS_H
#define STATISTICS_H

#include <iostream>
#include <numeric>
#include <execinfo.h>

#include "common.h"
#include "pin.H"

using std::string;
using std::ofstream;

class APP_THREAD_STATISTICS;
class OVERALL_STATISTICS
{
  public:
    OVERALL_STATISTICS() {};
    VOID Init(string outfile);
    VOID AccumulateAppThreadStatistics(APP_THREAD_STATISTICS* statistics);
    VOID Dump();
    VOID StartTime();
    VOID EndTime();

  private:
    UINT64 _numElementsProcessed;
    UINT32 _numBuffersFilled;
    UINT32 _numBuffersProcessedInAppThread;
    UINT64 _pause_instructions = 0;
    UINT64 _pthread_lock_time = 0;
    UINT64 _progTime = 0;
    UINT64 _ins_executed = 0;
    UINT64 _mem_ins_executed = 0;
    BOOL   _timeSet = false;
    PIN_MUTEX _mutex;
    FILE* _fp;

  public:
    std::map<ADDRINT, UINT64> ins_freqs;
    std::vector<std::map<ADDRINT, UINT64>> per_thread_ins_freqs;
};

class APP_THREAD_STATISTICS
{
  public:
    APP_THREAD_STATISTICS();

    VOID AddNumElementsProcessed(UINT64 numElementsProcessed);
    VOID IncPauseInstructions();
    VOID AddPthreadLockTime(UINT64 time);
    VOID IncrementInsFreq(ADDRINT insAddr);
    VOID IncrementInsExecuted(UINT64 ins, UINT64 mem_ins);

    // Non-locking increments and accessors
    VOID IncrementNumBuffersProcessedInAppThread() {_numBuffersProcessedInAppThread++; }
    VOID IncrementNumBuffersFilled() { _numBuffersFilled++; }
    UINT32 NumBuffersProcessedInAppThread() { return _numBuffersProcessedInAppThread; }
    UINT64 NumElementsProcessed() { return _numElementsProcessed; }
    UINT32 NumBuffersFilled() { return _numBuffersFilled; }
    UINT64 PauseInstructions() { return _pause_instructions; }
    UINT64 PthreadLockTime() { return _pthread_lock_time; }
    UINT64 InsExecuted() { return _ins_executed; }
    UINT64 MemInsExecuted() { return _mem_ins_executed; }
    std::map<ADDRINT, UINT64> InsFreqs() { return _ins_freqs; }

    // Space for rdtsc timer
    UINT64 timer = 0;

  private:
    UINT64 _numElementsProcessed;
    FILE* _fp;
    UINT32 _numBuffersProcessedInAppThread;
    UINT32 _numBuffersFilled;
    UINT64 _pause_instructions = 0;
    UINT64 _pthread_lock_time = 0;
    UINT64 _ins_executed = 0;
    UINT64 _mem_ins_executed = 0;
    std::map<ADDRINT, UINT64> _ins_freqs;
    PIN_MUTEX _mutex;
};

#endif
