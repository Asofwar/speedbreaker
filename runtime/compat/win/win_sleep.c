/* SpeedBreaker Windows port: nanosleep on a high-resolution timer.
 * GPL-3.0-or-later.
 *
 * winpthreads' nanosleep (what libstdc++'s sleep_for and sleep_until call,
 * and the runtime's own nanosleep calls) sleeps with Sleep(), in whole
 * system timer ticks: 15.6 ms unless the process asked for less, and
 * Windows 11 doesn't always honour that. The command processor's 100 us
 * polls and the audio thread's 5.33 ms ticks then wake a tick late. Defined
 * here, all three of its names, so the link never takes winpthreads'
 * nanosleep.o: a high-resolution waitable timer wakes within about half a
 * millisecond (Windows 10 1803+; Sleep() before that).
 *
 * C, not C++: the runtime's C++ sources see winpthreads' inline nanosleep
 * (compat/win/win_compat.h includes <pthread.h>). */
#include <windows.h>
#include <errno.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct sb_timespec64
{
    long long tv_sec;
    long tv_nsec;
};
struct sb_timespec32
{
    long tv_sec;
    long tv_nsec;
};

/* The calling thread's timer, made on its first sleep and closed when the
 * thread ends (a fiber-local slot's callback). */
static DWORD s_slot = FLS_OUT_OF_INDEXES;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static void WINAPI CloseTimer(void* timer)
{
    if (timer && timer != INVALID_HANDLE_VALUE)
        CloseHandle((HANDLE)timer);
}

static BOOL CALLBACK MakeSlot(PINIT_ONCE once, void* parameter, void** context)
{
    (void)once;
    (void)parameter;
    (void)context;
    s_slot = FlsAlloc(CloseTimer);
    return TRUE;
}

static HANDLE ThreadTimer(void)
{
    InitOnceExecuteOnce(&s_once, MakeSlot, NULL, NULL);
    if (s_slot == FLS_OUT_OF_INDEXES)
        return NULL;
    HANDLE timer = (HANDLE)FlsGetValue(s_slot);
    if (!timer)
    {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        FlsSetValue(s_slot, timer ? (void*)timer : (void*)INVALID_HANDLE_VALUE);  /* INVALID: don't try again */
    }
    return timer == INVALID_HANDLE_VALUE ? NULL : timer;
}

static int Sleep64(long long sec, long nsec)
{
    if (sec < 0 || nsec < 0 || nsec >= 1000000000)
    {
        errno = EINVAL;
        return -1;
    }
    const long long ns = sec * 1000000000LL + nsec;
    if (ns <= 0)
    {
        SwitchToThread();
        return 0;
    }
    HANDLE timer = ThreadTimer();
    if (timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);  /* relative, in 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
            return 0;
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
    return 0;
}

int __cdecl nanosleep64(const struct sb_timespec64* request, struct sb_timespec64* remain)
{
    if (!request)
    {
        errno = EINVAL;
        return -1;
    }
    /* Read before `remain` is written: libstdc++ passes the same struct
     * as both. */
    const long long sec = request->tv_sec;
    const long nsec = request->tv_nsec;
    if (remain)
    {
        remain->tv_sec = 0;
        remain->tv_nsec = 0;
    }
    return Sleep64(sec, nsec);
}

int __cdecl nanosleep32(const struct sb_timespec32* request, struct sb_timespec32* remain)
{
    if (!request)
    {
        errno = EINVAL;
        return -1;
    }
    /* Read before `remain` is written: libstdc++ passes the same struct
     * as both. */
    const long long sec = request->tv_sec;
    const long nsec = request->tv_nsec;
    if (remain)
    {
        remain->tv_sec = 0;
        remain->tv_nsec = 0;
    }
    return Sleep64(sec, nsec);
}

/* time_t is 64 bits on x86-64. */
int __cdecl nanosleep(const struct sb_timespec64* request, struct sb_timespec64* remain)
{
    return nanosleep64(request, remain);
}
