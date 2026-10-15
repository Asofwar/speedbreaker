// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "dispatcher.h"

#include <condition_variable>
#include <queue>
#include <utility>

#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>

namespace dispatcher
{
    static std::mutex s_lock;
    static std::condition_variable s_cv;

    std::mutex& Lock() { return s_lock; }
    void NotifyAllLocked() { s_cv.notify_all(); }

    int64_t GuestSystemTime()
    {
        return guesttime::SystemTime100ns();
    }

    std::optional<int64_t> Deadline(const be<int64_t>* timeout)
    {
        if (timeout == nullptr)
            return std::nullopt;
        int64_t t = *timeout;
        // INT64_MIN is the game's Sleep(INFINITE) (its SleepEx, from a file
        // read wrapper): -t * 100 used to wrap to 0, a wait that returns at
        // once, and the game runs that way; keep it so.
        int64_t relative100ns = t <= 0 ? (t == INT64_MIN ? 0 : -t) : std::max<int64_t>(0, t - GuestSystemTime());
        // 73 years is forever, and keeps guest ns (and the host deadline a
        // wait derives from it) from overflowing.
        relative100ns = std::min<int64_t>(relative100ns, INT64_MAX / 400);
        return guesttime::NowNs() + relative100ns * 100;
    }

    std::optional<int64_t> DeadlineFromMilliseconds(uint32_t ms)
    {
        if (ms == INFINITE)
            return std::nullopt;
        return guesttime::NowNs() + int64_t(ms) * 1'000'000;
    }

    std::condition_variable& ConditionVariable() { return s_cv; }
}

using namespace dispatcher;

namespace
{
    // One thread's wait, in s_waits (under Lock()) from its start to its end.
    struct WaitBlock
    {
        std::span<Waitable* const> objects;
        bool waitAll;
        // Bit i: objects[i] was signaled for this wait while it slept
        // (ReleaseWaitersLocked); counts at the wait's next check only. The
        // 360 waits on 64 objects at most.
        uint64_t granted = 0;
    };

    // The waits in progress, oldest first.
    std::vector<WaitBlock*> s_waits;

    bool Granted(uint64_t granted, size_t i)
    {
        return i < 64 && ((granted >> i) & 1) != 0;
    }

    // The kernel satisfies the waits on an object the moment it's set or
    // pulsed. Waiters here check only once they run again, which can be
    // after the event was reset (a pulse resets it at once): a set and a
    // reset, or a pulse, would release no one, or (as pulses did before)
    // leave the event signaled for whichever thread came to wait next. So
    // the waits in progress on `object` that the kernel would satisfy now,
    // every one (`all`: a notification object) or the oldest (a
    // synchronization event's pulse), are marked here and take `object` as
    // signaled at their next check. Left out, as the kernel would have
    // satisfied them already or can't yet: a wait released before, a
    // wait-any with another object signaled (it takes that one, so a pulse
    // goes to a thread that has nothing else), a wait-all whose other
    // objects are not all signaled. A mutant's ownership is judged here for
    // the signaling thread, not the waiter: a wait-all that the mutant then
    // refuses keeps waiting, and the pulse is lost. True if any was marked.
    bool ReleaseWaitersLocked(Waitable* object, bool all)
    {
        bool released = false;
        for (WaitBlock* w : s_waits)
        {
            uint64_t mine = 0;
            for (size_t i = 0; i < std::min<size_t>(w->objects.size(), 64); i++)
                if (w->objects[i] == object)
                    mine |= uint64_t(1) << i;
            if (mine == 0 || (!w->waitAll && w->granted != 0))
                continue;
            bool satisfied = true;
            for (size_t i = 0; i < w->objects.size() && satisfied; i++)
            {
                if (Granted(mine | w->granted, i))
                    continue;
                bool signaled = w->objects[i]->IsSignaledLocked();
                satisfied = w->waitAll ? signaled : !signaled;
            }
            if (!satisfied)
                continue;
            // Wait-any: the first index of `object` is the wait's result.
            w->granted |= w->waitAll ? mine : mine & (~mine + 1);
            released = true;
            if (!all)
                break;
        }
        return released;
    }
}

