// App thread representative class implementation
#include "common.h"
#include "buffers.h"

// Number of free buffers to pre-allocate per application thread (Pin's internal buffering).
// A small value (2-3) is enough since the consumer drains buffers quickly.
#define APP_BUFFERS 2

APP_THREAD_REPRESENTITVE::APP_THREAD_REPRESENTITVE(THREADID tid)
    : _myTid(tid)
{
    _numBufferElements = 0;
    for (int i = 0; i < NUM_BUF_TYPES; i++){
        _freeBufferListManagers[i] = new BUFFER_LIST_MANAGER();
        _currentBufs[i] = NULL;
        _numBuffersAllocated_TYPE[i] = 0;
    }
    _shared_mem = NULL;
    _shared_mem_size = 0;
    _sock_rdy = -1;
    _sock_size = -1;
}

APP_THREAD_REPRESENTITVE::~APP_THREAD_REPRESENTITVE() {
    for (int i = 0; i < NUM_BUF_TYPES; i++){
        delete _freeBufferListManagers[i];
    }
}

VOID* APP_THREAD_REPRESENTITVE::EnqueueFullAndGetNextToFill(VOID* fullBuf, UINT64 numElements, UINT32 buf_type, BUFFER_ID bufId)
{
    _appThreadStatistics.AddNumElementsProcessed(numElements);
    _appThreadStatistics.IncrementNumBuffersFilled();
    UINT32 n_filled = _appThreadStatistics.NumBuffersFilled();

    // Process the buffer in-thread (hands it off to the consumer via shared memory).
    _appThreadStatistics.IncrementNumBuffersProcessedInAppThread();
    ProcessBuffer(fullBuf, numElements, this, n_filled, FALSE, NULL, buf_type);

    // Get the next free buffer for Pin to fill.
    UINT64 numElementsDummy;
    APP_THREAD_REPRESENTITVE* appThreadRepresentitiveDummy;
    UINT32 indexDummy;
    UINT32 buf_field;
    _currentBufs[buf_type] = _freeBufferListManagers[buf_type]->GetBufferFromList(&numElementsDummy,
                            &appThreadRepresentitiveDummy, &indexDummy, _myTid, &buf_field, NULL);
    ASSERTX(_currentBufs[buf_type] != NULL);
    ASSERTX(buf_field == buf_type);
    ASSERTX(appThreadRepresentitiveDummy == this);
    return _currentBufs[buf_type];
}

VOID APP_THREAD_REPRESENTITVE::AllocateBuffers(UINT32 buf_type, BUFFER_ID bufId)
{
    if (_numBuffersAllocated_TYPE[buf_type] != 0) return;

    size_t buf_size = bufSizes[buf_type] * buf_entry_limit;

    for (UINT32 i = 0; i < APP_BUFFERS; i++) {
        VOID* buf = PIN_AllocateBuffer(bufId);
        OS_ProtectMemory(native_pid, buf, buf_size,
            OS_PAGE_PROTECTION_TYPE_READ | OS_PAGE_PROTECTION_TYPE_WRITE | OS_PAGE_PROTECTION_TYPE_EXECUTE);
        _freeBufferListManagers[buf_type]->PutBufferOnList(buf, 0, this, _myTid, 0, 0, buf_type, NULL);
        _numBuffersAllocated_TYPE[buf_type]++;
    }
}

BOOL APP_THREAD_REPRESENTITVE::AllBuffersProcessed()
{
    BOOL allProcessed = TRUE;
    for (UINT32 i = 0; i < NUM_BUF_TYPES; i++){
        allProcessed &= (_numBuffersAllocated_TYPE[i] == 0);
    }
    if (allProcessed)
        return TRUE;

    for (UINT32 i = 0; i < NUM_BUF_TYPES; i++){
        ASSERTX(_currentBufs[i] != NULL);
    }

    // Reclaim allocated buffers
    for (UINT32 i = 0; i < NUM_BUF_TYPES; i++){
        for (; _numBuffersAllocated_TYPE[i] > 0; _numBuffersAllocated_TYPE[i]--)
        {
            UINT64 numElementsDummy;
            APP_THREAD_REPRESENTITVE* appThreadRepresentitiveDummy = NULL;
            UINT32 indexDummy;
            UINT32 buf_field;
            VOID* buf = _freeBufferListManagers[i]->GetBufferFromList(&numElementsDummy,
                                &appThreadRepresentitiveDummy, &indexDummy, _myTid, &buf_field, NULL);
            ASSERTX(buf != NULL);
            ASSERTX(buf_field == i);
            ASSERTX(appThreadRepresentitiveDummy == this);
            PIN_DeallocateBuffer(buf_ids[i], buf);
        }
    }
    for (UINT32 i = 0; i < NUM_BUF_TYPES; i++){
        PIN_DeallocateBuffer(buf_ids[i], _currentBufs[i]);
    }
    return TRUE;
}
