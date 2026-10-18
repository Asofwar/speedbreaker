// study/hle-renderer: the D3D side channel for resolves (see hle_resolve.h).
#include <stdafx.h>
#include "hle_resolve.h"

#include <kernel/memory.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <chrono>
#include <unordered_map>

namespace hle
{
    namespace
    {
        const bool s_enabled = [] { const char* v = std::getenv("NFSMW_HLE_RESOLVE"); return v && v[0] == '1'; }();

        constexpr size_t kMaxPending = 512;
        std::mutex s_mutex;
        std::deque<Intent> s_pending;            // in the order the guest made them
        // The last intents passed over or matched: predicated tiling runs a
        // tile's packets once a tile, so a resolve in them comes again after
        // the next call's has matched (the scene's depth, once a tile).
        constexpr size_t kMaxRecent = 16;
        std::deque<Intent> s_recent;
        // Textures' physical bases the CPU has locked, and when it last did: a
        // texture counts as read by the CPU while its last lock is under
        // kLockedFor old (the 64x64 brightness, every frame; not those
        // locked once while loading, whose memory later holds targets).
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_locked;
        constexpr auto kLockedFor = std::chrono::seconds(2);
        bool LockedLately(uint32_t texBase)
        {
            auto it = texBase ? s_locked.find(texBase) : s_locked.end();
            return it != s_locked.end() && std::chrono::steady_clock::now() - it->second < kLockedFor;
        }
        uint64_t s_seq = 0;
        thread_local int t_tiling = 0;           // inside EndTiling on this thread
        thread_local int t_depth = 0;            // Resolve calls open on this thread (it doesn't nest; kept safe)
        thread_local uint64_t t_open[4] = {};    // their intents' seq
        struct Counts { uint64_t made = 0, byPacket = 0, byDest = 0, again = 0, unmatched = 0, skipped = 0, dropped = 0, locks = 0; } s_counts;

        uint32_t Physical(uint32_t address) { return address & 0x1FFFFFFF; }

        // The guest texture header at `tex` (D3DBaseTexture: the fetch
        // constant at +16, big-endian words).
        void ReadTexture(uint8_t* base, uint32_t tex, Intent& in)
        {
            uint32_t d0 = PPC_LOAD_U32(tex + 16), d1 = PPC_LOAD_U32(tex + 20), d2 = PPC_LOAD_U32(tex + 24), d5 = PPC_LOAD_U32(tex + 36);
            in.texHeader = tex;
            in.texFormat = d1 & 0x3F;
            in.texBase = Physical((d1 >> 12) << 12);  // (the fetch constant's address is in a window: A0000000 and up)
            in.texDimension = (d5 >> 9) & 3;  // 0 1D, 1 2D, 2 3D, 3 cube
            in.texPitch = ((d0 >> 22) & 0x1FF) * 32;
            if (in.texDimension == 2)
            {
                in.texWidth = (d2 & 0x7FF) + 1;
                in.texHeight = ((d2 >> 11) & 0x7FF) + 1;
            }
            else
            {
                in.texWidth = (d2 & 0x1FFF) + 1;
                in.texHeight = ((d2 >> 13) & 0x1FFF) + 1;
            }
            // An upper bound of its bytes (32-bit texels, whole 32x32 tiles,
            // six faces for a cube): only to find an intent by destination.
            const uint32_t rows = (in.texHeight + 31) & ~31u;
            in.texBytes = std::max(in.texPitch, in.texWidth) * rows * 4 * (in.texDimension == 3 ? 6 : 1);
        }

        void Push(Intent&& in)
        {
            std::lock_guard lock(s_mutex);
            in.seq = ++s_seq;
            in.cpuLocked = LockedLately(in.texBase);
            if (t_depth > 0 && t_depth <= 4)
                t_open[t_depth - 1] = in.seq;
            s_pending.push_back(in);
            s_counts.made++;
            while (s_pending.size() > kMaxPending)
            {
                s_pending.pop_front();
                s_counts.dropped++;
            }
        }
    }

    bool Enabled() { return s_enabled; }

    void ResolveEnter(PPCContext& ctx, uint8_t* base)
    {
        if (!s_enabled)
            return;
        Intent in;
        const uint32_t dev = ctx.r3.u32;
        in.ringStart = Physical(PPC_LOAD_U32(dev + 0));
        in.flags = ctx.r4.u32;
        if (uint32_t rect = ctx.r5.u32)
            for (int i = 0; i < 4; i++)
                in.rect[i] = int32_t(PPC_LOAD_U32(rect + i * 4));
        else
            in.wholeSurface = true;
        if (uint32_t tex = ctx.r6.u32)
            ReadTexture(base, tex, in);
        if (uint32_t point = ctx.r7.u32)
        {
            in.point = true;
            in.pointX = int32_t(PPC_LOAD_U32(point));
            in.pointY = int32_t(PPC_LOAD_U32(point + 4));
        }
        in.level = ctx.r8.u32;
        in.slice = ctx.r9.u32;
        in.inTiling = t_tiling > 0;
        t_depth++;
        Push(std::move(in));
    }

