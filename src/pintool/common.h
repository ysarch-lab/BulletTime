#include "pin.H"

#ifndef COMMON_H
#define COMMON_H

// Struct of memory references recorded in buffers.
struct MEMREF
{
    ADDRINT pc;
    ADDRINT ea;
    UINT32 sz;
    UINT32 is_read;
};

// BulletTime uses a single buffer type holding full MEMREF structs.
enum BUF_TYPE {
    MEMREF_BUF = 0,
    NUM_BUF_TYPES,
};

// Scoped lock helper
class SCOPED_LOCK
{
    PIN_LOCK* _lock;

    public:
    SCOPED_LOCK(PIN_LOCK* lock, THREADID tid) : _lock(lock)
    {
        ASSERTX(_lock != NULL);
        PIN_GetLock(_lock, tid + 1);
    }
    ~SCOPED_LOCK() { PIN_ReleaseLock(_lock); }
};

#endif
