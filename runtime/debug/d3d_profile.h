// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// study/hle-renderer instrumentation (never merged): counts the game's calls
// into Direct3D (the recompiled XDK 2.0.2135 D3D library, guest
// 0x82587000-0x825B0000) and attributes the command buffer's bytes to the
// D3D call that wrote them.
//   D3DPROF=1                 turn it on (off: the wrappers only test a flag)
//   D3DPROF=2                 only time the top-level calls (less overhead;
//                             no ring bytes, no per-call counts of nested calls)
//   D3DPROF_DUMP=a,b,...      dump the counters when the guest's swap count
//                             (calls of sub_82598670) reaches each value
//   D3DPROF_TRACE=<swap>[,n]  log the render target, resolve, clear, tiling
//                             and viewport calls of n frames (1) from that
//                             swap, with the draws between them
//   D3DPROF_OUT=<file>        where (default stderr)
// A top-level call is one made from outside the library (the thread's
// first wrapper on its stack). Its ring bytes are how far the device's
// command buffer pointer (device+0; the device is the global at
// 0x82A2CF60) moved during it, across MakeSpace's segment switches
// (sub_82597070). Bytes the pointer moved between two top-level calls were
// written by game code (or a pointer the game restored itself), and are
// charged to the call before them ("after").
#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>

struct PPCContext;

namespace d3dprof
{
    extern const uint32_t g_addrs[];
    extern const size_t g_count;
    extern const bool g_on;
    void Enter(size_t i, PPCContext& ctx, uint8_t* base);
    void Leave(size_t i, PPCContext& ctx, uint8_t* base);
    void MakeSpaceBefore(uint8_t* base);
    void MakeSpaceAfter(uint8_t* base);
    size_t IndexOf(uint32_t addr);

    // For hooks defined elsewhere (they wrap their own call to __imp__).
    struct Scope
    {
        size_t i;
        PPCContext& ctx;
        uint8_t* base;
        Scope(uint32_t addr, PPCContext& c, uint8_t* b) : i(g_on ? IndexOf(addr) : SIZE_MAX), ctx(c), base(b)
        {
            if (i != SIZE_MAX)
                Enter(i, ctx, base);
        }
        ~Scope()
        {
            if (i != SIZE_MAX)
                Leave(i, ctx, base);
        }
    };
}

#define D3DPROF_WRAP(index, name)                                    \
    extern "C" PPC_FUNC(__imp__##name);                              \
    PPC_FUNC(name)                                                   \
    {                                                                \
        if (!d3dprof::g_on)                                          \
        {                                                            \
            __imp__##name(ctx, base);                                \
            return;                                                  \
        }                                                            \
        d3dprof::Enter(index, ctx, base);                            \
        __imp__##name(ctx, base);                                    \
        d3dprof::Leave(index, ctx, base);                            \
    }

#define D3DPROF_MAKESPACE(index, name)                               \
    extern "C" PPC_FUNC(__imp__##name);                              \
    PPC_FUNC(name)                                                   \
    {                                                                \
        if (!d3dprof::g_on)                                          \
        {                                                            \
            __imp__##name(ctx, base);                                \
            return;                                                  \
        }                                                            \
        d3dprof::Enter(index, ctx, base);                            \
        d3dprof::MakeSpaceBefore(base);                              \
        __imp__##name(ctx, base);                                    \
        d3dprof::MakeSpaceAfter(base);                               \
        d3dprof::Leave(index, ctx, base);                            \
    }

// A wrapper that also calls the HLE side channel's hooks (gpu/hle_resolve.h:
// `enter` before the function, `leave` after it), counted or not.
#define D3DPROF_WRAP_HLE(index, name, enter, leave)                  \
    extern "C" PPC_FUNC(__imp__##name);                              \
    PPC_FUNC(name)                                                   \
    {                                                                \
        enter(ctx, base);                                            \
        if (!d3dprof::g_on)                                          \
            __imp__##name(ctx, base);                                \
        else                                                         \
        {                                                            \
            d3dprof::Enter(index, ctx, base);                        \
            __imp__##name(ctx, base);                                \
            d3dprof::Leave(index, ctx, base);                        \
        }                                                            \
        leave(ctx, base);                                            \
    }