    void ResolveLeave(PPCContext& ctx, uint8_t* base)
    {
        if (!s_enabled || t_depth <= 0)
            return;
        // (r3 is the return value now: the device is read from the global.)
        const uint32_t devPtr = PPC_LOAD_U32(0x82A2CF60);
        const uint32_t end = devPtr ? Physical(PPC_LOAD_U32(devPtr + 0)) : 0;
        const uint64_t seq = t_depth <= 4 ? t_open[t_depth - 1] : 0;
        t_depth--;
        std::lock_guard lock(s_mutex);
        for (auto it = s_pending.rbegin(); it != s_pending.rend(); ++it)
            if (it->seq == seq)
            {
                it->ringEnd = end;
                break;
            }
    }

    void EndTilingEnter(PPCContext&, uint8_t*)
    {
        if (s_enabled)
            t_tiling++;
    }

    void EndTilingLeave(PPCContext&, uint8_t*)
    {
        if (s_enabled && t_tiling > 0)
            t_tiling--;
    }

    void LockRect(PPCContext& ctx, uint8_t* base)
    {
        if (!s_enabled || !ctx.r3.u32)
            return;
        Intent in;
        ReadTexture(base, ctx.r3.u32, in);
        std::lock_guard lock(s_mutex);
        s_locked[in.texBase] = std::chrono::steady_clock::now();
        s_counts.locks++;
    }

    bool Match(uint32_t packet, uint32_t destBase, Intent& out)
    {
        if (!s_enabled)
            return false;
        packet = Physical(packet);
        destBase = Physical(destBase);
        std::lock_guard lock(s_mutex);
        // Its destination in the intent's texture (none named: any).
        auto into = [&](const Intent& in) {
            return !in.texBase || (destBase >= in.texBase && uint64_t(destBase) < uint64_t(in.texBase) + in.texBytes);
        };
        // Its packet in the intent's: first a call whose packets are one run
        // [start, end), then (pass 1) one that began a new segment of the
        // ring (end below start: it holds nearly every address) or is still
        // running (end 0).
        auto holds = [&](const Intent& in, int pass) {
            if (pass == 0)
                return in.ringEnd != 0 && in.ringEnd >= in.ringStart && packet >= in.ringStart && packet < in.ringEnd;
            if (in.ringEnd == 0)
                return packet >= in.ringStart;
            return in.ringEnd < in.ringStart && (packet >= in.ringStart || packet < in.ringEnd);
        };
        size_t found = s_pending.size();
        bool byPacket = false;
        for (int pass = 0; pass < 2 && found == s_pending.size(); pass++)
            for (size_t i = 0; i < s_pending.size(); i++)
                if (holds(s_pending[i], pass) && into(s_pending[i]))
                {
                    found = i;
                    byPacket = true;
                    break;
                }
        if (found == s_pending.size())
            for (size_t i = 0; i < s_pending.size() && i < 8; i++)
            {
                const Intent& in = s_pending[i];
                if (in.texBase && into(in))
                {
                    found = i;
                    break;
                }
            }
        if (found == s_pending.size())
        {
            // A tile's packets again: an intent already passed over.
            for (auto it = s_recent.rbegin(); it != s_recent.rend(); ++it)
                if (holds(*it, 0) && into(*it))
                {
                    out = *it;
                    out.cpuLocked = out.cpuLocked || LockedLately(out.texBase);
                    s_counts.again++;
                    return true;
                }
            s_counts.unmatched++;
            return false;
        }
        out = s_pending[found];
        out.cpuLocked = out.cpuLocked || LockedLately(out.texBase);
        s_counts.skipped += found;
        (byPacket ? s_counts.byPacket : s_counts.byDest)++;
        // Those before it are passed over (kept a while in s_recent); it
        // stays first: one call can make more than one resolve (a predicated
        // tile's each), all in its packets.
        for (size_t i = 0; i < found; i++)
            s_recent.push_back(s_pending[i]);
        while (s_recent.size() > kMaxRecent)
            s_recent.pop_front();
        s_pending.erase(s_pending.begin(), s_pending.begin() + ptrdiff_t(found));
        return true;
    }

    void Report()
    {
        if (!s_enabled)
            return;
        std::lock_guard lock(s_mutex);
        fprintf(stderr, "[hle] D3D resolve intents (NFSMW_HLE_RESOLVE): %llu made; resolves matched by their packet %llu, by destination %llu, "
            "again (a tile's packets) %llu, unmatched %llu; intents passed over %llu, dropped %llu; LockRect calls %llu (%zu textures)\n",
            (unsigned long long)s_counts.made, (unsigned long long)s_counts.byPacket, (unsigned long long)s_counts.byDest,
            (unsigned long long)s_counts.again, (unsigned long long)s_counts.unmatched, (unsigned long long)s_counts.skipped, (unsigned long long)s_counts.dropped,
            (unsigned long long)s_counts.locks, s_locked.size());
    }
}
