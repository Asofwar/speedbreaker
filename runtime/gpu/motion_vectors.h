// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Motion vectors for a temporal upscaler (NFSMW_MV, a prototype; off by
// default). The game gives none, so they are made from what every draw of
// the scene already has: its vertex shader's position constants (the
// translator finds which constants a shader's position depends on,
// TranslatedShader::positionConsts), this frame's and the previous frame's.
//
//   - Per draw (renderer.cpp, Draw): the scene's draws are matched with the
//     previous frame's by what they draw (shader, vertex and index buffers,
//     count) and, among draws of the same mesh (traffic cars of one model,
//     lamp posts), by the nearest position constants. The vertex shader's
//     motion vector variant runs twice, the second time with the matched
//     draw's constants: each vertex's previous position, for a static road,
//     the player's car, a traffic car or a wheel alike. Its pixel shader
//     writes the difference to an extra attachment of the scene's pass.
//   - The camera alone (the depth reprojection): the view-projection is the
//     position matrix most of the scene's draws share (the world is drawn in
//     world space), so previous clip = VP_prev * VP_cur^-1 * current clip
//     for every pixel of the depth buffer. Right for everything static, and
//     wrong for every car: the upscaler's fallback where no draw wrote a
//     motion vector, and an A/B mode.
//   - Jitter: the scene's draws (only those: never the HUD or the game's own
//     post-processing) are shifted by a sub-pixel Halton (2, 3) offset each
//     frame through the host transform (posScale), after the positions the
//     motion vectors are made from.
//
// Header only, no Vulkan (tests can use it alone).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace gpu::mv
{
    // The radical inverse of `index` in `base` (index >= 1): the Halton sequence.
    inline float Halton(uint32_t index, uint32_t base)
    {
        float f = 1.0f, r = 0.0f;
        while (index > 0)
        {
            f /= float(base);
            r += f * float(index % base);
            index /= base;
        }
        return r;
    }

    // Frame `frame`'s sub-pixel offset, in render pixels, in [-0.5, 0.5):
    // Halton (2, 3) over `phases` frames (FSR 2 asks for 8 x (display /
    // render width)^2).
    inline void Jitter(uint64_t frame, uint32_t phases, float& x, float& y)
    {
        const uint32_t i = uint32_t(frame % std::max(phases, 1u)) + 1;
        x = Halton(i, 2) - 0.5f;
        y = Halton(i, 3) - 0.5f;
    }

    // 4x4 matrices as the game's shaders hold them: row r is constant k + r,
    // clip[r] = dot(row r, position).
    struct Mat4
    {
        float m[16] = {};
        float& at(int r, int c) { return m[r * 4 + c]; }
        float at(int r, int c) const { return m[r * 4 + c]; }
    };

    inline Mat4 Multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                double s = 0;
                for (int k = 0; k < 4; k++)
                    s += double(a.at(i, k)) * double(b.at(k, j));
                r.at(i, j) = float(s);
            }
        return r;
    }

    // False when singular.
    inline bool Invert(const Mat4& in, Mat4& out)
    {
        double a[4][8];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 8; c++)
                a[r][c] = c < 4 ? double(in.at(r, c)) : (c - 4 == r ? 1.0 : 0.0);
        for (int c = 0; c < 4; c++)
        {
            int p = c;
            for (int r = c + 1; r < 4; r++)
                if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
                    p = r;
            if (std::fabs(a[p][c]) < 1e-12)
                return false;
            if (p != c)
                for (int k = 0; k < 8; k++)
                    std::swap(a[p][k], a[c][k]);
            const double d = a[c][c];
            for (int k = 0; k < 8; k++)
                a[c][k] /= d;
            for (int r = 0; r < 4; r++)
                if (r != c)
                {
                    const double f = a[r][c];
                    for (int k = 0; k < 8; k++)
                        a[r][k] -= f * a[c][k];
                }
        }
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                out.at(r, c) = float(a[r][c + 4]);
        return true;
    }

    // The scene's draws, frame to frame: for each, the float constants its
    // position depends on (`n` vec4s, as the shader's positionConsts list
    // them), keyed by what it draws.
    class DrawHistory
    {
    public:
        // A new frame: the one recorded so far becomes the previous one.
        void NextFrame()
        {
            prev_.swap(cur_);
            cur_.clear();
            prevValues_.swap(curValues_);
            curValues_.clear();
        }

        // The previous frame's constants of a draw of `key` nearest to
        // `cone` (squared distance over all values), or null when that frame
        // had none with the same count.
        const float* Previous(uint64_t key, const float* cone, uint32_t n, float* distance = nullptr) const
        {
            auto it = prev_.find(key);
            if (it == prev_.end())
                return nullptr;
            const float* best = nullptr;
            double bestD = 0;
            for (const Entry& e : it->second)
            {
                if (e.n != n)
                    continue;
                const float* v = prevValues_.data() + e.offset;
                double d = 0;
                for (uint32_t i = 0; i < n * 4; i++)
                {
                    const double x = double(v[i]) - double(cone[i]);
                    d += x * x;
                }
                if (!best || d < bestD)
                {
                    best = v;
                    bestD = d;
                }
            }
            if (distance)
                *distance = float(bestD);
            return best;
        }

        void Record(uint64_t key, const float* cone, uint32_t n)
        {
            const uint32_t offset = uint32_t(curValues_.size());
            curValues_.insert(curValues_.end(), cone, cone + n * 4);
            cur_[key].push_back({ n, offset });
        }

        size_t Draws() const
        {
            size_t s = 0;
            for (const auto& [k, v] : cur_)
                s += v.size();
            return s;
        }

    private:
        struct Entry
        {
            uint32_t n, offset;
        };
        std::unordered_map<uint64_t, std::vector<Entry>> cur_, prev_;
        std::vector<float> curValues_, prevValues_;
    };

    // The view-projection: the 4-constant position matrix most of a frame's
    // scene draws share.
    class CameraVote
    {
    public:
        void Clear() { votes_.clear(); }
        void Add(const float m[16])
        {
            uint64_t h = 0xCBF29CE484222325ull;
            uint32_t w[16];
            memcpy(w, m, sizeof(w));
            for (uint32_t x : w)
                h = (h ^ x) * 0x100000001B3ull;
            Vote& v = votes_[h];
            if (!v.count)
                memcpy(v.m.m, m, sizeof(v.m.m));
            v.count++;
        }
        // False when no draw voted.
        bool Winner(Mat4& out, uint32_t& count, uint32_t& total) const
        {
            count = total = 0;
            for (const auto& [h, v] : votes_)
            {
                total += v.count;
                if (v.count > count)
                {
                    count = v.count;
                    out = v.m;
                }
            }
            return count > 0;
        }

    private:
        struct Vote
        {
            Mat4 m;
            uint32_t count = 0;
        };
        std::unordered_map<uint64_t, Vote> votes_;
    };
}