// The wait itself, with Lock() held (`lock`, released while it sleeps).
static uint32_t WaitLocked(std::unique_lock<std::mutex>& lock, WaitBlock& block, std::optional<int64_t> deadline)
{
    std::span<Waitable* const> objects = block.objects;
    while (true)
    {
        // Only at this check: a wait-all whose other objects were taken
        // since is not satisfied after all, and keeps waiting.
        uint64_t granted = std::exchange(block.granted, 0);
        if (block.waitAll)
        {
            bool all = true;
            for (size_t i = 0; i < objects.size(); i++)
                all &= Granted(granted, i) || objects[i]->IsSignaledLocked();
            if (all)
            {
                // A granted object was taken by the set or pulse that
                // granted it (a pulsed synchronization event is reset
                // already, and acquiring it again would take a later set).
                for (size_t i = 0; i < objects.size(); i++)
                    if (!Granted(granted, i))
                        objects[i]->AcquireLocked();
                return STATUS_WAIT_0;
            }
        }
        else if (granted != 0)
        {
            return STATUS_WAIT_0 + uint32_t(std::countr_zero(granted));
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (objects[i]->IsSignaledLocked())
                {
                    objects[i]->AcquireLocked();
                    return STATUS_WAIT_0 + uint32_t(i);
                }
            }
        }

        if (!deadline)
        {
            ConditionVariable().wait(lock);
            continue;
        }
        // Guest time stands still while the game is suspended, so no timeout
        // can run out: wait without one until a change or the resume. That
        // includes a poll (timeout 0), which would otherwise return at once
        // to a guest loop that polls again, spinning a core throughout.
        // WakeAfterResume notifies under Lock() once Suspended() is false,
        // so this thread is either waiting by then or reads false here.
        if (guesttime::Suspended())
        {
            ConditionVariable().wait(lock);
            continue;
        }
        int64_t left = *deadline - guesttime::NowNs();
        if (left <= 0)
            return STATUS_TIMEOUT;
        // A suspension that starts meanwhile is seen when this wakes at the
        // host deadline, with guest time still short of it.
        ConditionVariable().wait_until(lock, Clock::now() + std::chrono::nanoseconds(left));
    }
}

uint32_t WaitForObjects(std::span<Waitable* const> objects, bool waitAll, std::optional<int64_t> deadline)
{
    // Every object stays allocated for the whole wait, however long: on the
    // 360 a wait references its objects, so another thread may close a handle
    // while this one waits on it (typically right after signaling it), and
    // the waiter still reads the object when it wakes. The last reference can
    // be the wait's own, released (outside Lock(), see ReleaseKernelObject)
    // once the wait is over.
    for (Waitable* o : objects)
        RetainKernelObject(o);
    uint32_t status;
    {
        std::unique_lock lock(Lock());
        WaitBlock block{ objects, waitAll };
        s_waits.push_back(&block);
        struct Leave
        {
            WaitBlock* block;
            ~Leave() { std::erase(s_waits, block); }
        } leave{ &block };
        status = WaitLocked(lock, block, deadline);
    }
    // What the wait took may be mirrored in guest memory: stored now that
    // the lock is released (see GuestSignalState).
    // Then, before the guest runs on, the stores of the threads that
    // signaled it, which may still be in flight (GuestSignalState): the guest
    // may free or reuse the header as soon as this returns.
    if (status != STATUS_TIMEOUT)
    {
        if (waitAll)
        {
            for (Waitable* o : objects)
                o->PublishGuestState();
            for (Waitable* o : objects)
                o->AwaitGuestPublishes();
        }
        else
        {
            objects[status - STATUS_WAIT_0]->PublishGuestState();
            objects[status - STATUS_WAIT_0]->AwaitGuestPublishes();
        }
    }
    for (Waitable* o : objects)
        ReleaseKernelObject(o);
    return status;
}

uint32_t Waitable::Wait(uint32_t timeoutMs)
{
    Waitable* self = this;
    return WaitForObjects({ &self, 1 }, false, DeadlineFromMilliseconds(timeoutMs));
}

// ---- Guest headers -----------------------------------------------------------

