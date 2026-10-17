// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// study/hle-renderer instrumentation: see d3d_profile.h.
#include <stdafx.h>
#include "d3d_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <x86intrin.h>

namespace d3dprof
{
    const bool g_on = [] { const char* v = std::getenv("D3DPROF"); return v && (v[0] == '1' || v[0] == '2'); }();
    // D3DPROF=2: only the top-level calls' time (excluding waits) and counts,
    // for the library's CPU cost with the least overhead.
    const bool g_light = [] { const char* v = std::getenv("D3DPROF"); return v && v[0] == '2'; }();

    namespace
    {
        constexpr uint32_t kDevGlobal = 0x82A2CF60;
        constexpr uint32_t kSwap = 0x82598670;
        // Waits inside D3D calls (excluded from their time): the GPU hang
        // check D3D polls in its wait loops, and BlockOnFence.
        constexpr uint32_t kWaitPoll = 0x825A5B20, kBlockOnFence = 0x82597498;

        struct Counters
        {
            std::atomic<uint64_t> calls{ 0 }, ext{ 0 }, ringBytes{ 0 }, tsc{ 0 }, afterBytes{ 0 }, afterEvents{ 0 }, waitTsc{ 0 };
        };
        Counters* s_c = nullptr;  // g_count entries
        std::atomic<uint64_t> s_swaps{ 0 }, s_outsideNoPrev{ 0 };
        std::atomic<uint32_t> s_lastPtr{ 0 };
        std::atomic<int64_t> s_lastTop{ -1 };
        std::vector<uint64_t> s_dumps;
        std::mutex s_dumpMutex;
        double s_tscPerNs = 0;
        uint64_t s_traceFrom = 0, s_traceFrames = 1;
        std::atomic<bool> s_tracing{ false };
        FILE* s_traceFile = nullptr;
        uint64_t s_traceSeq = 0;
        uint32_t s_drawsIdx = 0, s_drawsNon = 0, s_drawsUp = 0, s_beginVerts = 0, s_textures = 0, s_shaders = 0;

        thread_local int t_depth = 0;
        thread_local uint32_t t_segStart = 0;
        thread_local uint64_t t_acc = 0, t_tsc0 = 0, t_wait = 0, t_waitStart = 0;
        thread_local int t_waitDepth = 0;

        uint32_t Load32(uint8_t* base, uint32_t addr)
        {
            uint32_t v;
            memcpy(&v, base + addr, 4);
            return __builtin_bswap32(v);
        }

        float LoadF(uint8_t* base, uint32_t addr)
        {
            uint32_t v = Load32(base, addr);
            float f;
            memcpy(&f, &v, 4);
            return f;
        }

        uint32_t Device(uint8_t* base)
        {
            return Load32(base, kDevGlobal);
        }

        uint32_t RingPtr(uint8_t* base)
        {
            uint32_t dev = Device(base);
            return dev ? Load32(base, dev) : 0;
        }

        bool Near(uint32_t from, uint32_t to)
        {
            return to >= from && to - from < 0x200000;
        }

        std::vector<uint64_t> List(const char* v)
        {
            std::vector<uint64_t> out;
            if (!v)
                return out;
            std::string s = v;
            size_t p = 0;
            while (p < s.size())
            {
                size_t q = s.find(',', p);
                if (q == std::string::npos)
                    q = s.size();
                out.push_back(strtoull(s.substr(p, q - p).c_str(), nullptr, 10));
                p = q + 1;
            }
            return out;
        }

