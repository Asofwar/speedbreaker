// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// A timedemo replay's exposure: the scene's brightness from the frame
// before, in every run.
//
// The game's exposure follows the scene's brightness (sub_822234F0, on its
// render thread, once a frame): it draws the frame down to 64x64, resolves
// that into one of two textures (they take turns, [0x82A2C4A8] says which),
// and locks the other one, the previous frame's, to average a channel of its
// 4096 pixels on the CPU (sub_82223308) into the brightness the exposure
// adapts to. It locks with D3DLOCK_READONLY (0x10), and with that D3D's lock
// (sub_82594D40) skips its wait for the texture's fence: on the console the
// GPU is done with the previous frame by then, and a read-only lock has no
// reason to stall. Here the GPU side can be frames behind (uncapped, the
// game runs up to four ahead), and the command processor writes a resolve
// into guest memory only once it gets to it: the average was of the previous
// frame's picture or of one from two (or more) frames before, as the command
// processor's lag had it. So two runs of one replay adapted their exposure to
// different frames and drew every picture after a little brighter or darker
// (more so uncapped, where the lag varies most: 37 to 91 dB PSNR between
// uncapped runs, at the vblank's pace a level or two).
//
// In a replay that takes pictures (NFSMW_BENCH_SHOTS), that one lock (called
// from 0x822236E8) waits first for the texture's fence, as a lock without the
// flag does (D3D's own sub_82597410, which picks the fence, and BlockOnFence,
// sub_82597498, r4 the fence's address; the fence is the resource's word at
// +4, which D3D's lock reads too): the average is of the previous frame in
// every run, capped or not. The render thread then waits for the GPU to
// finish the previous frame's brightness pass where it used to run on
// (counted as a game wait, like D3D's other ones). That costs an uncapped
// replay on a Steam Deck about 1.5% of its frame rate (121.4 against 123.2
// fps, p99 13.5 against 12.6 ms, one build), and a capped one a few late
// frames (0 to 6 in a run, up to 29 ms, on the Deck and headless; none
// without it): a replay that takes no pictures, whose times are what it
// measures, doesn't wait, as in play.
// NFSMW_TIMEDEMO_EXPOSURE_WAIT=1 or 0 makes any replay wait or not. Capture
// and normal play are untouched: the exposure is drawing only, nothing the
// game's simulation or its Joylog sees.
#include <stdafx.h>
#include <debug/d3d_profile.h>  // study/hle-renderer
#include "timedemo.h"

#include <kernel/function.h>

extern "C" PPC_FUNC(__imp__sub_82589398);  // D3D: LockRect of a texture's level
extern "C" PPC_FUNC(__imp__sub_82597410);  // D3D: the fence a lock of a resource waits for
PPC_FUNC(sub_82597498);  // D3D: BlockOnFence (through debug/timeline.cpp's hook: a game wait)

namespace
{
    constexpr uint32_t kExposureLock = 0x822236E8;  // sub_822234F0's lock of the previous frame's 64x64

    // NFSMW_TIMEDEMO_EXPOSURE_WAIT=1 or 0, else whether the run takes
    // pictures (NFSMW_BENCH_SHOTS).
    const bool s_enabled = [] {
        if (const char* v = std::getenv("NFSMW_TIMEDEMO_EXPOSURE_WAIT"))
            return v[0] != '0';
        const char* shots = std::getenv("NFSMW_BENCH_SHOTS");
        return shots && shots[0];
    }();
}

PPC_FUNC(sub_82589398)
{
    d3dprof::Scope d3dprofScope(0x82589398, ctx, base);  // study/hle-renderer
    if (uint32_t(ctx.lr) == kExposureLock && timedemo::Replaying())
    {
        static std::atomic<bool> s_said{ false };
        if (!s_said.exchange(true, std::memory_order_relaxed))
            fprintf(stderr, s_enabled
                    ? "[timedemo] exposure: the brightness the exposure adapts to waits for its frame on the GPU "
                      "(NFSMW_TIMEDEMO_EXPOSURE_WAIT=0: it doesn't)\n"
                    : "[timedemo] exposure: the brightness as the GPU side has it, no wait: the run takes no pictures "
                      "(NFSMW_TIMEDEMO_EXPOSURE_WAIT=1: it waits)\n");
    }
    if (s_enabled && uint32_t(ctx.lr) == kExposureLock && timedemo::Replaying())
    {
        // The resource the lock's fence is that of (as sub_82589398 picks
        // it: a level whose type bits are 4 stands for its parent at +44).
        uint32_t texture = ctx.r3.u32, resource = texture;
        if ((PPC_LOAD_U32(texture) & 0x00070000) == 0x00040000 && PPC_LOAD_U32(texture + 44))
            resource = PPC_LOAD_U32(texture + 44);
        if (uint32_t fence = PPC_LOAD_U32(resource + 4))
        {
            const PPCContext saved = ctx;
            // A word below the caller's stack for the fence, as D3D's lock
            // keeps it in its own frame; the calls' frames go below it.
            const uint32_t sp = (ctx.r1.u32 - 256) & ~15u;
            PPC_STORE_U32(sp + 80, fence);
            const uint32_t device = PPC_LOAD_U32(PPC_LOAD_U32(0x82000B48));  // (KeGetCurrentProcessType() is 1: the title's)
            ctx.r1.u64 = sp;
            ctx.r3.u64 = device;
            ctx.r4.u64 = sp + 80;
            ctx.r5.u64 = resource;
            __imp__sub_82597410(ctx, base);
            if (PPC_LOAD_U32(sp + 80))
            {
                ctx.r3.u64 = device;
                ctx.r4.u64 = sp + 80;
                sub_82597498(ctx, base);
            }
            ctx = saved;
        }
    }
    __imp__sub_82589398(ctx, base);
}