void GuestSignalState::Publish()
{
    if (field == nullptr)
        return;
    uint32_t v = value.load();
    while (true)
    {
        *field = v;
        // The store before the check: a thread whose older value landed
        // after a newer one reads the newer one here and stores it again.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint32_t now = value.load();
        if (now == v)
            return;
        v = now;
    }
}

void GuestSignalState::PublishAndEnd()
{
    Publish();
    if (inFlight.fetch_sub(1, std::memory_order_release) == 1)
        inFlight.notify_all();
}

void GuestSignalState::AwaitPublishes()
{
    // Usually 0 at once; otherwise a setter between its unlock and its store,
    // or in a write-watch fault waiting for the GPU (which needs no lock this
    // thread holds).
    for (uint32_t n = inFlight.load(std::memory_order_acquire); n != 0; n = inFlight.load(std::memory_order_acquire))
        inFlight.wait(n, std::memory_order_acquire);
}

// ---- Event -----------------------------------------------------------------

EventObject::EventObject(XKEVENT* header)
    : manualReset(header->Type == 0), signaled(header->SignalState != 0),
      guest(&header->SignalState, header->SignalState != 0 ? 1 : 0)
{
}

EventObject::EventObject(bool manualReset, bool initialState)
    : manualReset(manualReset), signaled(initialState)
{
}

bool EventObject::MirrorLocked()
{
    guest.SetLocked(signaled ? 1 : 0);
    return guest.field != nullptr;
}

void EventObject::AcquireLocked()
{
    if (!manualReset)
        signaled = false;
    MirrorLocked();
}

// Set, Reset and Pulse publish the guest header after the lock only when
// MirrorLocked found one, decided under the lock: an event created by
// handle can be freed by a racing NtClose as soon as the lock is released
// (ReleaseKernelObject), while one bound to a guest header is never freed.
bool EventObject::Set()
{
    bool previous;
    bool publish;
    {
        std::lock_guard lock(Lock());
        previous = signaled;
        // A notification event releases every thread waiting on it now,
        // even one that runs only after a reset.
        if (manualReset && !signaled)
            ReleaseWaitersLocked(this, true);
        signaled = true;
        publish = MirrorLocked();
        if (publish)
            guest.BeginPublishLocked();
        NotifyAllLocked();
    }
    if (publish)
        guest.PublishAndEnd();
    return previous;
}

bool EventObject::Reset()
{
    bool previous;
    bool publish;
    {
        std::lock_guard lock(Lock());
        previous = signaled;
        signaled = false;
        publish = MirrorLocked();
        if (publish)
            guest.BeginPublishLocked();
    }
    if (publish)
        guest.PublishAndEnd();
    return previous;
}

// Releases the threads waiting right now (all of them for a notification
// event, one for a synchronization event), then leaves the event reset. As
// KePulseEvent, an event signaled already is only reset: the threads still
// waiting on it wait for other objects too. Only the waits in progress are
// released (ReleaseWaitersLocked), and the event is never left signaled.
bool EventObject::Pulse()
{
    bool previous;
    bool publish;
    {
        std::lock_guard lock(Lock());
        previous = signaled;
        if (!signaled && ReleaseWaitersLocked(this, manualReset))
            NotifyAllLocked();
        signaled = false;
        publish = MirrorLocked();
        if (publish)
            guest.BeginPublishLocked();
    }
    if (publish)
        guest.PublishAndEnd();
    return previous;
}

// ---- Semaphore -------------------------------------------------------------

SemaphoreObject::SemaphoreObject(XKSEMAPHORE* semaphore)
    : count(int32_t(uint32_t(semaphore->Header.SignalState))), limit(int32_t(uint32_t(semaphore->Limit))),
      guest(&semaphore->Header.SignalState, uint32_t(semaphore->Header.SignalState))
{
}

SemaphoreObject::SemaphoreObject(int32_t count, int32_t limit)
    : count(count), limit(limit)
{
}

void SemaphoreObject::AcquireLocked()
{
    count--;
    guest.SetLocked(uint32_t(count));
}

