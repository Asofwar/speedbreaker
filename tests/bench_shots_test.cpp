// Tests for the timedemo's frame images (runtime/debug/bench_shots.h):
// NFSMW_BENCH_SHOTS's steps, the game's swaps against its frames, which swap
// is a frame's picture, and the PPM a picture is written as. From the repo
// root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/bench_shots_test.cpp -o build/bench_shots_test
//   build/bench_shots_test
#include <debug/bench_shots.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace bench::shots;

namespace
{
    std::string Join(const std::vector<int64_t>& v)
    {
        std::string out;
        for (size_t i = 0; i < v.size(); i++)
            out += (i ? "," : "") + std::to_string(v[i]);
        return out;
    }

    void Steps()
    {
        std::string bad;
        CHECK(Join(ParseSteps("1600,2200,2800", &bad)) == "1600,2200,2800" && bad.empty(), "plain list: %s", Join(ParseSteps("1600,2200,2800")).c_str());
        // Spaces around the numbers, any order, repeats: sorted, each once.
        CHECK(Join(ParseSteps(" 2200 ,1600,\t2200 , 0", &bad)) == "0,1600,2200" && bad.empty(), "spaces, order, repeats: %s",
            Join(ParseSteps(" 2200 ,1600,\t2200 , 0")).c_str());
        // Empty parts are nothing; a value that isn't a step count is left out and named.
        bad.clear();
        std::vector<int64_t> steps = ParseSteps("1600,,abc,-5,2.5,3000,", &bad);
        CHECK(Join(steps) == "1600,3000", "bad parts left out: %s", Join(steps).c_str());
        CHECK(bad == "abc,-5,2.5", "bad parts named: \"%s\"", bad.c_str());
        CHECK(ParseSteps("").empty(), "empty text: no steps");
        CHECK(ParseSteps(" , ").empty(), "only separators: no steps");
        // Absurdly long numbers are refused, not overflowed.
        bad.clear();
        CHECK(ParseSteps("99999999999999999999", &bad).empty() && !bad.empty(), "too long: refused");
    }

    void Pictures()
    {
        // As many swaps as frames before each frame (offset 0): the frame
        // that began at loop 4497 is the game's swap 4498.
        CHECK(PictureSwap(0, 4497) == 4498, "offset 0: %llu", (unsigned long long)PictureSwap(0, 4497));
        CHECK(PictureSwap(3, 4497) == 4501, "3 swaps ahead: %llu", (unsigned long long)PictureSwap(3, 4497));
        CHECK(PictureSwap(-2, 4497) == 4496, "2 swaps behind: %llu", (unsigned long long)PictureSwap(-2, 4497));
    }

    void Offsets()
    {
        // A race as the Deck replayed it: the intro at the vblank's pace (the
        // render thread caught up at nearly every frame's start, now and then
        // a frame behind), then the uncapped window, the render thread one
        // to four frames behind the loop.
        {
            SwapOffset o;
            CHECK(!o.Known() && o.Offset() == 0, "nothing noted yet");
            for (uint32_t loop = 2897; loop < 4427; loop++)
                o.Note(loop % 70 == 0 ? loop - 1 : loop, loop);  // the race's intro
            CHECK(o.Offset() == 0, "offset %lld", (long long)o.Offset());
            CHECK(o.Frames() == 4427 - 2897 && o.CaughtUp() > 1400, "frames at it: %llu of %llu", (unsigned long long)o.CaughtUp(),
                (unsigned long long)o.Frames());
            for (uint32_t loop = 4427; loop < 4497; loop++)
                o.Note(loop - 1 - loop % 4, loop);  // uncapped, behind: doesn't lower it
            CHECK(o.Offset() == 0, "behind doesn't count: %lld", (long long)o.Offset());
            o.Freeze();  // the first shot
            for (uint32_t loop = 4497; loop < 8240; loop++)
                o.Note(loop - 1 - loop % 5, loop);
            CHECK(o.Verified() && o.Past() == 0, "verified");
            CHECK(PictureSwap(o.Offset(), 5097) == 5098, "a later shot's swap");
        }
        // The counts apart: two swaps more than frames. Frames behind before
        // the render thread caught up don't set it; the most seen does.
        {
            SwapOffset o;
            o.Note(100, 100);
            o.Note(101, 101);
            CHECK(o.Offset() == 0 && o.CaughtUp() == 2, "0 so far");
            o.Note(104, 102);  // two swaps that aren't a frame's
            CHECK(o.Offset() == 2 && o.CaughtUp() == 1, "now 2, seen once: %lld, %llu", (long long)o.Offset(),
                (unsigned long long)o.CaughtUp());
            o.Note(104, 103);  // a frame behind
            o.Note(107, 105);
            CHECK(o.Offset() == 2 && o.CaughtUp() == 2, "2, seen twice");
        }
        // A swap that isn't a frame's after the first shot: unverified.
        {
            SwapOffset o;
            for (uint32_t loop = 10; loop < 100; loop++)
                o.Note(loop, loop);
            o.Freeze();
            o.Note(100, 100);
            CHECK(o.Verified(), "verified so far");
            o.Note(102, 101);
            o.Note(105, 102);
            CHECK(!o.Verified() && o.Past() == 2 && o.PastBy() == 3, "past: %llu frames, by %lld", (unsigned long long)o.Past(),
                (long long)o.PastBy());
            CHECK(o.Offset() == 0, "kept: %lld", (long long)o.Offset());
        }
        // No race frame before the first shot: nothing known, unverified.
        {
            SwapOffset o;
            o.Freeze();
            CHECK(!o.Known() && o.Offset() == 0 && !o.Verified(), "no frames: unverified");
        }
    }

    void Files()
    {
        CHECK(FileName(1600) == "shot-1600.ppm", "%s", FileName(1600).c_str());
        CHECK(FileName(0) == "shot-0.ppm", "%s", FileName(0).c_str());

        // 3x2 RGBA, rows 16 bytes apart (padding after each row's 12):
        // RGB kept, alpha and padding dropped, top row first.
        const uint8_t rgba[32] = {
            1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 0, 0xEE, 0xEE, 0xEE, 0xEE,
            10, 11, 12, 1, 13, 14, 15, 2, 16, 17, 18, 3, 0xEE, 0xEE, 0xEE, 0xEE,
        };
        std::string ppm = Ppm(rgba, 3, 2, 16);
        const char header[] = "P6\n3 2\n255\n";
        CHECK(ppm.size() == strlen(header) + 18, "size %zu", ppm.size());
        CHECK(ppm.compare(0, strlen(header), header) == 0, "header: %.12s", ppm.c_str());
        const uint8_t pixels[18] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
        CHECK(ppm.size() >= strlen(header) + 18 && memcmp(ppm.data() + strlen(header), pixels, 18) == 0, "pixels");
        // A 1280x720 frame: 2764800 bytes of pixels after the header.
        std::vector<uint8_t> frame(size_t(1280) * 720 * 4, 0x80);
        CHECK(Ppm(frame.data(), 1280, 720, 1280 * 4).size() == strlen("P6\n1280 720\n255\n") + 2764800, "1280x720");
    }
}

int main()
{
    Steps();
    Pictures();
    Offsets();
    Files();
    if (g_failures)
    {
        printf("bench_shots_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("bench_shots_test: all passed\n");
    return 0;
}