        void Init()
        {
            s_c = new Counters[g_count];
            s_dumps = List(std::getenv("D3DPROF_DUMP"));
            auto trace = List(std::getenv("D3DPROF_TRACE"));
            if (!trace.empty())
            {
                s_traceFrom = trace[0];
                s_traceFrames = trace.size() > 1 ? trace[1] : 1;
            }
            auto t0 = std::chrono::steady_clock::now();
            uint64_t c0 = __rdtsc();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint64_t c1 = __rdtsc();
            auto t1 = std::chrono::steady_clock::now();
            s_tscPerNs = double(c1 - c0) / double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        const bool s_init = [] { if (g_on) Init(); return true; }();

        FILE* Out()
        {
            const char* path = std::getenv("D3DPROF_OUT");
            return path ? fopen(path, "a") : stderr;
        }

        void Dump(uint64_t swap)
        {
            std::lock_guard lock(s_dumpMutex);
            FILE* f = Out();
            if (!f)
                return;
            fprintf(f, "# d3dprof swap %llu tscPerNs %.4f outsideNoPrev %llu\n", (unsigned long long)swap, s_tscPerNs,
                (unsigned long long)s_outsideNoPrev.load());
            fprintf(f, "fn,calls,ext,ringBytes,topNs,afterBytes,afterEvents,waitNs\n");
            for (size_t i = 0; i < g_count; i++)
            {
                const Counters& c = s_c[i];
                uint64_t calls = c.calls.load(std::memory_order_relaxed);
                if (!calls)
                    continue;
                fprintf(f, "%08X,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", g_addrs[i], (unsigned long long)calls,
                    (unsigned long long)c.ext.load(), (unsigned long long)c.ringBytes.load(),
                    (unsigned long long)(double(c.tsc.load()) / s_tscPerNs), (unsigned long long)c.afterBytes.load(),
                    (unsigned long long)c.afterEvents.load(), (unsigned long long)(double(c.waitTsc.load()) / s_tscPerNs));
            }
            fprintf(f, "# end\n");
            if (f != stderr)
                fclose(f);
        }

        // Frame trace: the calls that shape passes, with the draws between.
        void Pending()
        {
            if (s_drawsIdx || s_drawsNon || s_drawsUp || s_beginVerts)
            {
                fprintf(s_traceFile, "T %llu   draws: indexed %u, non-indexed %u, UP %u, BeginVertices %u (SetTexture %u, shaders %u)\n",
                    (unsigned long long)s_traceSeq, s_drawsIdx, s_drawsNon, s_drawsUp, s_beginVerts, s_textures, s_shaders);
                s_drawsIdx = s_drawsNon = s_drawsUp = s_beginVerts = s_textures = s_shaders = 0;
            }
        }

        std::string Words(uint8_t* base, uint32_t addr, int n)
        {
            if (!addr)
                return "null";
            if (addr < 0x40000000 || addr >= 0xC0000000)
                return "not a pointer";
            std::string s;
            char b[16];
            for (int k = 0; k < n; k++)
            {
                snprintf(b, sizeof(b), "%s%08X", k ? " " : "", Load32(base, addr + 4 * k));
                s += b;
            }
            return s;
        }

        void TraceEnter(uint32_t addr, PPCContext& ctx, uint8_t* base)
        {
            switch (addr)
            {
            case 0x82593A58: s_drawsIdx++; return;
            case 0x82593818: s_drawsNon++; return;
            case 0x82593390: s_drawsUp++; return;
            case 0x825930E0: s_beginVerts++; return;
            case 0x8258A450: s_textures++; return;
            case 0x8259BBC8: case 0x8259BF58: s_shaders++; return;
            default: break;
            }
            const char* name = nullptr;
            std::string args;
            char b[512];
            switch (addr)
            {
            case 0x8258D920:
                name = "SetRenderTarget";
                snprintf(b, sizeof(b), "index %u surface %08X [%s]", ctx.r4.u32, ctx.r5.u32, Words(base, ctx.r5.u32, 8).c_str());
                args = b;
                break;
            case 0x8258DC88:
                name = "SetDepthStencilSurface";
                snprintf(b, sizeof(b), "surface %08X [%s]", ctx.r4.u32, Words(base, ctx.r4.u32, 8).c_str());
                args = b;
                break;
            case 0x82592340:
                name = "Resolve";
                snprintf(b, sizeof(b), "flags %X rect [%s] dest %08X [%s] point [%s] level %u slice %u clearColor [%s] z %.3f stencil %u params %08X",
                    ctx.r4.u32, Words(base, ctx.r5.u32, 4).c_str(), ctx.r6.u32, Words(base, ctx.r6.u32, 10).c_str(),
                    Words(base, ctx.r7.u32, 2).c_str(), ctx.r8.u32, ctx.r9.u32, Words(base, ctx.r10.u32, 4).c_str(), ctx.f1.f64,
                    Load32(base, ctx.r1.u32 + 92), Load32(base, ctx.r1.u32 + 100));
                args = b;
                break;
            case 0x8259A308:
                name = "ClearF";
                snprintf(b, sizeof(b), "flags %X rect [%s] color [%s] z %.3f stencil %u", ctx.r4.u32, Words(base, ctx.r5.u32, 4).c_str(),
                    Words(base, ctx.r6.u32, 4).c_str(), ctx.f1.f64, ctx.r7.u32);
                args = b;
                break;
            case 0x825990F8:
                name = "BeginTiling";
                snprintf(b, sizeof(b), "flags %X count %u rects [%s] clearColor [%s] z %.3f stencil %u", ctx.r4.u32, ctx.r5.u32,
                    Words(base, ctx.r6.u32, std::min<uint32_t>(ctx.r5.u32, 4) * 4).c_str(), Words(base, ctx.r7.u32, 4).c_str(), ctx.f1.f64,
                    ctx.r8.u32);
                args = b;
                break;
            case 0x82599488:
                name = "EndTiling";
                snprintf(b, sizeof(b), "flags %X rect [%s] dest %08X [%s] clearColor [%s] z %.3f stencil %u params %08X", ctx.r4.u32,
                    Words(base, ctx.r5.u32, 4).c_str(), ctx.r6.u32, Words(base, ctx.r6.u32, 10).c_str(), Words(base, ctx.r7.u32, 4).c_str(),
                    ctx.f1.f64, ctx.r8.u32, ctx.r9.u32);
                args = b;
                break;
            case 0x82598FC8:
                name = "SetPredication";
                snprintf(b, sizeof(b), "mask %08X", ctx.r4.u32);
                args = b;
                break;
            case 0x8258D500:
                name = "SetViewport";
                if (ctx.r4.u32)
                    snprintf(b, sizeof(b), "%u,%u %ux%u z %.2f-%.2f", Load32(base, ctx.r4.u32), Load32(base, ctx.r4.u32 + 4),
                        Load32(base, ctx.r4.u32 + 8), Load32(base, ctx.r4.u32 + 12), LoadF(base, ctx.r4.u32 + 16), LoadF(base, ctx.r4.u32 + 20));
                else
                    snprintf(b, sizeof(b), "null");
                args = b;
                break;
            case kSwap:
                name = "Swap";
                break;
            default:
                return;
            }
            Pending();
            fprintf(s_traceFile, "T %llu %s %s\n", (unsigned long long)++s_traceSeq, name, args.c_str());
        }

        void TraceLeave(uint32_t addr, PPCContext& ctx, uint8_t* base)
        {
            if (addr != 0x8258D920 && addr != 0x8258DC88)
                return;
            uint32_t dev = Device(base);
            fprintf(s_traceFile, "T %llu   -> RB_SURFACE_INFO %08X RB_COLOR_INFO %08X RB_DEPTH_INFO %08X RB_COLOR1_INFO %08X RB_COLOR_MASK %08X\n",
                (unsigned long long)s_traceSeq, Load32(base, dev + 11456), Load32(base, dev + 11460), Load32(base, dev + 11464),
                Load32(base, dev + 11468), Load32(base, dev + 11548));
        }
    }