uint32_t SemaphoreObject::Release(int32_t releaseCount, int32_t* previousCount)
{
    bool publish;
    {
        std::lock_guard lock(Lock());
        if (previousCount)
            *previousCount = count;
        if (releaseCount <= 0 || int64_t(count) + releaseCount > limit)
            return 0xC0000047; // STATUS_SEMAPHORE_LIMIT_EXCEEDED
        count += releaseCount;
        guest.SetLocked(uint32_t(count));
        // As EventObject::Set.
        publish = guest.field != nullptr;
        if (publish)
            guest.BeginPublishLocked();
        NotifyAllLocked();
    }
    if (publish)
        guest.PublishAndEnd();
    return STATUS_SUCCESS;
}

// ---- Mutant ----------------------------------------------------------------

MutantObject::MutantObject(bool initialOwner)
{
    if (initialOwner)
    {
        owner = GuestThread::GetCurrentThreadId();
        recursion = 1;
    }
}

bool MutantObject::IsSignaledLocked() const
{
    return recursion == 0 || owner == GuestThread::GetCurrentThreadId();
}

void MutantObject::AcquireLocked()
{
    owner = GuestThread::GetCurrentThreadId();
    recursion++;
}

uint32_t MutantObject::Release(int32_t* previous)
{
    std::lock_guard lock(Lock());
    if (previous)
        *previous = int32_t(recursion);
    if (recursion == 0 || owner != GuestThread::GetCurrentThreadId())
        return 0xC0000046; // STATUS_MUTANT_NOT_OWNED
    if (--recursion == 0)
    {
        owner = 0;
        NotifyAllLocked();
    }
    return STATUS_SUCCESS;
}

// ---- Timer -----------------------------------------------------------------

namespace
{
    std::atomic<uint64_t> s_firings{ 0 };

    struct TimerEntry
    {
        int64_t due;  // guest ns
        TimerObject* timer;
        uint64_t generation;
        bool operator>(const TimerEntry& o) const { return due > o.due; }
    };

    // One host thread fires every timer. Queued entries hold a reference on
    // their timer so a closed handle can't leave a dangling pointer.
    struct TimerThread
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>> queue;
        std::thread thread{ [this] { Run(); } };

        void Schedule(TimerObject* timer, int64_t due, uint64_t generation)
        {
            RetainKernelObject(timer);
            std::lock_guard lock(mutex);
            queue.push({ due, timer, generation });
            cv.notify_one();
        }

        void Run()
        {
            std::unique_lock lock(mutex);
            while (true)
            {
                // No timer comes due while the game is suspended (guest time
                // stands still). Suspended() is read under `mutex`, which
                // WakeAfterResume takes to notify once it's false.
                if (queue.empty() || guesttime::Suspended())
                {
                    cv.wait(lock);
                    continue;
                }
                TimerEntry next = queue.top();
                int64_t left = next.due - guesttime::NowNs();
                if (left > 0)
                {
                    cv.wait_until(lock, Clock::now() + std::chrono::nanoseconds(left));
                    continue;
                }
                queue.pop();
                lock.unlock();
                Fire(next);
                ReleaseKernelObject(next.timer);
                lock.lock();
            }
        }

        void Fire(const TimerEntry& e)
        {
            std::lock_guard lock(Lock());
            TimerObject* t = e.timer;
            if (t->generation != e.generation)
                return;  // cancelled or re-armed since
            // A notification timer, as a notification event: its waits are
            // released even if it's re-armed (reset) before they run.
            if (t->manualReset && !t->signaled)
                ReleaseWaitersLocked(t, true);
            t->signaled = true;
            s_firings.fetch_add(1, std::memory_order_relaxed);
            NotifyAllLocked();
            // Re-armed only while something besides this entry holds the
            // timer (a handle, a wait): once its last handle is closed no one
            // can wait on it or set it again, and a periodic timer closed
            // without a cancel would fire, and stay allocated, for good (the
            // kernel cancels a timer when it deletes it).
            if (t->periodMs != 0 && t->refs.load() > 1)
            {
                // From the last due time in guest time: a suspension adds no
                // periods to catch up on.
                t->due = e.due + int64_t(t->periodMs) * 1'000'000;
                RetainKernelObject(t);
                std::lock_guard qlock(mutex);
                queue.push({ t->due, t, t->generation });
            }
        }
    };

    // Created by the first timer armed. WakeAfterResume reads it under
    // s_timersMutex too, so it either finds the thread or the thread starts
    // after the resume and reads Suspended() false itself.
    std::mutex s_timersMutex;
    TimerThread* s_timers = nullptr;

    TimerThread& Timers()
    {
        std::lock_guard lock(s_timersMutex);
        if (s_timers == nullptr)
        {
            s_timers = new TimerThread();
            s_timers->thread.detach();
        }
        return *s_timers;
    }
}

