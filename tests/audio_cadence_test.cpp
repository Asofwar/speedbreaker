// Tests for apu::RenderCadence (runtime/apu/audio.h): how many frames the
// audio render thread renders each 5.33 ms tick against a simulated output
// device, and the watchdog for a device that stops taking audio (GitHub
// issue #20). From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/audio_cadence_test.cpp -o build/audio_cadence_test
//   build/audio_cadence_test
#include <apu/audio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using apu::RenderCadence;

namespace
{
    constexpr int kFrame = int(apu::kSamplesPerFrame);
    constexpr int kSampleBytes = RenderCadence::kFrameBytes / kFrame;  // 5.1 float
    constexpr int64_t kSecond = apu::kSampleRate;                      // time in samples at 48 kHz

    // SDL's queue between the render thread and a device that takes `buffer`
    // samples at a time, one buffer's time apart (its clock `rate` times
    // ours), as audio.cpp's thread drives it. The target is FollowDevice's.
    struct Sim
    {
        RenderCadence cadence;
        int buffer;
        double rate;
        int target;
        int64_t queued = 0;  // samples
        int64_t now = 0;     // the next tick, in samples
        double nextPull;
        bool taking = true;  // false: a stalled device (its pulls take nothing)
        bool startedPulls = false;

        // What happened.
        int64_t rendered = 0;
        int stalls = 0, restores = 0;
        int64_t stalledAt = -1, restoredAt = -1;
        int64_t lastUnderrun = -1;  // a pull that found less than a buffer
        int64_t maxQueued = 0;
        int badFrames = 0;   // a step outside 0..2 frames
        int badClears = 0;   // a clear while not stalled
        int64_t stalledUnnominal = 0;  // stalled ticks that didn't render exactly one frame

        Sim(int bufferSamples, double deviceRate, int64_t startAfter = 0)
            : buffer(bufferSamples), rate(deviceRate),
              target(std::max(4, (2 * bufferSamples + kFrame - 1) / kFrame)), nextPull(double(startAfter))
        {
        }

        void Pulls()
        {
            while (nextPull <= double(now))
            {
                if (taking)
                {
                    if (startedPulls && queued < buffer)
                        lastUnderrun = int64_t(nextPull);
                    queued -= std::min<int64_t>(queued, buffer);
                    startedPulls = true;
                }
                nextPull += buffer / rate;
            }
        }

        void Tick()
        {
            Pulls();
            bool wasStalled = cadence.Stalled();
            RenderCadence::Step s = cadence.Next(int(queued * kSampleBytes), target);
            if (s.frames < 0 || s.frames > 2)
                badFrames++;
            if (s.clear && !cadence.Stalled())
                badClears++;
            if (s.event == RenderCadence::Event::Stalled)
            {
                stalls++;
                stalledAt = now;
            }
            if (s.event == RenderCadence::Event::Restored)
            {
                restores++;
                restoredAt = now;
            }
            if (wasStalled && cadence.Stalled() && s.frames != 1)
                stalledUnnominal++;
            if (s.clear)
                queued = 0;
            queued += int64_t(s.frames) * kFrame;
            rendered += s.frames;
            maxQueued = std::max(maxQueued, queued);
            if (cadence.Stalled())
                cadence.Rendered(int(queued * kSampleBytes));
            now += kFrame;
        }

        void Run(int64_t samples)
        {
            for (int64_t end = now + samples; now < end;)
                Tick();
        }

        // A suspension: the thread parks and the device is paused (no pulls),
        // then both go on where they were.
        void Suspend(int64_t samples)
        {
            now += samples;
            nextPull += double(samples);
            cadence.AfterSuspension();
        }
    };

    const int kBuffers[] = { 256, 480, 512, 1024, 2048, 4096, 8192 };

    void Steady()
    {
        printf("a working device, any buffer, its clock a little off or not:\n");
        for (int buffer : kBuffers)
            for (double rate : { 0.999, 1.0, 1.001 })
            {
                Sim sim(buffer, rate);
                sim.Run(120 * kSecond);
                CHECK(sim.stalls == 0, "%d samples at %.3f: no stall (%d)", buffer, rate, sim.stalls);
                CHECK(sim.lastUnderrun < 2 * kSecond, "%d samples at %.3f: no underrun after the start (last at %.2f s)", buffer,
                    rate, double(sim.lastUnderrun) / kSecond);
                // It renders at the device's rate, give or take the queue.
                double want = 120.0 * kSecond * rate / kFrame;
                CHECK(std::abs(double(sim.rendered) - want) <= sim.target + 4, "%d samples at %.3f: %lld frames rendered, %.0f taken",
                    buffer, rate, (long long)sim.rendered, want);
                CHECK(sim.badFrames == 0 && sim.badClears == 0, "%d samples at %.3f: steps within 0..2 frames, no clears", buffer, rate);
            }
        // A device at half the rate (slow, not stopped): followed, not a stall.
        Sim slow(1024, 0.5);
        slow.Run(60 * kSecond);
        CHECK(slow.stalls == 0, "a device at half the rate: no stall (%d)", slow.stalls);
        // A device that starts half a second late (Core Audio starting up).
        Sim late(512, 1.0, kSecond / 2);
        late.Run(10 * kSecond);
        CHECK(late.stalls == 0, "a device starting 0.5 s late: no stall (%d)", late.stalls);
    }

