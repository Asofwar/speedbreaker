// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// The motion vector prototype's CPU side (gpu/motion_vectors.h): the
// jitter sequence, the matrices the camera's reprojection is made from, the
// matching of a frame's draws with the previous frame's (the race's lamp
// posts: one mesh at many places) and the view-projection's vote.
//
// Build by hand: c++ -std=c++20 -O2 -I runtime tests/motion_vectors_test.cpp
#include <gpu/motion_vectors.h>

#include <cmath>
#include <cstdio>

using namespace gpu::mv;

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            s_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)

    // The race's view-projection at step 4800 (c0..c3 of its scene draws).
    Mat4 RaceVp()
    {
        const float v[16] = { -0.22762f, -0.88273f, 0.00001f, 58.42f, -0.04716f, 0.01217f, 1.54248f, -89.3588f,
            0.96788f, -0.24957f, 0.03156f, -2410.98f, 0.96784f, -0.24957f, 0.03156f, -2410.40f };
        Mat4 m;
        for (int i = 0; i < 16; i++)
            m.m[i] = v[i];
        return m;
    }

    void Apply(const Mat4& m, const double p[4], double out[4])
    {
        for (int r = 0; r < 4; r++)
            out[r] = double(m.at(r, 0)) * p[0] + double(m.at(r, 1)) * p[1] + double(m.at(r, 2)) * p[2] + double(m.at(r, 3)) * p[3];
    }

    void TestJitter()
    {
        CHECK(Halton(1, 2) == 0.5f, "%f", Halton(1, 2));
        CHECK(std::fabs(Halton(2, 3) - 2.0f / 3.0f) < 1e-6f, "%f", Halton(2, 3));
        float sx = 0, sy = 0;
        for (uint64_t f = 0; f < 8; f++)
        {
            float x, y;
            Jitter(f, 8, x, y);
            CHECK(x >= -0.5f && x < 0.5f && y >= -0.5f && y < 0.5f, "frame %llu: %f %f", (unsigned long long)f, x, y);
            sx += x;
            sy += y;
        }
        // Over its phases the sequence is about centred.
        CHECK(std::fabs(sx / 8) < 0.1f && std::fabs(sy / 8) < 0.1f, "mean %f %f", sx / 8, sy / 8);
        float x0, y0, x8, y8;
        Jitter(0, 8, x0, y0);
        Jitter(8, 8, x8, y8);
        CHECK(x0 == x8 && y0 == y8, "phase 8 repeats phase 0");
    }

    void TestCamera()
    {
        const Mat4 vp = RaceVp();
        Mat4 inv;
        CHECK(Invert(vp, inv), "the race's view-projection inverts");
        const Mat4 id = Multiply(vp, inv);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                CHECK(std::fabs(id.at(r, c) - (r == c ? 1.0f : 0.0f)) < 1e-3f, "VP VP^-1 [%d][%d] = %f", r, c, id.at(r, c));
        // The camera moved 0.5 along world x: a static point's previous clip
        // position is the previous matrix's, from its current one.
        Mat4 prev = vp;
        for (int r = 0; r < 4; r++)
            prev.at(r, 3) += prev.at(r, 0) * 0.5f;
        const Mat4 camera = Multiply(prev, inv);
        const double world[4] = { 2410.0, 60.0, -5.0, 1.0 };
        double cur[4], want[4], got[4];
        Apply(vp, world, cur);
        Apply(prev, world, want);
        Apply(camera, cur, got);
        for (int i = 0; i < 2; i++)
            CHECK(std::fabs(got[i] / got[3] - want[i] / want[3]) < 1e-3, "ndc[%d] %f, want %f", i, got[i] / got[3], want[i] / want[3]);
    }

    void TestHistory()
    {
        // Three lamp posts of one mesh (one key), 20 m apart; the camera
        // moved a little: each must find itself, not a neighbour.
        DrawHistory h;
        auto post = [](float x, float move) {
            Mat4 m = RaceVp();
            m.at(0, 3) += x + move;
            return m;
        };
        for (float x : { 0.0f, 20.0f, 40.0f })
            h.Record(7, post(x, 0.0f).m, 4);
        h.NextFrame();
        for (float x : { 40.0f, 0.0f, 20.0f })
        {
            const Mat4 now = post(x, 0.3f);
            const float* p = h.Previous(7, now.m, 4);
            CHECK(p && std::fabs(p[3] - post(x, 0.0f).m[3]) < 1e-3f, "post at %f matched %f", x, p ? p[3] : -1.0f);
            h.Record(7, now.m, 4);
        }
        CHECK(!h.Previous(8, post(0, 0).m, 4), "another key has no previous draw");
        CHECK(!h.Previous(7, post(0, 0).m, 5), "another constant count has none");
        h.NextFrame();
        h.NextFrame();
        CHECK(!h.Previous(7, post(0, 0).m, 4), "two frames on, nothing");
    }

    void TestVote()
    {
        CameraVote v;
        const Mat4 vp = RaceVp();
        Mat4 car = vp;
        car.at(0, 3) = 1.0f;
        for (int i = 0; i < 5; i++)
            v.Add(vp.m);
        for (int i = 0; i < 3; i++)
            v.Add(car.m);
        Mat4 won;
        uint32_t count, total;
        CHECK(v.Winner(won, count, total) && count == 5 && total == 8 && won.m[3] == vp.m[3], "count %u total %u", count, total);
        v.Clear();
        CHECK(!v.Winner(won, count, total), "no votes, no winner");
    }
}

int main()
{
    TestJitter();
    TestCamera();
    TestHistory();
    TestVote();
    if (s_failures)
    {
        fprintf(stderr, "motion_vectors_test: %d failures\n", s_failures);
        return 1;
    }
    printf("motion_vectors_test: ok\n");
    return 0;
}