void dispatcher::WakeAfterResume()
{
    // Two locks one after the other, never nested (Fire takes Lock() and
    // then the timer mutex). Both are released by their waiters' cv waits.
    {
        std::lock_guard lock(Lock());
        NotifyAllLocked();
    }
    TimerThread* timers;
    {
        std::lock_guard lock(s_timersMutex);
        timers = s_timers;  // never created here
    }
    if (timers != nullptr)
    {
        std::lock_guard lock(timers->mutex);
        timers->cv.notify_all();
    }
}

uint64_t dispatcher::TimerFirings()
{
    return s_firings.load(std::memory_order_relaxed);
}

TimerObject::TimerObject(uint32_t timerType)
    : manualReset(timerType == 0)
{
}

bool TimerObject::Set(std::optional<int64_t> dueTime, uint32_t period)
{
    uint64_t gen;
    int64_t when;
    bool previous;
    {
        std::lock_guard lock(Lock());
        previous = signaled;
        signaled = false;
        periodMs = period;
        due = dueTime.value_or(guesttime::NowNs());
        gen = ++generation;
        when = due;
    }
    Timers().Schedule(this, when, gen);
    return previous;
}

bool TimerObject::Cancel()
{
    std::lock_guard lock(Lock());
    bool previous = signaled;
    generation++;
    return previous;
}

// ---- Binding -----------------------------------------------------------------

Waitable* BindDispatcherHeader(XDISPATCHER_HEADER& header)
{
    // Bound already (every use but the first): read without g_kernelLock.
    // The read can fault, on a page the GPU is still to write, into a wait
    // for the GPU (write_watch.cpp's HandleFault), and the command processor
    // runs the guest's interrupt handler, whose KeSetEvent comes here: with
    // the lock held across the fault the two would wait for each other (as
    // GuestSignalState explains for the dispatcher's lock). A bind stores
    // Blink before Flink (release), so a Flink read as the signature
    // (acquire) comes with its Blink. Only the first use takes the lock.
    uint32_t* flink = &header.WaitListHead.Flink.value;
    if (ByteSwap(__atomic_load_n(flink, __ATOMIC_ACQUIRE)) == OBJECT_SIGNATURE)
        return static_cast<Waitable*>(g_memory.Translate(header.WaitListHead.Blink.get()));

    std::lock_guard guard{ g_kernelLock };
    if (header.WaitListHead.Flink == OBJECT_SIGNATURE)
        return static_cast<Waitable*>(g_memory.Translate(header.WaitListHead.Blink.get()));

    Waitable* obj;
    switch (header.Type)
    {
    case 0: // NotificationEvent
    case 1: // SynchronizationEvent
        obj = CreateKernelObject<EventObject>(reinterpret_cast<XKEVENT*>(&header));
        break;
    case 5: // Semaphore
        obj = CreateKernelObject<SemaphoreObject>(reinterpret_cast<XKSEMAPHORE*>(&header));
        break;
    default:
        fprintf(stderr, "[dispatcher] guest dispatcher header type %u is not supported yet\n", header.Type);
        assert(false && "unsupported guest dispatcher object type");
        return nullptr;
    }
    header.WaitListHead.Blink = g_memory.MapVirtual(obj);
    __atomic_store_n(flink, ByteSwap(OBJECT_SIGNATURE), __ATOMIC_RELEASE);
    return obj;
}

Waitable* WaitableFromHandle(uint32_t handle)
{
    KernelObject* obj;
    if (handle == 0xFFFFFFFE)
        obj = GuestThread::GetCurrentThreadObject();
    else if (handle != GUEST_INVALID_HANDLE_VALUE && IsKernelObject(handle))
        obj = GetKernelObject(handle);
    else
        return nullptr;
    return dynamic_cast<Waitable*>(obj);
}