    void Stall()
    {
        printf("a device that stops taking audio for 5 s, then goes on:\n");
        for (int buffer : kBuffers)
        {
            Sim sim(buffer, 1.0);
            sim.Run(10 * kSecond);
            const int64_t stoppedAt = sim.now;
            const int64_t renderedBefore = sim.rendered;
            sim.taking = false;
            sim.Run(5 * kSecond);
            const int64_t resumedAt = sim.now;
            sim.taking = true;
            sim.Run(20 * kSecond);
            CHECK(sim.stalls == 1 && sim.restores == 1, "%d samples: one stall, one restore (%d, %d)", buffer, sim.stalls, sim.restores);
            // Seen within the watchdog's ticks once the queue is full (two
            // targets of frames at most), not before; the ticks over the
            // target can start up to a device buffer before the stop (the
            // pull that would have ended them never came).
            int64_t ticksToStall = (sim.stalledAt - stoppedAt) / kFrame;
            CHECK(ticksToStall >= int64_t(RenderCadence::kStallTicks) - buffer / kFrame &&
                    ticksToStall <= int64_t(RenderCadence::kStallTicks) + 2 * sim.target + 4,
                "%d samples: stall seen after %lld ticks (watchdog %u)", buffer, (long long)ticksToStall, RenderCadence::kStallTicks);
            // Back at the device's first pull after it goes on.
            CHECK(sim.restoredAt >= resumedAt && sim.restoredAt <= resumedAt + buffer + 2 * kFrame, "%d samples: restored %.1f ms after the device went on",
                buffer, 1000.0 * double(sim.restoredAt - resumedAt) / kSecond);
            // Stalled, one frame a tick: the game's audio clock kept time, and
            // the queue stayed at its level.
            CHECK(sim.stalledUnnominal == 0, "%d samples: one frame every stalled tick (%lld ticks otherwise)", buffer,
                (long long)sim.stalledUnnominal);
            int64_t ticks = (sim.restoredAt - stoppedAt) / kFrame;
            int64_t frames = sim.rendered - renderedBefore;
            CHECK(frames >= ticks - int64_t(RenderCadence::kStallTicks) - sim.target, "%d samples: %lld frames rendered in %lld ticks", buffer,
                (long long)frames, (long long)ticks);
            CHECK(sim.maxQueued <= int64_t(sim.target + 4) * kFrame + buffer, "%d samples: queue at most %lld frames (target %d)", buffer,
                (long long)(sim.maxQueued / kFrame), sim.target);
            CHECK(sim.lastUnderrun < sim.restoredAt + 2 * kSecond, "%d samples: no underrun 2 s after the restore (last at %.2f s)", buffer,
                double(sim.lastUnderrun) / kSecond);
            CHECK(sim.badFrames == 0 && sim.badClears == 0, "%d samples: steps within 0..2 frames, clears only stalled", buffer);
        }
        // A device that never takes anything: a stall, then one frame a tick for good.
        Sim dead(1024, 1.0);
        dead.taking = false;
        dead.Run(60 * kSecond);
        CHECK(dead.stalls == 1 && dead.restores == 0, "a dead device: one stall, no restore (%d, %d)", dead.stalls, dead.restores);
        CHECK(dead.rendered >= 60 * kSecond / kFrame - int64_t(RenderCadence::kStallTicks) - dead.target - 4,
            "a dead device: %lld frames rendered in 60 s", (long long)dead.rendered);
        CHECK(dead.maxQueued <= int64_t(dead.target + 4) * kFrame, "a dead device: queue at most %lld frames", (long long)(dead.maxQueued / kFrame));
        // A device that stops again after coming back: a second stall.
        Sim twice(512, 1.0);
        for (int i = 0; i < 2; i++)
        {
            twice.Run(5 * kSecond);
            twice.taking = false;
            twice.Run(3 * kSecond);
            twice.taking = true;
        }
        twice.Run(5 * kSecond);
        CHECK(twice.stalls == 2 && twice.restores == 2, "stopped twice: two stalls, two restores (%d, %d)", twice.stalls, twice.restores);
    }

    void Suspension()
    {
        printf("a suspension (iOS), the device paused with the game:\n");
        // The device takes nothing for 0.6 s, the game is suspended for 30 s,
        // and the device takes another 0.6 s to start again after it: no
        // stall, the time before the suspension doesn't count.
        Sim sim(1024, 1.0);
        sim.Run(10 * kSecond);
        sim.taking = false;
        sim.Run(kSecond * 6 / 10);
        sim.Suspend(30 * kSecond);
        sim.Run(kSecond * 6 / 10);
        sim.taking = true;
        sim.Run(10 * kSecond);
        CHECK(sim.stalls == 0, "0.6 s, suspended, 0.6 s: no stall (%d)", sim.stalls);
        // The same 1.2 s without the suspension is one.
        Sim plain(1024, 1.0);
        plain.Run(10 * kSecond);
        plain.taking = false;
        plain.Run(kSecond * 12 / 10);
        plain.taking = true;
        plain.Run(10 * kSecond);
        CHECK(plain.stalls == 1 && plain.restores == 1, "1.2 s in one go: a stall and a restore (%d, %d)", plain.stalls, plain.restores);
        // Suspended while stalled: still stalled after it, until the device
        // takes audio again.
        Sim stalled(512, 1.0);
        stalled.Run(5 * kSecond);
        stalled.taking = false;
        stalled.Run(2 * kSecond);
        stalled.Suspend(10 * kSecond);
        stalled.Run(kSecond);
        CHECK(stalled.cadence.Stalled() && stalled.restores == 0, "suspended while stalled: still stalled after it");
        stalled.taking = true;
        stalled.Run(kSecond);
        CHECK(!stalled.cadence.Stalled() && stalled.restores == 1, "and restored once the device takes audio");
    }
}

int main()
{
    Steady();
    Stall();
    Suspension();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
