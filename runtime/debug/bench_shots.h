// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The timedemo's frame images (NFSMW_BENCH_SHOTS, debug/bench.h): which swap
// is a world step's picture, and the file a picture goes to, apart from the
// game so tests/bench_shots_test.cpp can check them: standard headers only.
//
// Which swap. The game swaps once a frame: its render thread draws each frame
// the main loop has run and swaps it (VdSwap), in order, so the frame that
// began at loop L (RealLoopCounter at the start of that main loop iteration)
// is the game's swap L + 1 + offset, the offset fixed by where the two counts
// started (0 in the supported game, menus and movies included). The command
// processor executes that swap one to seven swaps after the loop began the
// frame, a lag that changes from frame to frame (uncapped, with every hitch),
// and the render thread lags the loop by as much: so the swap either is at
// when the loop reaches a step names a different frame in every run, while
// swap L + 1 + offset names the same one. Two runs of one replay make the
// same swaps in the same order (their frames.csv agree on the draws of each
// swap number), so a picture is the same frame in every run.
//
// The offset is measured, not assumed, and in the race only: the boot,
// menus and loading don't keep it (on the Deck the menus ran two swaps ahead
// of their frames and the race at 0: two of the loading's frames swapped
// nothing). At the start of a race frame L the game has made at most
// L + offset swaps (one for each frame before), and exactly that many when
// its render thread has caught up with the loop, which at the vblank's pace
// (the race's intro, before the measured window) is nearly every frame. So
// the offset is the most swaps - L seen at the start of a race frame, taken
// up to the first shot and kept from then on. A frame that starts with more
// than that later means a swap that wasn't a frame's, and every shot is then
// unverified; so is a run whose race showed no frame before its first shot
// (nothing then says what the offset is).
//
// Holding the main loop to let the render thread catch up doesn't work: the
// render thread needs the loop to go on (held at a frame's start, an
// uncapped replay's render thread stays the four frames behind it was).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace bench::shots
{
    // NFSMW_BENCH_SHOTS: world steps after the race's load, separated by
    // commas (spaces allowed), sorted and each once. Anything that isn't a
    // whole number of steps goes into `bad` (comma-separated) and is left out.
    inline std::vector<int64_t> ParseSteps(std::string_view text, std::string* bad = nullptr)
    {
        std::vector<int64_t> steps;
        size_t at = 0;
        while (at <= text.size())
        {
            size_t comma = text.find(',', at);
            std::string_view part = text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
            at = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
            while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
                part.remove_prefix(1);
            while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
                part.remove_suffix(1);
            if (part.empty())
                continue;
            bool digits = part.size() <= 12;
            for (char c : part)
                digits = digits && c >= '0' && c <= '9';
            if (!digits)
            {
                if (bad)
                    *bad += (bad->empty() ? "" : ",") + std::string(part);
                continue;
            }
            steps.push_back(std::strtoll(std::string(part).c_str(), nullptr, 10));
        }
        std::sort(steps.begin(), steps.end());
        steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
        return steps;
    }

    // The game's swaps against its frames (see the top), noted at the start
    // of every race frame.
    class SwapOffset
    {
    public:
        // At the start of the race frame that began at `loop` the game had
        // made `swaps` swaps.
        void Note(uint64_t swaps, uint32_t loop)
        {
            const int64_t offset = int64_t(swaps) - int64_t(loop);
            frames++;
            if (frozen)
            {
                if (offset > best)
                {
                    past++;
                    pastBy = std::max(pastBy, offset - best);
                }
                return;
            }
            if (offset > best)
            {
                best = offset;
                caughtUp = 0;
            }
            if (offset == best)
                caughtUp++;
        }
        // Keep the offset from now on (the first shot).
        void Freeze() { frozen = true; }
        bool Frozen() const { return frozen; }
        bool Known() const { return best != kNone; }
        int64_t Offset() const { return Known() ? best : 0; }
        // Race frames noted, and of them those at the offset before it was kept.
        uint64_t Frames() const { return frames; }
        uint64_t CaughtUp() const { return caughtUp; }
        // Frames after that past the offset, and by how many swaps at most.
        uint64_t Past() const { return past; }
        int64_t PastBy() const { return pastBy; }
        bool Verified() const { return frozen && caughtUp > 0 && past == 0; }

    private:
        static constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
        int64_t best = kNone;
        uint64_t frames = 0, caughtUp = 0, past = 0;
        int64_t pastBy = 0;
        bool frozen = false;
    };

    // The picture of the frame that began at loop `frameLoop`.
    inline uint64_t PictureSwap(int64_t offset, uint32_t frameLoop)
    {
        return uint64_t(int64_t(frameLoop) + 1 + offset);
    }

    // shot-<step>.ppm, in the run's report folder.
    inline std::string FileName(int64_t step)
    {
        return "shot-" + std::to_string(step) + ".ppm";
    }

    // A binary PPM (P6, 8 bits a channel, top row first) of an RGBA8 image
    // whose rows are `rowBytes` apart; the alpha channel, which the screen
    // never shows, is left out.
    inline std::string Ppm(const uint8_t* rgba, uint32_t width, uint32_t height, size_t rowBytes)
    {
        char header[48];
        int n = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", width, height);
        std::string out(header, size_t(std::max(n, 0)));
        size_t start = out.size();
        out.resize(start + size_t(width) * height * 3);
        char* rgb = out.data() + start;
        for (uint32_t y = 0; y < height; y++)
        {
            const uint8_t* row = rgba + size_t(y) * rowBytes;
            for (uint32_t x = 0; x < width; x++, rgb += 3)
            {
                rgb[0] = char(row[x * 4 + 0]);
                rgb[1] = char(row[x * 4 + 1]);
                rgb[2] = char(row[x * 4 + 2]);
            }
        }
        return out;
    }
}