    size_t IndexOf(uint32_t addr)
    {
        const uint32_t* end = g_addrs + g_count;
        const uint32_t* it = std::lower_bound(g_addrs, end, addr);
        return it != end && *it == addr ? size_t(it - g_addrs) : SIZE_MAX;
    }

    void Enter(size_t i, PPCContext& ctx, uint8_t* base)
    {
        uint32_t addr = g_addrs[i];
        if (g_light)
        {
            if (addr == kWaitPoll || addr == kBlockOnFence)
            {
                if (t_waitDepth++ == 0)
                    t_waitStart = __rdtsc();
            }
            if (t_depth++ == 0)
            {
                t_wait = 0;
                t_tsc0 = __rdtsc();
            }
            return;
        }
        s_c[i].calls.fetch_add(1, std::memory_order_relaxed);
        if (addr == kWaitPoll || addr == kBlockOnFence)
        {
            if (t_waitDepth++ == 0)
                t_waitStart = __rdtsc();
        }
        if (t_depth++ != 0)
            return;
        if (s_tracing.load(std::memory_order_relaxed))
            TraceEnter(addr, ctx, base);
        s_c[i].ext.fetch_add(1, std::memory_order_relaxed);
        uint32_t p = RingPtr(base);
        uint32_t last = s_lastPtr.load(std::memory_order_relaxed);
        if (last && p != last && Near(last, p))
        {
            int64_t prev = s_lastTop.load(std::memory_order_relaxed);
            if (prev >= 0)
            {
                s_c[prev].afterBytes.fetch_add(p - last, std::memory_order_relaxed);
                s_c[prev].afterEvents.fetch_add(1, std::memory_order_relaxed);
            }
            else
                s_outsideNoPrev.fetch_add(p - last, std::memory_order_relaxed);
        }
        t_segStart = p;
        t_acc = 0;
        t_wait = 0;
        t_tsc0 = __rdtsc();
    }

