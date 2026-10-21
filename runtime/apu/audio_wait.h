// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// How the audio thread (apu/audio.cpp) waits for its next 5.33 ms tick.
//
// The game's voices hold only a couple of decoded frames, so the render
// callback has to run evenly, one frame per tick. A tick that wakes more than
// one interval late is followed by a catch-up burst (the next deadline has
// passed already, so the thread doesn't sleep), and every frame after the
// first two in a burst is rendered from empty voices: silence. On the output
// that is the "two frames on, one off" chop.
//
// Windows: std::this_thread::sleep_until ends up in a Sleep()/Wait with a
// millisecond timeout, which fires on the system timer tick: 15.6 ms unless
// the process has asked for better. Since Windows 10 2004 timeBeginPeriod only
// helps the process that calls it, and since Windows 11 it isn't honoured
// while the process's window is occluded, minimized or "inaudible", so SDL's
// 1 ms request (SDL_InitTicks) can't be counted on. A high-resolution
// waitable timer (Windows 10 1803+) wakes within about half a millisecond
// whatever the system tick is. The thread also joins MMCSS ("Pro Audio", as
// WASAPI's own threads do) so that a busy core doesn't hold it back.
//
// Anything older than 1803, or a refused timer, falls back to
// timeBeginPeriod(1) and sleep_until. Elsewhere this is sleep_until, as before.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#ifdef _WIN32
#include <compat/win/win_sys.h>

namespace apu
{
    // Create on the audio thread: the scheduling it asks for is the calling
    // thread's. Until() is for that thread only. The Windows calls are in
    // compat/win/win_compat.cpp (<windows.h> stays out of the runtime).
    class AudioWait
    {
    public:
        AudioWait()
        {
            m_timer = winsys::CreatePreciseTimer();
            if (!m_timer)
                m_period = winsys::RaiseTimerResolution();  // before Windows 10 1803: the best the system tick allows
            m_mmcss = winsys::JoinMmcssAudio();
            if (!m_mmcss)
                winsys::RaiseThreadPriority();
        }

        AudioWait(const AudioWait&) = delete;
        AudioWait& operator=(const AudioWait&) = delete;

        ~AudioWait()
        {
            winsys::ClosePreciseTimer(m_timer);
        }

        void Until(std::chrono::steady_clock::time_point t)
        {
            using namespace std::chrono;
            const auto remaining = t - steady_clock::now();
            if (remaining <= steady_clock::duration::zero())
                return;
            if (m_timer && winsys::WaitPrecise(m_timer, duration_cast<nanoseconds>(remaining).count()))
                return;
            std::this_thread::sleep_until(t);
        }

        // For the log.
        const char* Describe() const
        {
            if (m_timer)
                return m_mmcss ? "high-resolution timer, MMCSS" : "high-resolution timer, thread priority";
            return m_mmcss ? (m_period ? "sleep_until at 1 ms, MMCSS" : "sleep_until at the system timer tick, MMCSS")
                           : (m_period ? "sleep_until at 1 ms, thread priority" : "sleep_until at the system timer tick, thread priority");
        }

    private:
        void* m_timer = nullptr;
        bool m_mmcss = false;
        bool m_period = false;
    };
}

#else

namespace apu
{
    class AudioWait
    {
    public:
        void Until(std::chrono::steady_clock::time_point t) { std::this_thread::sleep_until(t); }
        const char* Describe() const { return "sleep_until"; }
    };
}

#endif
