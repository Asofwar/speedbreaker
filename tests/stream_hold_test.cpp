// Tests for game::streams (runtime/game/stream_hold.h): the frame in which a
// timedemo's main thread sees an audio stream's first data. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/stream_hold_test.cpp -o build/stream_hold_test
//   build/stream_hold_test
#include <game/stream_hold.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    using game::streams::Action;
    using game::streams::Channel;
    using game::streams::kHeldFrames;
    using game::streams::Next;

    const char* Name(Action a)
    {
        switch (a)
        {
        case Action::Pass: return "pass";
        case Action::Hide: return "hide";
        case Action::Wait: return "wait";
        }
        return "?";
    }

    // The default race's intro stream, as replays saw it: started at loop
    // 4412, its data (2569 ms) at 4413, 4414 or 4415. Every one of them shows
    // the data at 4414 (kHeldFrames after the start), waiting at 4414 for a
    // read still to land.
    void DataAtAnyFrame()
    {
        static_assert(kHeldFrames == 2);
        for (uint32_t landed : { 4412u, 4413u, 4414u, 4415u, 4420u })
        {
            Channel c;
            uint32_t shownAt = 0;
            // Before the start: not playing.
            CHECK(Next(c, 4411, 0, 0) == Action::Pass, "not started");
            for (uint32_t loop = 4412; loop < 4430 && !shownAt; loop++)
            {
                // Two asks a frame, as the game's stream class makes.
                for (int ask = 0; ask < 2 && !shownAt; ask++)
                {
                    uint32_t position = loop >= landed ? 2569 : 0;
                    Action a = Next(c, loop, 1, position);
                    if (loop < 4412 + kHeldFrames)
                        CHECK(a == (position ? Action::Hide : Action::Pass), "landed %u, loop %u: %s", landed, loop, Name(a));
                    else if (!position)
                    {
                        CHECK(a == Action::Wait, "landed %u, loop %u: %s", landed, loop, Name(a));
                        // The wait asks again until the read lands (here, at
                        // once): the same frame shows it.
                        a = Next(c, loop, 1, 2569);
                        CHECK(a == Action::Pass, "landed %u, loop %u, after the wait: %s", landed, loop, Name(a));
                        shownAt = loop;
                    }
                    else
                    {
                        CHECK(a == Action::Pass, "landed %u, loop %u: %s", landed, loop, Name(a));
                        shownAt = loop;
                    }
                }
            }
            CHECK(shownAt == 4414, "landed %u: shown at %u", landed, shownAt);
            // From then on as it is, its position back to 0 at its end included.
            CHECK(Next(c, 4500, 1, 3000) == Action::Pass, "flowing");
            CHECK(Next(c, 4600, 1, 0) == Action::Pass, "flowing, at its end");
        }
    }

    // Stopped and started again: held again from the new start.
    void Restart()
    {
        Channel c;
        CHECK(Next(c, 100, 1, 0) == Action::Pass, "start");
        CHECK(Next(c, 101, 1, 500) == Action::Hide, "held");
        CHECK(Next(c, 102, 1, 500) == Action::Pass, "shown");
        CHECK(Next(c, 200, 0, 0) == Action::Pass, "stopped");
        CHECK(c.phase == Channel::Phase::Idle, "idle after the stop");
        CHECK(Next(c, 300, 1, 700) == Action::Hide, "restarted with data at once: held");
        CHECK(c.startLoop == 300, "the new start: %u", c.startLoop);
        CHECK(Next(c, 301, 1, 700) == Action::Hide, "held");
        CHECK(Next(c, 302, 1, 700) == Action::Pass, "shown two frames on");
    }

    // A stream stopped while held (its data never came): no wait.
    void StoppedWhileHeld()
    {
        Channel c;
        CHECK(Next(c, 10, 1, 0) == Action::Pass, "start");
        CHECK(Next(c, 12, 0, 0) == Action::Pass, "stopped before its data");
        CHECK(Next(c, 13, 0, 0) == Action::Pass, "still stopped");
    }

    // A wait that ran out: the stream as it is from then on.
    void GiveUp()
    {
        Channel c;
        CHECK(Next(c, 10, 1, 0) == Action::Pass, "start");
        CHECK(Next(c, 12, 1, 0) == Action::Wait, "due");
        game::streams::GiveUp(c);
        CHECK(Next(c, 12, 1, 0) == Action::Pass, "given up");
        CHECK(Next(c, 13, 1, 0) == Action::Pass, "no second wait");
    }

    // The first ask is a frame or more after the start (the game didn't ask
    // in between): shown, or waited for, at once when that is past the hold.
    void LateFirstAsk()
    {
        Channel c;
        CHECK(Next(c, 50, 1, 900) == Action::Hide, "first seen with data");
        CHECK(Next(c, 60, 1, 900) == Action::Pass, "asked again ten frames on");
        Channel d;
        CHECK(Next(d, 50, 1, 0) == Action::Pass, "first seen without");
        CHECK(Next(d, 60, 1, 0) == Action::Wait, "asked again ten frames on, still none");
    }
}

int main()
{
    DataAtAnyFrame();
    Restart();
    StoppedWhileHeld();
    GiveUp();
    LateFirstAsk();
    if (g_failures)
    {
        printf("stream_hold_test: %d failures\n", g_failures);
        return 1;
    }
    printf("stream_hold_test: all passed\n");
    return 0;
}
