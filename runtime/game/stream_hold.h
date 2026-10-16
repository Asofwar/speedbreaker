#pragma once
// A timedemo's audio streams as the game's main thread sees them (see
// game/stream_status.cpp): the frame in which a stream the game started shows
// its first data, a fixed number of frames after the main thread first saw it
// started, whenever the stream player's reads actually land. Header only:
// tests/stream_hold_test.cpp checks it alone.
#include <cstdint>

namespace game::streams
{
    // Frames from the one the main thread first sees a stream started in to
    // the one it sees the stream's first data in. Two: the default race's
    // intro stream, started at loop 4412, had its data at 4414 in its capture
    // (on a Steam Deck the read lands within the 120 ms the frame after the
    // intro movie takes), and a capture made with the hold records it.
    inline constexpr uint32_t kHeldFrames = 2;

    // One stream channel, as the main thread has seen it.
    struct Channel
    {
        enum class Phase : uint8_t
        {
            Idle,     // not started (state 0), or never asked about
            Held,     // started at startLoop, no data shown yet
            Flowing,  // its first data shown: as it is from here on
        };
        Phase phase = Phase::Idle;
        uint32_t startLoop = 0;
    };

    // What the main thread's status call gets.
    enum class Action : uint8_t
    {
        Pass,  // the status as it is
        Hide,  // the status with position 0 (the data isn't due yet)
        Wait,  // due, but no data: wait for it and ask again
    };

    // The status the player gave for `channel`, asked in main-loop frame
    // `loop` (RealLoopCounter): its state (0: not playing) and how much of it
    // is in, in ms.
    inline Action Next(Channel& channel, uint32_t loop, uint32_t state, uint32_t positionMs)
    {
        using Phase = Channel::Phase;
        if (state == 0)
        {
            channel.phase = Phase::Idle;
            return Action::Pass;
        }
        if (channel.phase == Phase::Idle)
        {
            channel.phase = Phase::Held;
            channel.startLoop = loop;
        }
        if (channel.phase == Phase::Flowing)
            return Action::Pass;
        if (loop - channel.startLoop < kHeldFrames)
            return positionMs ? Action::Hide : Action::Pass;
        if (positionMs == 0)
            return Action::Wait;
        channel.phase = Phase::Flowing;
        return Action::Pass;
    }

    // A wait that ran out: the stream as it is from here on (no second wait).
    inline void GiveUp(Channel& channel)
    {
        channel.phase = Channel::Phase::Flowing;
    }
}