    void Leave(size_t i, PPCContext& ctx, uint8_t* base)
    {
        uint32_t addr = g_addrs[i];
        if (g_light)
        {
            if (addr == kWaitPoll || addr == kBlockOnFence)
            {
                if (--t_waitDepth == 0)
                    t_wait += __rdtsc() - t_waitStart;
            }
            if (--t_depth != 0)
                return;
            uint64_t tsc = __rdtsc() - t_tsc0;
            s_c[i].ext.fetch_add(1, std::memory_order_relaxed);
            s_c[i].calls.fetch_add(1, std::memory_order_relaxed);
            s_c[i].tsc.fetch_add(tsc - std::min(tsc, t_wait), std::memory_order_relaxed);
            s_c[i].waitTsc.fetch_add(std::min(tsc, t_wait), std::memory_order_relaxed);
            if (addr == kSwap)
            {
                uint64_t n = s_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
                if (std::find(s_dumps.begin(), s_dumps.end(), n) != s_dumps.end())
                    Dump(n);
            }
            return;
        }
        if (addr == kWaitPoll || addr == kBlockOnFence)
        {
            if (--t_waitDepth == 0)
                t_wait += __rdtsc() - t_waitStart;
        }
        if (--t_depth != 0)
            return;
        uint64_t tsc = __rdtsc() - t_tsc0;
        uint32_t p = RingPtr(base);
        if (t_segStart && Near(t_segStart, p))
            t_acc += p - t_segStart;
        s_c[i].ringBytes.fetch_add(t_acc, std::memory_order_relaxed);
        s_c[i].tsc.fetch_add(tsc - std::min(tsc, t_wait), std::memory_order_relaxed);
        s_c[i].waitTsc.fetch_add(std::min(tsc, t_wait), std::memory_order_relaxed);
        s_lastPtr.store(p, std::memory_order_relaxed);
        s_lastTop.store(int64_t(i), std::memory_order_relaxed);
        if (s_tracing.load(std::memory_order_relaxed))
            TraceLeave(addr, ctx, base);
        if (addr == kSwap)
        {
            uint64_t n = s_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
            if (std::find(s_dumps.begin(), s_dumps.end(), n) != s_dumps.end())
                Dump(n);
            if (s_traceFrom)
            {
                if (n == s_traceFrom)
                {
                    s_traceFile = Out();
                    fprintf(s_traceFile, "# trace from swap %llu, %llu frames\n", (unsigned long long)n, (unsigned long long)s_traceFrames);
                    s_tracing = true;
                }
                else if (n == s_traceFrom + s_traceFrames && s_tracing)
                {
                    s_tracing = false;
                    Pending();
                    fprintf(s_traceFile, "# end trace\n");
                    if (s_traceFile != stderr)
                        fclose(s_traceFile);
                }
            }
        }
    }

    void MakeSpaceBefore(uint8_t* base)
    {
        uint32_t p = RingPtr(base);
        if (t_segStart && Near(t_segStart, p))
            t_acc += p - t_segStart;
    }

    void MakeSpaceAfter(uint8_t* base)
    {
        t_segStart = RingPtr(base);
    }
}
