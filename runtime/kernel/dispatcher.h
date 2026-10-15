// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Kernel dispatcher: every waitable object (event, semaphore, mutant, timer,
// thread) exposes "is it signaled?" and "consume the signal", and one wait
// routine serves single and multiple waits, wait-any and wait-all, with
// relative, absolute or infinite guest timeouts. Semantics follow Xenia's
// XObject/XEvent/XSemaphore/XMutant/XTimer.
//
// All state changes and checks happen under one global lock; waiters sleep on
// one condition variable and re-check after every state change. Simple and
// correct first; revisit only if profiling shows contention. A re-check can
// come after the change was undone (an event set and reset, or pulsed), so a
// set or pulse also marks the waits it satisfies (dispatcher.cpp's
// ReleaseWaitersLocked), as the kernel satisfies them at once. Guest memory
// is never touched under the lock (see GuestSignalState), and a wait holds a
// reference on each of its objects (WaitForObjects).
//
// Timeouts and timers run on guest time (guesttime::NowNs()), which stands
// still while the game is suspended: a timed wait or a timer that would come
// due then parks without a timeout instead, and WakeAfterResume wakes it.
#pragma once
#include <chrono>
#include <optional>
#include <span>

#include "xdm.h"

namespace dispatcher
{
    using Clock = std::chrono::steady_clock;

    std::mutex& Lock();
    // Wake every waiter to re-check. Call with Lock() held, after a change.
    void NotifyAllLocked();

    // Guest time in 100 ns units since 1601 (KeQuerySystemTime's clock).
    int64_t GuestSystemTime();
    // Deadlines are guest ns (guesttime::NowNs()). nullptr = infinite;
    // negative = relative 100 ns; positive = absolute guest system time;
    // 0 = poll.
    std::optional<int64_t> Deadline(const be<int64_t>* timeout);
    std::optional<int64_t> DeadlineFromMilliseconds(uint32_t ms);

    // guesttime's resume hook, on its own thread: wakes the waits and the
    // timer thread that parked without a timeout while the game was
    // suspended, so they measure guest time again.
    void WakeAfterResume();
    // Kernel timer firings so far (the suspension log counts them).
    uint64_t TimerFirings();
}

struct Waitable : KernelObject
{
    // Both are called with dispatcher::Lock() held, on the waiting thread.
    virtual bool IsSignaledLocked() const = 0;
    virtual void AcquireLocked() {}
    // Called without Lock(), after AcquireLocked: stores the state it changed
    // into the object's guest-embedded header, if it has one.
    virtual void PublishGuestState() {}
    // Called without Lock() by a wait that took the object, before it
    // returns to the guest: waits for the stores into the guest header that
    // other threads began before (GuestSignalState::AwaitPublishes).
    virtual void AwaitGuestPublishes() {}

    // KernelObject interface: millisecond timeout (INFINITE = forever).
    uint32_t Wait(uint32_t timeoutMs) override;
};

// Returns STATUS_WAIT_0 + index (wait-any), STATUS_WAIT_0 (wait-all) or
// STATUS_TIMEOUT. `deadline` is guest ns (dispatcher::Deadline).
uint32_t WaitForObjects(std::span<Waitable* const> objects, bool waitAll,
    std::optional<int64_t> deadline);

// The SignalState of an object's guest-embedded dispatcher header (a KEVENT
// or KSEMAPHORE in game memory), kept equal to the host object's state for
// guest code that reads the header itself. It is never stored under
// dispatcher::Lock(): on a page the GPU still uses the store faults into a
// wait for the GPU (write_watch.cpp's HandleFault), and the command
// processor runs the guest's interrupt handler, whose KeSetEvent needs the
// lock, so the two would wait for each other. The state is recorded under
// the lock (`value`) and stored once it is released, by each thread that
// changed it: each stores the latest value and then checks it is still the
// latest, so whatever the order the threads' stores land in, the last one
// is the current state.
//
// A store that lands late must not land after a thread the change released
// has gone back to the guest: on the 360 KeSetEvent writes the header under
// the dispatcher's lock, so once a waiter returns nothing writes it any more,
// and the guest may free the KEVENT (often on the waiter's stack) or
// initialize another one there. So a setter counts its store as in flight
// under the lock (BeginPublishLocked), and a wait that took the object
// waits, without the lock, until no store begun before is in flight
// (AwaitPublishes, from WaitForObjects).
struct GuestSignalState
{
    be<uint32_t>* field = nullptr;  // in guest memory; nullptr: no guest header
    std::atomic<uint32_t> value{ 0 };
    std::atomic<uint32_t> inFlight{ 0 };  // stores begun under Lock() and not yet done

