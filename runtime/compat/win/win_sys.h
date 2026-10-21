// SpeedBreaker Windows port: the Windows calls the runtime makes, behind
// plain types (compat/win/win_compat.cpp implements them; <windows.h> stays
// out of the runtime's sources, see win_compat.h). GPL-3.0-or-later.
#pragma once
#include <cstdint>
#include <string>

namespace winsys
{
    // A high-resolution waitable timer (Windows 10 1803+), or null. It
    // wakes within about half a millisecond, whatever the system timer tick
    // (15.6 ms unless a process asked for less, and Windows 11 doesn't honour
    // such a request while the window is hidden).
    void* CreatePreciseTimer();
    void ClosePreciseTimer(void* timer);
    // Waits `ns` nanoseconds on the timer. False if it couldn't (the caller
    // sleeps some other way).
    bool WaitPrecise(void* timer, int64_t ns);
    // timeBeginPeriod(1) for this process; true if granted.
    bool RaiseTimerResolution();
    // The calling thread into MMCSS's "Pro Audio" (or "Audio") task, as
    // WASAPI's own threads are; true if it joined.
    bool JoinMmcssAudio();
    void RaiseThreadPriority();

    // At startup: the system timer at 1 ms for this process, kept so while
    // the window is hidden (Windows 11 ignores the request then unless told
    // otherwise), and no EcoQoS throttling of the game's threads. Every
    // std::this_thread::sleep_for and sleep_until goes through
    // win_compat.cpp's nanosleep, on a high-resolution timer, whatever the
    // tick; this is for the waits that take a millisecond timeout
    // (condition variables, events).
    void PreciseTimers();

    // WaitOnAddress: sleeps while the 32-bit value at `address` is
    // `expected`, `timeoutNs` at most (rounded up to the millisecond). False
    // at the timeout. WakeOne and WakeAll end such waits.
    bool WaitOnValue(void* address, uint32_t expected, int64_t timeoutNs);
    void WakeOne(void* address);
    void WakeAll(void* address);

    // A string value under HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE, as
    // UTF-8 (empty: none).
    std::string UserRegistryString(const char* key, const char* value);
    std::string MachineRegistryString(const char* key, const char* value);

    // For the system report (report/system_info.cpp): "Windows 11 Pro 24H2
    // (build 26100.1234)", the physical cores, and memory in bytes.
    std::string OsDescription();
    unsigned PhysicalCores();
    struct Memory
    {
        uint64_t total = 0, available = 0, pageFile = 0;
    };
    Memory MemoryStatus();
}
