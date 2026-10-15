// Tests for the kernel dispatcher (runtime/kernel/dispatcher.cpp, with
// kernel/xdm.cpp's reference counts) on stub guest memory and a stub
// physical heap: a wait keeps its objects allocated when their handles are
// closed meanwhile; a pulse or a set releases the threads waiting then, and
// a pulse never leaves the event signaled for a thread that comes later; the
// SignalState of a guest-embedded KEVENT or KSEMAPHORE is stored outside the
// dispatcher's lock, and a bound header read outside g_kernelLock; a
// periodic timer whose last handle is closed stops. From the repo root:
//   clang++ -std=c++20 -O2 -pthread -Iruntime -Iruntime/kernel -Iruntime/thirdparty/o1heap -Ippc
//     -Itools/XenonRecomp/XenonUtils -Itools/XenonRecomp/thirdparty/simde tests/kernel_dispatcher_test.cpp
//     runtime/kernel/dispatcher.cpp runtime/kernel/xdm.cpp runtime/cpu/guest_time.cpp -o build/kernel_dispatcher_test
//   build/kernel_dispatcher_test
// A few seconds. The lock checks fault where the write watch would wait for
// the GPU: the header's page is protected, and the fault handler waits until
// a helper thread (the command processor's stand-in, whose interrupt handler
// sets events) has taken the lock in question. A lock the faulting thread
// holds fails the check after 2 s instead of deadlocking the test.
#include <stdafx.h>
#include <kernel/dispatcher.h>
#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// What dispatcher.cpp and xdm.cpp need of the runtime (memory.cpp, heap.cpp
// and guest_thread.cpp are not linked): guest memory, a physical heap for
// kernel objects that never reuses memory, poisons what it frees (a use
// after the free calls through a broken vtable) and records which thread
// freed it, and guest thread ids.
Memory::Memory() {}
Memory g_memory;
Heap g_userHeap;

namespace
{
    std::mutex s_heapMutex;
    uint32_t s_nextPhysical = 0x80100000;  // kernel objects sit above 0x80000000 (IsKernelObject)
    std::map<void*, size_t> s_sizes;
    std::map<void*, int> s_freedBy;
    std::atomic<int> s_nextThread{ 1 };
    thread_local int t_thread = 0;

    int ThisThread()
    {
        if (t_thread == 0)
            t_thread = s_nextThread++;
        return t_thread;
    }

    // The test thread that freed `object`, 0 while it's allocated.
    int FreedBy(void* object)
    {
        std::lock_guard lock(s_heapMutex);
        auto it = s_freedBy.find(object);
        return it == s_freedBy.end() ? 0 : it->second;
    }
}

void* Heap::AllocPhysical(size_t size, size_t alignment)
{
    std::lock_guard lock(s_heapMutex);
    alignment = std::max<size_t>(16, alignment);
    s_nextPhysical = uint32_t((s_nextPhysical + alignment - 1) & ~(alignment - 1));
    void* p = g_memory.Translate(s_nextPhysical);
    s_nextPhysical += uint32_t(size);
    s_sizes[p] = size;
    return p;
}

void Heap::Free(void* ptr)
{
    std::lock_guard lock(s_heapMutex);
    memset(ptr, 0xDD, s_sizes[ptr]);
    s_freedBy[ptr] = ThisThread();
}

uint32_t GuestThread::GetCurrentThreadId()
{
    return uint32_t(ThisThread());
}

KernelObject* GuestThread::GetCurrentThreadObject()
{
    return nullptr;
}

namespace
{
    // A waitable object the test sets (`signaled`, under dispatcher::Lock())
    // and that counts its checks: a waiter that checked it once is asleep by
    // the time anyone else gets the lock.
    struct Probe final : Waitable
    {
        bool signaled = false;
        mutable std::atomic<int> checks{ 0 };

        bool IsSignaledLocked() const override
        {
            checks++;
            return signaled;
        }
    };

    int64_t In(int64_t ms)
    {
        return guesttime::NowNs() + ms * 1'000'000;
    }

    uint32_t Wait(std::vector<Waitable*> objects, bool waitAll, int64_t ms)
    {
        return WaitForObjects(objects, waitAll, In(ms));
    }

    // Whether `w` is signaled; an auto-reset object that is gets taken.
    uint32_t Poll(Waitable* w)
    {
        return WaitForObjects({ &w, 1 }, false, guesttime::NowNs());
    }