    GuestSignalState() = default;
    GuestSignalState(be<uint32_t>* field, uint32_t value) : field(field), value(value) {}

    // With Lock() held, at every change of the host object's state.
    void SetLocked(uint32_t v) { value.store(v); }
    // With Lock() held, by a thread that will PublishAndEnd() once it has
    // released it.
    void BeginPublishLocked() { inFlight.fetch_add(1, std::memory_order_relaxed); }
    // Without Lock(), while the object is sure to stay allocated.
    void Publish();
    // Publish, then the end of the store BeginPublishLocked counted.
    void PublishAndEnd();
    // Without Lock(): returns once every store counted before is done.
    void AwaitPublishes();
};

struct EventObject final : Waitable, HostObject<XKEVENT>
{
    bool manualReset;
    bool signaled;
    GuestSignalState guest;  // guest-embedded event: SignalState is mirrored

    explicit EventObject(XKEVENT* header);
    EventObject(bool manualReset, bool initialState);

    bool IsSignaledLocked() const override { return signaled; }
    void AcquireLocked() override;
    void PublishGuestState() override { guest.Publish(); }
    void AwaitGuestPublishes() override { guest.AwaitPublishes(); }

    // Each returns the previous signal state.
    bool Set();
    bool Reset();
    bool Pulse();

private:
    // Records the state for the guest header. True when there is one: the
    // caller then publishes it once Lock() is released.
    bool MirrorLocked();
};

struct SemaphoreObject final : Waitable, HostObject<XKSEMAPHORE>
{
    int32_t count;
    int32_t limit;
    GuestSignalState guest;  // guest-embedded semaphore: the count is mirrored

    explicit SemaphoreObject(XKSEMAPHORE* semaphore);
    SemaphoreObject(int32_t count, int32_t limit);

    bool IsSignaledLocked() const override { return count > 0; }
    void AcquireLocked() override;
    void PublishGuestState() override { guest.Publish(); }
    void AwaitGuestPublishes() override { guest.AwaitPublishes(); }
    // Returns STATUS_SUCCESS or STATUS_SEMAPHORE_LIMIT_EXCEEDED.
    uint32_t Release(int32_t releaseCount, int32_t* previousCount);
};

struct MutantObject final : Waitable
{
    uint32_t owner = 0;      // guest thread id; 0 = unowned
    uint32_t recursion = 0;

    explicit MutantObject(bool initialOwner);

    bool IsSignaledLocked() const override;
    void AcquireLocked() override;
    // Returns STATUS_SUCCESS or STATUS_MUTANT_NOT_OWNED; *previous = old count.
    uint32_t Release(int32_t* previous);
};

struct TimerObject final : Waitable
{
    bool manualReset;        // NotificationTimer (type 0) vs SynchronizationTimer (1)
    bool signaled = false;
    uint64_t generation = 0; // bumped by Set/Cancel; stale firings are ignored
    uint32_t periodMs = 0;
    int64_t due = 0;         // guest ns

    explicit TimerObject(uint32_t timerType);

    bool IsSignaledLocked() const override { return signaled; }
    void AcquireLocked() override { if (!manualReset) signaled = false; }

    // Returns the previous signal state. `dueTime` is guest ns (nullopt: now).
    bool Set(std::optional<int64_t> dueTime, uint32_t periodMs);
    bool Cancel();
};

// Host object bound to a guest-embedded dispatcher header (KEVENT in game
// memory, ...), created on first use from the header's Type.
Waitable* BindDispatcherHeader(XDISPATCHER_HEADER& header);

// Waitable for a handle, including the current-thread pseudo-handle.
// Returns nullptr for invalid or non-waitable handles.
Waitable* WaitableFromHandle(uint32_t handle);
