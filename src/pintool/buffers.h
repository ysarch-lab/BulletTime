// Key data structures for BulletTime
#ifndef BUFFERS_H
#define BUFFERS_H

#include <cstdio>
#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <list>
#include "pin.H"
#include "statistics.h"

using std::list;
using std::string;

// Declare classes
class APP_THREAD_REPRESENTITVE;
class BUFFER_LIST_MANAGER;

// Globals
extern NATIVE_PID native_pid;
extern UINT64 buf_entry_limit;
extern BUFFER_ID buf_ids[NUM_BUF_TYPES];
extern size_t bufSizes[NUM_BUF_TYPES];
extern BUFFER_LIST_MANAGER* fullBuffersListManager;

VOID ProcessBuffer(VOID* buf, UINT64 numElements, APP_THREAD_REPRESENTITVE* associatedAppThread, UINT32 index, BOOL stallable, VOID* lzo_mem, UINT32 buf_type);

/*
 * BUFFER_LIST_MANAGER
 * This class implements buffer list management, both for the global fullBuffers list
 * and for the per-app-thread bufferBuffersList
 */
class BUFFER_LIST_MANAGER
{
  public:
    BUFFER_LIST_MANAGER(BOOL notifyExitRequired = FALSE);
    ~BUFFER_LIST_MANAGER();

    BOOL PutBufferOnList(VOID* buf, UINT64 numElements,
                         /* the thread that owns the buffer */
                         APP_THREAD_REPRESENTITVE* appThreadRepresentitive,
                         /* thread Id of the thread making the call */
                         THREADID tid,
                         /* buffer index (for tracing) */
                         UINT32 index, UINT32 bufferType, UINT32 buf_field, UINT64* sync_time);
    VOID* GetBufferFromList(UINT64* numElements,
                            /* the thread that owns the buffer */
                            APP_THREAD_REPRESENTITVE** appThreadRepresentitive,
                            /* buffer index */
                            UINT32* index,
                            /* thread Id of the thread making the call */
                            THREADID tid, UINT32 *buf_field, UINT64 *sync_time);
    VOID NotifyExit();
    VOID ClearBufferSem() { PIN_SemaphoreClear(_bufferSem); }
    BOOL BufferStatus() { return PIN_SemaphoreIsSet(_bufferSem); }
    BOOL ExitStatus() { return PIN_SemaphoreIsSet(_exitEvent); }
    BOOL IsEmpty() { return _bufferList.empty(); }

  private:
    // structure of an element of the buffer list
    struct BUFFER_LIST_ELEMENT
    {
        VOID* buf;
        UINT64 numElements;
        // the application thread that puts this buffer on the list
        APP_THREAD_REPRESENTITVE* appThreadRepresentitive;
        UINT32 index;
        UINT32 buf_field;
    };
    PIN_SEMAPHORE *_exitEvent;
    PIN_SEMAPHORE *_bufferSem;
    PIN_LOCK _bufferListLock;
    list< BUFFER_LIST_ELEMENT > _bufferList;
};

/*
 * APP_THREAD_REPRESENTITVE
 * Each application thread, creates an object of this class and saves it in it's Pin TLS
 * slot (appThreadRepresentitiveKey).
 * This object is used when the BufferFull function is called. It provides the functionality
 * of:
 * 1) Managing the buffers allocated (by Pin) by this thread. It uses it's BUFFER_LIST_MANAGER
 *    _freeBufferListManager to do this.
 * 2) Enqueuing a full buffer on the global full buffers list (fullBuffersListManager) so it
 *    will be processed by one of the internal-tool buffer processing threads.
 * 3) If there is no internal-tool buffer processing thread running yet
 *    then ProcessBuffer is used to process the buffer by the application
 *    thread. It cannot wait for processing thread to start running
 *    because this may cause deadlock - because this app thread may be holding some OS
 *    resource that the processing thread needs in order to start running - e.g. the LoaderLock
 */
class APP_THREAD_REPRESENTITVE
{
  public:
    APP_THREAD_REPRESENTITVE(THREADID tid);
    ~APP_THREAD_REPRESENTITVE();

    // Called from the BufferFull callback
    VOID* EnqueueFullAndGetNextToFill(VOID* buf, UINT64 numElements, UINT32 buf_type, BUFFER_ID bufId);

    // Allocated app thread buffers
    VOID AllocateBuffers(UINT32 buf_type, BUFFER_ID bufId);

    // Called from the ThreadFini callback, to know when all the buffers of this app thread
    // have been processed
    BOOL AllBuffersProcessed();

    APP_THREAD_STATISTICS* Statistics() { return (&_appThreadStatistics); }
    BUFFER_LIST_MANAGER& FreeBufferListManager(UINT32 buf_type) { return *_freeBufferListManagers[buf_type]; }
    THREADID _myTid;
    VOID* _currentBufs[NUM_BUF_TYPES];
    UINT64 _numBufferElements;

    // For shared memory mode
    VOID* _shared_mem;
    UINT64 _shared_mem_size;
    NATIVE_FD _shm_fd;
    int _sock_rdy;
    int _sock_size;
    UINT64 last_ins_executed = 0;
    UINT64 last_mem_ins_executed = 0;
    UINT64 last_bblocks_executed = 0;
    UINT64 fcalls_executed = 0;
    UINT64 last_fcalls_executed = 0;
    UINT64 _buf_gen_time[NUM_BUF_TYPES];

  private:
    // the buffers of this thread are placed on this list when they are available for filling
    // BUFFER_LIST_MANAGER* _freeBufferListManager;
    BUFFER_LIST_MANAGER* _freeBufferListManagers[NUM_BUF_TYPES];
    UINT32 _numBuffersAllocated_TYPE[NUM_BUF_TYPES];
    UINT32 _numProcessed;

    APP_THREAD_STATISTICS _appThreadStatistics;
};

#endif