    void SetProbe(Probe* p)
    {
        std::lock_guard lock(dispatcher::Lock());
        p->signaled = true;
        dispatcher::NotifyAllLocked();
    }

    // A thread in WaitForObjects; `status` is its result once it returned.
    struct Waiter
    {
        std::thread thread;
        std::atomic<uint32_t> status{ 0xFFFFFFFF };
        std::atomic<int> id{ 0 };

        Waiter(std::vector<Waitable*> objects, bool waitAll, int64_t ms)
        {
            thread = std::thread([this, objects, waitAll, ms]
            {
                id = ThisThread();
                status = Wait(objects, waitAll, ms);
            });
        }

        uint32_t Join()
        {
            thread.join();
            return status;
        }
    };

    bool UntilChecked(const Probe* p)
    {
        for (int i = 0; i < 2000; i++)
        {
            if (p->checks.load() > 0)
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void TestPulseReleasesWaiters()
    {
        printf("pulse: the threads waiting are released, and the event ends reset\n");
        {
            auto* e = CreateKernelObject<EventObject>(true, false);
            auto* p1 = CreateKernelObject<Probe>();
            auto* p2 = CreateKernelObject<Probe>();
            Waiter a({ e, p1 }, false, 2000), b({ e, p2 }, false, 2000);
            CHECK(UntilChecked(p1) && UntilChecked(p2), "waiters never checked");
            CHECK(!e->Pulse(), "previous state");
            CHECK(a.Join() == STATUS_WAIT_0, "notification pulse: first waiter got %X", uint32_t(a.status));
            CHECK(b.Join() == STATUS_WAIT_0, "notification pulse: second waiter got %X", uint32_t(b.status));
            CHECK(Poll(e) == STATUS_TIMEOUT, "notification event left signaled by a pulse");
        }
        {
            auto* e = CreateKernelObject<EventObject>(false, false);
            auto* p1 = CreateKernelObject<Probe>();
            auto* p2 = CreateKernelObject<Probe>();
            Waiter a({ e, p1 }, false, 600), b({ e, p2 }, false, 600);
            CHECK(UntilChecked(p1) && UntilChecked(p2), "waiters never checked");
            e->Pulse();
            uint32_t ra = a.Join(), rb = b.Join();
            CHECK((ra == STATUS_WAIT_0) + (rb == STATUS_WAIT_0) == 1 && (ra == STATUS_TIMEOUT) + (rb == STATUS_TIMEOUT) == 1,
                "synchronization pulse released one waiter: %X %X", ra, rb);
            CHECK(Poll(e) == STATUS_TIMEOUT, "synchronization event left signaled by a pulse");
        }
        {
            // Nobody waiting: the pulse is lost, the event stays reset.
            auto* e = CreateKernelObject<EventObject>(true, false);
            e->Pulse();
            CHECK(Poll(e) == STATUS_TIMEOUT, "pulse with no waiter left the event signaled");
        }
    }

    // The audit's case: a thread waits on {F, E}, F is signaled while it
    // sleeps and E pulsed before it runs; it takes F. E used to stay
    // signaled (its pulse counted one waiter still to release), so the next
    // thread to wait on E went through.
    void TestPulseTakenElsewhere()
    {
        printf("pulse: a waiter that takes another object leaves the event reset\n");
        for (bool manual : { false, true })
        {
            auto* e = CreateKernelObject<EventObject>(manual, false);
            auto* f = CreateKernelObject<Probe>();
            Waiter a({ f, e }, false, 2000);
            CHECK(UntilChecked(f), "waiter never checked");
            {
                std::lock_guard lock(dispatcher::Lock());
                f->signaled = true;  // no notify: it's woken by the pulse
            }
            e->Pulse();
            CHECK(a.Join() == STATUS_WAIT_0, "%s: waiter got %X, not F", manual ? "notification" : "synchronization",
                uint32_t(a.status));
            CHECK(Poll(e) == STATUS_TIMEOUT, "%s event signaled after its pulse, for a thread that came later",
                manual ? "notification" : "synchronization");
        }
        {
            // A synchronization event's pulse goes to the waiter that has
            // nothing else signaled, as the kernel's, which has satisfied
            // the other already.
            auto* e = CreateKernelObject<EventObject>(false, false);
            auto* f = CreateKernelObject<Probe>();
            auto* p = CreateKernelObject<Probe>();
            Waiter a({ f, e }, false, 2000);
            CHECK(UntilChecked(f), "first waiter never checked");
            Waiter c({ e, p }, false, 2000);
            CHECK(UntilChecked(p), "second waiter never checked");
            {
                std::lock_guard lock(dispatcher::Lock());
                f->signaled = true;
            }
            e->Pulse();
            CHECK(a.Join() == STATUS_WAIT_0, "first waiter got %X, not F", uint32_t(a.status));
            CHECK(c.Join() == STATUS_WAIT_0, "second waiter got %X, not the pulse", uint32_t(c.status));
            CHECK(Poll(e) == STATUS_TIMEOUT, "event signaled after its pulse");
        }
    }

    // A wait-all satisfied by a pulse when its other objects are signaled
    // then; one that isn't misses the pulse for good.
    void TestPulseWaitAll()
    {
        printf("pulse: wait-all\n");
        {
            auto* e = CreateKernelObject<EventObject>(true, false);
            auto* s = CreateKernelObject<SemaphoreObject>(1, 1);
            auto* p = CreateKernelObject<Probe>();
            p->signaled = true;
            Waiter a({ e, s, p }, true, 2000);
            CHECK(UntilChecked(p), "waiter never checked");
            e->Pulse();
            CHECK(a.Join() == STATUS_WAIT_0, "waiter got %X", uint32_t(a.status));
            CHECK(Poll(s) == STATUS_TIMEOUT, "the semaphore wasn't taken");
            CHECK(Poll(e) == STATUS_TIMEOUT, "event signaled after its pulse");
        }
        {
            auto* e = CreateKernelObject<EventObject>(true, false);
            auto* s = CreateKernelObject<SemaphoreObject>(0, 1);
            auto* p = CreateKernelObject<Probe>();
            p->signaled = true;
            Waiter a({ e, s, p }, true, 500);
            CHECK(UntilChecked(p), "waiter never checked");
            e->Pulse();
            std::this_thread::sleep_for(50ms);
            s->Release(1, nullptr);
            CHECK(a.Join() == STATUS_TIMEOUT, "completed by a pulse that came before its semaphore: %X", uint32_t(a.status));
            CHECK(Poll(s) == STATUS_WAIT_0, "the semaphore was taken");
        }
    }

    // A notification event set and at once reset: the kernel released the
    // threads waiting at the set. Here they used to find it reset when they
    // got to run, and slept on (the setter usually wins the lock back).
    void TestSetThenReset()
    {
        printf("set then reset: the waiters are released\n");
        int missed = 0;
        for (int i = 0; i < 20; i++)
        {
            auto* e = CreateKernelObject<EventObject>(true, false);
            auto* p = CreateKernelObject<Probe>();
            Waiter a({ e, p }, false, 1000);
            CHECK(UntilChecked(p), "waiter never checked");
            e->Set();
            e->Reset();
            missed += a.Join() != STATUS_WAIT_0;
        }
        CHECK(missed == 0, "%d of 20 waiters missed the set", missed);
    }

    // The handle of an object a thread waits on is closed (its last
    // reference): the object stays allocated until the wait is over.
    void TestCloseDuringWait()
    {
        printf("close during a wait: the wait keeps the object\n");
        {
            auto* e = CreateKernelObject<EventObject>(false, false);
            auto* p = CreateKernelObject<Probe>();
            Waiter a({ e, p }, false, 2000);
            CHECK(UntilChecked(p), "waiter never checked");
            ReleaseKernelObject(e);  // NtClose
            if (FreedBy(e) != 0)
            {
                // The waiter would read the poisoned object when it wakes.
                printf("  FAIL %s:%d: the event was freed while a thread waits on it\n", __FILE__, __LINE__);
                fflush(stdout);
                _exit(1);
            }
            SetProbe(p);
            CHECK(a.Join() == STATUS_WAIT_0 + 1, "waiter got %X", uint32_t(a.status));
            CHECK(FreedBy(e) == a.id, "the wait's reference wasn't the last (freed by thread %d, waiter %d)", FreedBy(e), a.id.load());
        }
        {
            // As in the audit: set, then closed, while the waiter is still
            // to wake.
            auto* e = CreateKernelObject<EventObject>(false, false);
            auto* p = CreateKernelObject<Probe>();
            Waiter a({ e, p }, false, 2000);
            CHECK(UntilChecked(p), "waiter never checked");
            e->Set();
            ReleaseKernelObject(e);
            CHECK(a.Join() == STATUS_WAIT_0, "waiter got %X", uint32_t(a.status));
            CHECK(FreedBy(e) != 0, "never freed");
        }
    }

    // A periodic timer whose handle is closed without a cancel: it stops
    // and is freed (it used to fire, and stay allocated, for good).
    void TestClosedPeriodicTimer()
    {
        printf("periodic timer closed: it stops\n");
        auto* t = CreateKernelObject<TimerObject>(1);
        t->Set(In(5), 5);
        std::this_thread::sleep_for(30ms);
        ReleaseKernelObject(t);  // NtClose
        bool freed = false;
        for (int i = 0; i < 1000 && !freed; i++)
        {
            freed = FreedBy(t) != 0;
            std::this_thread::sleep_for(1ms);
        }
        CHECK(freed, "the closed timer wasn't freed");
        uint64_t firings = dispatcher::TimerFirings();
        std::this_thread::sleep_for(50ms);
        CHECK(dispatcher::TimerFirings() == firings, "a closed timer fires on");
    }

    // The write watch's wait for the GPU, as far as the locks go: a fault on
    // the protected page waits until the helper thread could take the lock
    // in question.
    uint8_t* s_page = nullptr;
    size_t s_pageSize = 0;
    std::atomic<std::mutex*> s_probedLock{ nullptr };
    std::atomic<int> s_request{ 0 }, s_granted{ 0 }, s_faults{ 0 };
    std::atomic<bool> s_heldInFault{ false }, s_stop{ false };
    // Slow mode: a fault on the thread marked t_slow waits 200 ms (a GPU
    // still using the page), any other thread's fault opens the page at once.
    std::atomic<bool> s_slowMode{ false };
    std::atomic<int> s_slowDone{ 0 };
    thread_local bool t_slow = false;

    void OnFault(int sig, siginfo_t* info, void*)
    {
        auto* at = static_cast<uint8_t*>(info->si_addr);
        if (s_page == nullptr || at < s_page || at >= s_page + s_pageSize)
        {
            signal(sig, SIG_DFL);  // a real crash: the access faults again
            return;
        }
        if (s_slowMode)
        {
            if (t_slow)
            {
                struct timespec ts{ 0, 200'000'000 };
                nanosleep(&ts, nullptr);
            }
            mprotect(s_page, s_pageSize, PROT_READ | PROT_WRITE);
            if (t_slow)
                ++s_slowDone;
            return;
        }
        int n = ++s_faults;
        s_request = n;
        bool granted = false;
        for (int i = 0; i < 2000 && !(granted = s_granted.load() == n); i++)
        {
            struct timespec ts{ 0, 1'000'000 };
            nanosleep(&ts, nullptr);
        }
        s_request = 0;
        if (!granted)
            s_heldInFault = true;
        mprotect(s_page, s_pageSize, PROT_READ | PROT_WRITE);
    }

    void Helper()
    {
        while (!s_stop)
        {
            // Read before the try: a fault was in progress when it succeeded.
            int n = s_request.load();
            std::mutex* m = s_probedLock.load();
            if (n != 0 && n != s_granted.load() && m != nullptr && m->try_lock())
            {
                m->unlock();
                s_granted = n;
            }
            std::this_thread::sleep_for(100us);
        }
    }

    template<class F>
    void Guarded(const char* what, int prot, std::mutex& lock, F operation)
    {
        s_probedLock = &lock;
        s_heldInFault = false;
        int faults = s_faults;
        mprotect(s_page, s_pageSize, prot);
        operation();
        mprotect(s_page, s_pageSize, PROT_READ | PROT_WRITE);
        CHECK(s_faults > faults, "%s: no fault (nothing tested)", what);
        CHECK(!s_heldInFault, "%s: faulted with the lock held (the GPU's interrupt handler would wait on it)", what);
    }

    void TestGuestHeadersOutsideLocks()
    {
        printf("guest headers: no fault with the dispatcher's lock or g_kernelLock held\n");
        s_pageSize = size_t(sysconf(_SC_PAGESIZE));
        s_page = static_cast<uint8_t*>(g_memory.Translate(0x40000000));
        memset(s_page, 0, s_pageSize);
        auto* ev = reinterpret_cast<XKEVENT*>(s_page);
        ev->Type = 1;  // SynchronizationEvent
        auto* sem = reinterpret_cast<XKSEMAPHORE*>(s_page + 0x100);
        sem->Header.Type = 5;
        sem->Limit = 4;
        auto* e = static_cast<EventObject*>(BindDispatcherHeader(*ev));
        auto* s = static_cast<SemaphoreObject*>(BindDispatcherHeader(sem->Header));

        struct sigaction sa{}, oldSegv{}, oldBus{};
        sa.sa_sigaction = OnFault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, &oldSegv);
        sigaction(SIGBUS, &sa, &oldBus);
        std::thread helper(Helper);

        Guarded("set", PROT_READ, dispatcher::Lock(), [&] { e->Set(); });
        CHECK(uint32_t(ev->SignalState) == 1, "SignalState %u after a set", uint32_t(ev->SignalState));
        Guarded("wait", PROT_READ, dispatcher::Lock(), [&]
        {
            CHECK(Wait({ e }, false, 1000) == STATUS_WAIT_0, "the wait timed out");
        });
        CHECK(uint32_t(ev->SignalState) == 0, "SignalState %u after a wait took it", uint32_t(ev->SignalState));
        e->Set();
        Guarded("reset", PROT_READ, dispatcher::Lock(), [&] { e->Reset(); });
        CHECK(uint32_t(ev->SignalState) == 0, "SignalState %u after a reset", uint32_t(ev->SignalState));
        Guarded("release", PROT_READ, dispatcher::Lock(), [&] { s->Release(2, nullptr); });
        CHECK(uint32_t(sem->Header.SignalState) == 2, "SignalState %u after releasing 2", uint32_t(sem->Header.SignalState));
        Guarded("semaphore wait", PROT_READ, dispatcher::Lock(), [&]
        {
            CHECK(Wait({ s }, false, 1000) == STATUS_WAIT_0, "the wait timed out");
        });
        CHECK(uint32_t(sem->Header.SignalState) == 1, "SignalState %u after a wait took 1", uint32_t(sem->Header.SignalState));
        Guarded("bound header", PROT_NONE, g_kernelLock, [&]
        {
            CHECK(BindDispatcherHeader(*ev) == e, "bound to another object");
        });

        // A waiter the set released returns only after the setter's store into
        // the header has landed: once the wait returns, the guest may free the
        // KEVENT (it is often on the waiter's stack) or initialize another
        // there, and on the 360 nothing writes it after that. The setter's
        // store waits in a slow fault; the waiter's own store goes through.
        printf("guest headers: a released waiter returns after the setter's store\n");
        e->Reset();
        s_slowMode = true;
        int slowBefore = s_slowDone;
        std::atomic<int> slowAtReturn{ -1 };
        std::thread waiter([&]
        {
            CHECK(Wait({ e }, false, 2000) == STATUS_WAIT_0, "the wait timed out");
            slowAtReturn = s_slowDone.load();
        });
        std::this_thread::sleep_for(20ms);  // the waiter is waiting
        mprotect(s_page, s_pageSize, PROT_READ);
        std::thread setter([&]
        {
            t_slow = true;
            e->Set();
        });
        setter.join();
        waiter.join();
        mprotect(s_page, s_pageSize, PROT_READ | PROT_WRITE);
        s_slowMode = false;
        CHECK(s_slowDone > slowBefore, "the setter's store never faulted (nothing tested)");
        CHECK(slowAtReturn > slowBefore, "the waiter returned while the setter's store into the header was still in flight");
        CHECK(uint32_t(ev->SignalState) == 0, "SignalState %u after the waiter took the event", uint32_t(ev->SignalState));

        s_stop = true;
        helper.join();
        sigaction(SIGSEGV, &oldSegv, nullptr);
        sigaction(SIGBUS, &oldBus, nullptr);
        s_page = nullptr;
    }
}

int main()
{
    void* base = mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }
    g_memory.base = static_cast<uint8_t*>(base);

    TestPulseReleasesWaiters();
    TestPulseTakenElsewhere();
    TestPulseWaitAll();
    TestSetThenReset();
    TestCloseDuringWait();
    TestClosedPeriodicTimer();
    TestGuestHeadersOutsideLocks();

    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
