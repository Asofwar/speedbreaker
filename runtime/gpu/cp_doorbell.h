// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The command processor's doorbell: how it sleeps while it has nothing to
// do, and who wakes it. Vulkan-free: tests/gpu_cp_doorbell_test.cpp checks
// it alone, and that the sources ring it where they should.
//
// The game's stores to the ring's write pointer land in plain guest memory
// (gpu/command_processor.h: there are no MMIO traps), and so does the store
// of its vblank interrupt handler that ends the GPU's wait for the vblank (a
// WAIT_REG_MEM on D3D's presentation flag). The command processor polled
// both: after each piece of work it spun on yield() for 2 ms, then slept
// 100 us at a time. At 60 fps it waits 5-10 ms of every frame for the
// vblank, so it spun the first 2 ms of each such wait and woke dozens of
// times in the rest: CPU time and power, taken on a Steam Deck from the one
// budget its CPU and GPU share. In the timedemo's race at 60 fps there,
// sleeping on this doorbell instead cut the thread's CPU time by a tenth of
// a core (33.5 s to 27.6 s in the 63.5 s window), its wake-ups from ~3400
// to ~1200 a second and the APU's power by ~0.3 W of 9.8, frame pacing
// unchanged. The wait for the vblank now ends within ~30 us of it, where
// polling took 4-170 us. The price: work right after a long sleep runs on a
// cold core, ~5% slower (CP busy 6.8 to 7.2 ms a frame), which only
// happens when there was time to spare.
//
// What ends those waits now rings this doorbell right after it: the game's
// ring kicks (D3D's two functions that store CP_RB_WPTR, hooked in
// command_processor.cpp), the vblank and GPU interrupts (after the game's
// handler has run), the renderer's completion thread (a submission retired:
// the queue is shallow again and its writes are stored), and a guest thread
// that waits on recorded work (kernel/write_watch.cpp). The command
// processor sleeps on it, with a timeout as the safety net for a store that
// nothing rings for.
//
// Linux: a futex on the count of rings. Ring() is one atomic increment and,
// only while the command processor sleeps, one FUTEX_WAKE: the game's kicks
// cost no system call while it is busy. Both are async-signal-safe (the
// write watch rings from its fault handler). Elsewhere Wait() just sleeps
// out its timeout, and the command processor keeps its polling
// (NFSMW_CP_DOORBELL in command_processor.cpp): untested there, and on
// Apple a sleeping thread's timers are coalesced unless its QoS says not.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#ifdef _WIN32
#include <compat/win/win_sys.h>
#endif
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace gpu
{
    class alignas(64) Doorbell
    {
    public:
#if defined(__linux__) || defined(_WIN32)
        static constexpr bool kBlocking = true;
#else
        static constexpr bool kBlocking = false;
#endif

        // Any thread, after it changed something the waiter checks.
        void Ring()
        {
            // Sequentially consistent against the waiter's Prepare: either
            // this sees it announced (and wakes it), or its count read after
            // the announcement sees this ring (and it won't sleep), and then
            // its check sees the change made before the ring.
            m_rings.fetch_add(1, std::memory_order_seq_cst);
            if (m_sleeping.load(std::memory_order_seq_cst)) [[unlikely]]
                Wake();
        }

        // The waiter (a single thread), before it checks what it waits for:
        // announces it may sleep and returns the count of rings to Wait on.
        // A ring before this is seen by the check that follows; one after it
        // ends the Wait at once.
        uint32_t Prepare()
        {
            m_sleeping.store(true, std::memory_order_seq_cst);
            return m_rings.load(std::memory_order_seq_cst);
        }

        // The waiter, after Prepare and a check that found nothing to do:
        // sleeps until a ring since Prepare, or for `timeout`. True when it
        // was rung (a spurious wake-up returns false; the caller checks
        // again either way). Ends what Prepare began.
        bool Wait(uint32_t rings, std::chrono::nanoseconds timeout)
        {
            if (timeout.count() > 0 && m_rings.load(std::memory_order_seq_cst) == rings)
            {
#ifdef __linux__
                struct timespec ts{ time_t(timeout.count() / 1'000'000'000), long(timeout.count() % 1'000'000'000) };
                // Returns at once when the count has moved on (EAGAIN), when
                // woken, at the timeout, or on a signal (EINTR).
                syscall(SYS_futex, Word(), FUTEX_WAIT_PRIVATE, rings, &ts, nullptr, 0);
#elif defined(_WIN32)
                winsys::WaitOnValue(Word(), rings, timeout.count());
#else
                std::this_thread::sleep_for(timeout);
#endif
            }
            m_sleeping.store(false, std::memory_order_relaxed);
            return m_rings.load(std::memory_order_acquire) != rings;
        }

        // The waiter, when the check after Prepare found work after all.
        void Cancel()
        {
            m_sleeping.store(false, std::memory_order_relaxed);
        }

        // Rings so far (diagnostics: a waiter that timed out and found work
        // with this unchanged since Prepare was not rung for it).
        uint32_t Rings() const
        {
            return m_rings.load(std::memory_order_acquire);
        }

    private:
        void Wake()
        {
#ifdef __linux__
            syscall(SYS_futex, Word(), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#elif defined(_WIN32)
            winsys::WakeOne(Word());
#endif
        }

        uint32_t* Word()
        {
            static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t) && std::atomic<uint32_t>::is_always_lock_free);
            return reinterpret_cast<uint32_t*>(&m_rings);
        }

        std::atomic<uint32_t> m_rings{ 0 };
        std::atomic<bool> m_sleeping{ false };
    };

    // The command processor's (gpu/command_processor.cpp sleeps on it).
    inline Doorbell g_cpDoorbell;

    // A doorbell for any number of waiters: guest threads in D3D's GPU wait
    // loops (debug/d3d_waits.cpp), which poll guest memory the GPU side
    // stores to (a fence, the ring's read pointer write-back) or an
    // interrupt handler does. Each such store rings it after it is made
    // (command_processor.cpp). A waiter reads Rings() before its check of
    // what it waits for, and after a check that found it not done yet Waits
    // on that count: a ring since then ends the Wait at once.
    class alignas(64) WaitBell
    {
    public:
        // Any thread, after a store a waiter may check (async-signal-safe).
        void Ring()
        {
            // Sequentially consistent against Wait's announcement: either this
            // sees a sleeper (and wakes them all), or the sleeper's count read
            // after announcing itself sees this ring (and it doesn't sleep).
            m_rings.fetch_add(1, std::memory_order_seq_cst);
            if (m_sleepers.load(std::memory_order_seq_cst) != 0) [[unlikely]]
                Wake();
        }

        uint32_t Rings() const
        {
            return m_rings.load(std::memory_order_seq_cst);
        }

        // Sleeps until a ring after `rings` was read, or for `timeout`.
        void Wait(uint32_t rings, std::chrono::nanoseconds timeout)
        {
            m_sleepers.fetch_add(1, std::memory_order_seq_cst);
            if (timeout.count() > 0 && m_rings.load(std::memory_order_seq_cst) == rings)
            {
#ifdef __linux__
                struct timespec ts{ time_t(timeout.count() / 1'000'000'000), long(timeout.count() % 1'000'000'000) };
                syscall(SYS_futex, Word(), FUTEX_WAIT_PRIVATE, rings, &ts, nullptr, 0);
#elif defined(_WIN32)
                winsys::WaitOnValue(Word(), rings, timeout.count());
#else
                std::this_thread::sleep_for(timeout);
#endif
            }
            m_sleepers.fetch_sub(1, std::memory_order_relaxed);
        }

    private:
        void Wake()
        {
#ifdef __linux__
            syscall(SYS_futex, Word(), FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, nullptr, nullptr, 0);
#elif defined(_WIN32)
            winsys::WakeAll(Word());
#endif
        }

        uint32_t* Word()
        {
            return reinterpret_cast<uint32_t*>(&m_rings);
        }

        std::atomic<uint32_t> m_rings{ 0 };
        std::atomic<uint32_t> m_sleepers{ 0 };
    };

    // Stores the game's D3D waits poll for (see WaitBell).
    inline WaitBell g_gpuProgress;
}
