// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The XAudio render driver (audio.cpp), as the rest of the runtime sees it
// while the game is suspended (iOS: the app isn't the active one; see
// cpu/guest_time.h), and its render cadence.
#pragma once
#include <cstdint>

namespace apu
{
    // Main thread (video's SuspendGame and ResumeGame), idempotent. Suspend
    // pauses the output device: it plays silence without taking what is
    // queued, which then plays first on the resume. Resume starts it again,
    // after guesttime::Resume(): never while the game is still suspended.
    // The render cadence stops and starts with guest time on its own.
    void SuspendOutput();
    void ResumeOutput();

    // A render callback's frame: 256 samples at 48 kHz.
    inline constexpr uint32_t kSamplesPerFrame = 256, kSampleRate = 48000;
    inline constexpr double kFrameNs = kSamplesPerFrame * 1e9 / kSampleRate;  // 5.33 ms

    // Any thread: render callbacks so far (one frame of guest time each),
    // and the audio queued for the device (ms; 0 without one).
    uint64_t RenderedFrames();
    double QueuedMs();

    // How many frames the render thread renders each 5.33 ms tick (audio.cpp),
    // apart from SDL so tests/audio_cadence_test.cpp can run it against a
    // simulated device. It follows the device's clock: one frame a tick, none
    // while SDL's queue is over its target by more than two frames, two while
    // it is under by more than two.
    //
    // And a watchdog (GitHub issue #20, macOS: the audio dropped out, then the
    // game hung). A device that stops taking audio without going away (no
    // disconnect, so SDL neither moves the stream nor drains it) left the
    // queue full, and the rule above rendered nothing from then on: the
    // game's XAudio callback never ran again, its voices never finished, and
    // whatever waited for one (a movie's clock follows the audio played)
    // waited for good. Nothing was logged either: the dropout report lives in
    // the submit, which no longer ran. Over the target for kStallTicks ticks
    // in a row (~1 s) is a stall: a working device takes from the queue once
    // per device buffer, and the largest SDL reports (PipeWire's biggest
    // quantum, 8192 samples: 171 ms) fits into it almost six times; so does
    // the gap while the output moves to new headphones. A false alarm costs
    // a log line and the few frames queued; a real stall costs the game a
    // second without audio time instead of a hang. Stalled, the cadence
    // renders one frame every tick, so the game's audio clock keeps time,
    // and drops what the device doesn't take (it could only play late),
    // until the device takes audio again.
    class RenderCadence
    {
    public:
        // One frame as SDL queues it: 256 samples of 5.1 float.
        static constexpr int kFrameBytes = int(kSamplesPerFrame) * 6 * int(sizeof(float));
        static constexpr uint32_t kStallTicks = kSampleRate / kSamplesPerFrame;  // ~1 s

        enum class Event
        {
            None,
            Stalled,   // the device stopped taking audio
            Restored,  // it takes audio again
        };
        struct Step
        {
            int frames = 1;      // to render this tick
            bool clear = false;  // drop what is queued first
            Event event = Event::None;
        };

        // A tick, with `queuedBytes` in SDL's queue and its target in frames.
        Step Next(int queuedBytes, int targetFrames)
        {
            Step step;
            queuedBytes = queuedBytes < 0 ? 0 : queuedBytes;
            int queued = queuedBytes / kFrameBytes;
            if (m_stalled)
            {
                // Below the level the last tick left: only the device takes
                // from the queue, so it is back, and the rule below goes on
                // from the queue as it is.
                if (queuedBytes < m_after)
                {
                    m_stalled = false;
                    m_over = 0;
                    step.event = Event::Restored;
                }
                else
                {
                    m_stalledTicks++;
                    step.clear = queued > targetFrames + 2;  // held where the rule below holds it
                    return step;
                }
            }
            if (queued > targetFrames + 2)
            {
                step.frames = 0;
                if (++m_over >= kStallTicks)
                {
                    m_stalled = true;
                    m_stalledTicks = 0;
                    step.frames = 1;
                    step.clear = true;
                    step.event = Event::Stalled;
                }
            }
            else
            {
                m_over = 0;
                if (queued < targetFrames - 2)
                    step.frames = 2;
            }
            return step;
        }

        // While stalled, after the tick's frames went in: the queue now, which
        // only the device can lower before the next tick.
        void Rendered(int queuedBytes) { m_after = queuedBytes < 0 ? 0 : queuedBytes; }
        // After a suspension: the device was paused with the game, so ticks
        // over the target before it don't count towards a stall.
        void AfterSuspension() { m_over = 0; }

        bool Stalled() const { return m_stalled; }
        // Ticks rendered stalled, this stall or the last one (for the log).
        uint64_t StalledTicks() const { return m_stalledTicks; }

    private:
        uint32_t m_over = 0;  // ticks over the target in a row
        bool m_stalled = false;
        int m_after = 0;
        uint64_t m_stalledTicks = 0;
    };
}
