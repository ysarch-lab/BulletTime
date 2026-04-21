// Buffer List Manager Implementation
#include "common.h"
#include "buffers.h"

BUFFER_LIST_MANAGER::BUFFER_LIST_MANAGER(BOOL notifyExitRequired) : _exitEvent(NULL)
{
    PIN_InitLock(&_bufferListLock);
    _bufferSem = new PIN_SEMAPHORE;
    if (!PIN_SemaphoreInit(_bufferSem))
    {
        fprintf(stderr, "BufferSem: PIN_SemaphoreInit failed");
        exit(-1);
    }
    if (notifyExitRequired)
    {
        _exitEvent = new PIN_SEMAPHORE;
        if (!PIN_SemaphoreInit(_exitEvent))
        {
            fprintf(stderr, "ExitEvent: PIN_SemaphoreInit failed");
            exit(-1);
        }
    }
}

BUFFER_LIST_MANAGER::~BUFFER_LIST_MANAGER()
{
    if (_exitEvent != NULL)
    {
        PIN_SemaphoreFini(_exitEvent);
        delete _exitEvent;
    }
    PIN_SemaphoreFini(_bufferSem);
    delete _bufferSem;
}

BOOL BUFFER_LIST_MANAGER::PutBufferOnList(VOID* buf, UINT64 numElements,
                                          /* the thread that owns the buffer */
                                          APP_THREAD_REPRESENTITVE* appThreadRepresentitive,
                                          /* thread Id of the thread making the call */
                                          THREADID tid,
                                          /* buffer index (for tracing) */
                                          UINT32 index, UINT32 bufferType, UINT32 buf_field, UINT64* sync_time)
{
    if ((_exitEvent != NULL) && PIN_SemaphoreIsSet(_exitEvent))
    {
        // Exit event signaled. Do not add new buffers.
        return FALSE;
    }

    BUFFER_LIST_ELEMENT bufferListElement;
    bufferListElement.buf                     = buf;
    bufferListElement.numElements             = numElements;
    bufferListElement.appThreadRepresentitive = appThreadRepresentitive;
    bufferListElement.index                   = index;
    bufferListElement.buf_field               = buf_field;

    UINT64 start_time_us, end_time_us;
    OS_Time(&start_time_us);
    {
        SCOPED_LOCK lock(&_bufferListLock, tid);
        _bufferList.push_back(bufferListElement);
    }
    OS_Time(&end_time_us);
    if (sync_time != NULL) *sync_time = end_time_us - start_time_us;

    PIN_SemaphoreSet(_bufferSem);

    return TRUE;
}

VOID* BUFFER_LIST_MANAGER::GetBufferFromList(UINT64* numElements,
                                             /* the thread that owns the buffer */
                                             APP_THREAD_REPRESENTITVE** appThreadRepresentitive,
                                             /* buffer index */
                                             UINT32* index,
                                             /* thread Id of the thread making the call */
                                             THREADID tid,
                                             /* type of data in the buffer */
                                             UINT32* buf_field,
                                             /* total sync time*/
                                             UINT64* sync_time)
{
    // Loop until buffer is available or exit event is signaled + no more buffers to process
    UINT64 start_time, end_time, total_sync_time = 0;
    VOID* ret = NULL;
    bool exit = false;
    while (!exit){
        // if (_exitEvent != NULL && PIN_SemaphoreIsSet(_exitEvent) && !PIN_SemaphoreIsSet(_bufferSem)) {
        if (_exitEvent != NULL && PIN_SemaphoreIsSet(_exitEvent) && _bufferList.empty()) {
            // Process exit flow started and there is no pending buffers to process.
            return NULL;
        }

        // No exit event, wait for a buffer
        // - This can wait indefinitely if no buffer is available and the process exits
        // - Thus, PrepareForFini must set the exit event and the buffer semaphore
        PIN_SemaphoreWait(_bufferSem);

        // At this point, there is at least one buffer available, or the exit event is signaled
        // - Process a buffer is there are some available
        // - Keep the buffer semaphore set if any of the following conditions are met:
        //      - There are still buffers to be processed
        //      - The exit event is signaled
        // - Otherwise, the buffer semaphore is cleared
        OS_Time(&start_time);
        {
            SCOPED_LOCK lock(&_bufferListLock, tid);

            // Lock acquired, we must check if there is still a buffer available
            if (!_bufferList.empty()){
                // Buffer is available
                ASSERTX(!_bufferList.empty());
                const BUFFER_LIST_ELEMENT& bufferListElement = (_bufferList.front());
                VOID* buf                                    = bufferListElement.buf;
                *numElements                                 = bufferListElement.numElements;
                *appThreadRepresentitive                     = bufferListElement.appThreadRepresentitive;
                *index                                       = bufferListElement.index;
                *buf_field                                   = bufferListElement.buf_field;
                _bufferList.pop_front();

                // Clear buffer semaphore if no more buffers to process
                if (_bufferList.empty()){
                    PIN_SemaphoreClear(_bufferSem);
                }
                ret = buf;
                exit = true;
            }
            // Otherwise, other thread(s) took the buffer(s), so we must re-poll at the top of the loop
            // Alternatively, if the exit event is signaled, we must exit the loop
            // - The stage 2 manager always returns NULL, so we signal threads to exit with str_buf = NULL
            else if (_exitEvent != NULL && PIN_SemaphoreIsSet(_exitEvent)){
                // Exit event is signaled
                exit = true;
            }
        }
        OS_Time(&end_time);
        total_sync_time += end_time - start_time;
    }
    if (sync_time != NULL) *sync_time = total_sync_time;
    return ret;
}

VOID BUFFER_LIST_MANAGER::NotifyExit()
{
    // Case 1: This is the global full buffers list manager
    // - Set the exit event
    // - Wait until the buffer list is empty
    // - Finally set the buffer semaphore to ensure that all remaining threads wake up and exit
    if (_exitEvent != NULL)
    {
        PIN_SemaphoreSet(_exitEvent);
        while (!_bufferList.empty()){
            PIN_SemaphoreSet(_bufferSem);
        }
        PIN_SemaphoreSet(_bufferSem);
    }
    // Case 2: This is an application thread's free buffer list manager, do nothing
}