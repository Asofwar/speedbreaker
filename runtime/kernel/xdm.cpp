// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker.
#include <stdafx.h>
#include "xdm.h"
#include "dispatcher.h"
#include "freelist.h"

Mutex g_kernelLock;

void DestroyKernelObject(KernelObject* obj)
{
    obj->~KernelObject();
    g_userHeap.Free(obj);
}

void ReleaseKernelObject(KernelObject* obj)
{
    if (--obj->refs != 0)
        return;
    // A waitable object is read and changed under the dispatcher's lock.
    // The last reference is gone, so no wait can be on it (each holds one,
    // WaitForObjects), but a thread that looked it up by handle just before
    // another closed that handle can be inside such a section still (a set
    // racing the close, which the 360's kernel allows: it references the
    // object for the call). Taking the lock once makes the free wait for
    // that section to end. Never called with the lock held.
    if (dynamic_cast<Waitable*>(obj) != nullptr)
    {
        std::lock_guard lock(dispatcher::Lock());
    }
    DestroyKernelObject(obj);
}

uint32_t GetKernelHandle(KernelObject* obj)
{
    assert(obj != GetInvalidKernelObject());
    return g_memory.MapVirtual(obj);
}

void DestroyKernelObject(uint32_t handle)
{
    DestroyKernelObject(GetKernelObject(handle));
}

bool IsKernelObject(uint32_t handle)
{
    return (handle & 0x80000000) != 0;
}

bool IsKernelObject(void* obj)
{
    return IsKernelObject(g_memory.MapVirtual(obj));
}

bool IsInvalidKernelObject(void* obj)
{
    return obj == GetInvalidKernelObject();
}
