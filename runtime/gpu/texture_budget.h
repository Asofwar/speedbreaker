// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// When the renderer's texture cache (gpu/renderer.cpp) gives textures back,
// and which. Up to v0.1.4 it never did: a texture lived until a setting
// change dropped the cache or the game exited. City streaming reuses guest
// addresses for textures of other sizes and formats, and every new
// combination made a new image, so a long free roam only ever grew the
// cache, until an allocation failed (an abort) or the system killed the game
// (a Steam Deck, an iPhone, a Mac: memory the GPU shares with everything
// else).
//
// Now, at a swap now and then, the cache is measured against its limits: its
// own bytes and texture count (a cap each), and the device's local heaps as
// the driver counts this process's use against its budget
// (VK_EXT_memory_budget, where the device has it: on Apple's, MoltenVK
// counts what the system lets the app have). Over one, the textures unused
// longest are evicted, down to a lower mark (hysteresis: not one texture a
// check), and only those unused for minAge swaps or more: what the current
// scene draws stays, however far over the cache is, so eviction never
// reloads a texture the scene is still drawing (no thrash). The renderer
// destroys an evicted texture only once the submission that could last use
// it has completed. An allocation that fails asks the cache too, with a
// minimum age of one swap (anything the frame being recorded hasn't drawn).
// Vulkan-free: tests/gpu_texture_budget_test.cpp checks it alone.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gpu::texbudget
{
    constexpr uint64_t kMB = 1ull << 20;
    // The cache's default byte cap: half the device's largest local heap
    // (the other half is the driver's, render targets', guest memory's
    // copy and other programs'), within these bounds. Half a 1 GB carve-out
    // (an APU reporting its VRAM alone) is 512 MB; 2 GB is four times the
    // 360's whole memory, which held every texture the game had loaded at
    // once (16-bit formats load as 32-bit ones here, and scaled textures
    // grow with the internal resolution). iOS keeps less: the system ends
    // an app well before its GPU heap is full.
    constexpr uint64_t kDefaultFloor = 512 * kMB;
    constexpr uint64_t kDefaultCeiling = 2048 * kMB;
    constexpr uint64_t kDefaultCeilingIos = 1024 * kMB;
    // Textures at most by default: far more than a scene binds (hundreds),
    // so it bounds the dead ones a long session leaves, not the live ones.
    constexpr uint32_t kDefaultCount = 8192;
    // Swaps a texture must have gone unused to be evicted at a check: 10 s
    // at 60 fps (20 s at 30). An area driven back to within that keeps its
    // textures.
    constexpr uint64_t kDefaultMinAge = 600;

    struct Limits
    {
        uint64_t bytes = 0;  // the cache's bytes at most (0: no cap)
        uint32_t count = 0;  // textures at most (0: no cap)
        uint64_t minAge = kDefaultMinAge;
    };

    constexpr uint64_t DefaultBytes(uint64_t largestDeviceHeap, uint64_t ceiling = kDefaultCeiling)
    {
        return std::clamp(largestDeviceHeap / 2, std::min(kDefaultFloor, ceiling), ceiling);
    }

    struct Usage
    {
        uint64_t bytes = 0;  // the cached textures' memory
        uint32_t count = 0;
        // VK_EXT_memory_budget over the device-local heaps (0: unknown).
        uint64_t deviceUsed = 0, deviceBudget = 0;
    };

    // What a check finds to give back: at least `bytes` and `count` textures
    // (both 0: nothing), and why.
    struct Excess
    {
        uint64_t bytes = 0;
        uint32_t count = 0;
        bool overBytes = false, overCount = false, overDevice = false;
        bool Any() const { return bytes != 0 || count != 0; }
    };

    // Over a cap, down to 7/8 of it; the device's heaps over 9/10 of their
    // budget, down to 4/5 (what the textures can give of it).
    inline Excess Over(const Limits& limits, const Usage& usage)
    {
        Excess e;
        if (limits.bytes && usage.bytes > limits.bytes)
        {
            e.overBytes = true;
            e.bytes = usage.bytes - limits.bytes / 8 * 7;
        }
        if (limits.count && usage.count > limits.count)
        {
            e.overCount = true;
            e.count = usage.count - limits.count / 8 * 7;
        }
        if (usage.deviceBudget && usage.deviceUsed > usage.deviceBudget / 10 * 9)
        {
            e.overDevice = true;
            e.bytes = std::max(e.bytes, std::min(usage.bytes, usage.deviceUsed - usage.deviceBudget / 10 * 8));
        }
        return e;
    }

    struct Candidate
    {
        uint64_t bytes = 0;
        uint64_t lastUsed = 0;  // the swap count at its latest use
    };

    // The candidates to evict (indices), least recently used first (the
    // larger first among equals, then in order), until `excess` is covered:
    // only those unused for `minAge` swaps by `now`, and never one used
    // since the last swap (`now` itself), whatever minAge says. Fewer when
    // the old ones don't cover it: the rest of the scene stays.
    inline std::vector<size_t> Pick(const std::vector<Candidate>& candidates, uint64_t now, uint64_t minAge, const Excess& excess)
    {
        std::vector<size_t> order;
        if (!excess.Any())
            return order;
        minAge = std::max<uint64_t>(minAge, 1);
        for (size_t i = 0; i < candidates.size(); i++)
            if (candidates[i].lastUsed <= now && now - candidates[i].lastUsed >= minAge)
                order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const Candidate& x = candidates[a];
            const Candidate& y = candidates[b];
            return x.lastUsed != y.lastUsed ? x.lastUsed < y.lastUsed : x.bytes > y.bytes;
        });
        uint64_t bytes = 0;
        size_t taken = 0;
        while (taken < order.size() && (bytes < excess.bytes || taken < excess.count))
            bytes += candidates[order[taken++]].bytes;
        order.resize(taken);
        return order;
    }
}
