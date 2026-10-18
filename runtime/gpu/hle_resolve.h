// study/hle-renderer, phase 1 of the hybrid plan (docs/hle-renderer-study.md,
// 5.2): what Direct3D meant by a resolve, from hooks on its functions, handed
// to the command processor beside the command stream.
//
// NFSMW_HLE_RESOLVE=1 (off by default) turns the side channel on. The guest's
// Resolve (sub_82592340, also the ones EndTiling sub_82599488 makes) records
// an intent: its flags (which surface, which clears), its source rectangle
// (none: the whole surface), its destination texture (the fetch constant of
// its header: base, size, format, type), level, slice and destination point,
// and where in the command buffer its packets went (the device's write
// pointer, device+0, before and after the call). LockRect (sub_82589398)
// marks the textures the CPU reads. The command processor's resolve takes the
// intent whose packets hold the draw that triggers it (renderer::g_drawPacket),
// or, should none, the first with that destination; intents passed over
// (predicated tiles the renderer skips) are dropped.
//
// With it off: one test of a constant in each hook.
#pragma once
#include <cstdint>

struct PPCContext;

namespace hle
{
    struct Intent
    {
        uint64_t seq = 0;
        uint32_t ringStart = 0, ringEnd = 0;  // guest physical, [start, end); end 0 while the call runs
        uint32_t flags = 0;                   // D3DRESOLVE_*: 0-3 render target, 4 depth, 0x100 clear colour, 0x200 clear depth/stencil
        bool wholeSurface = false;            // pSrcRect null
        int32_t rect[4] = {};                 // pSrcRect (x0, y0, x1, y1)
        bool point = false;                   // pDestPoint given
        int32_t pointX = 0, pointY = 0;
        uint32_t texHeader = 0;               // pDestTex (0: none)
        uint32_t texBase = 0, texBytes = 0;   // its memory (physical), from the fetch constant
        uint32_t texFormat = 0, texWidth = 0, texHeight = 0, texDimension = 0, texPitch = 0;
        uint32_t level = 0, slice = 0;
        bool inTiling = false;                // made by EndTiling
        bool cpuLocked = false;               // the CPU has locked this texture (LockRect) in the last 2 s
        bool clearColour() const { return (flags & 0x100) != 0; }
        bool clearDepth() const { return (flags & 0x200) != 0; }
        bool depth() const { return (flags & 0xF) == 4; }
    };

    // NFSMW_HLE_RESOLVE=1.
    bool Enabled();

    // Guest threads: the hooks (debug/d3d_profile_wrappers.cpp, game/exposure_lock.cpp).
    void ResolveEnter(PPCContext& ctx, uint8_t* base);
    void ResolveLeave(PPCContext& ctx, uint8_t* base);
    void EndTilingEnter(PPCContext& ctx, uint8_t* base);
    void EndTilingLeave(PPCContext& ctx, uint8_t* base);
    void LockRect(PPCContext& ctx, uint8_t* base);

    // Command processor thread: the intent of the resolve the draw at
    // `packet` (guest physical) triggers, its copy destination `destBase`
    // (RB_COPY_DEST_BASE). False: none (`out` untouched).
    bool Match(uint32_t packet, uint32_t destBase, Intent& out);

    // Counts, at exit ([hle] lines).
    void Report();
}
