// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The textures the CPU locks (D3D's LockRect, sub_82589398: game/exposure_lock.cpp
// calls Note), for the renderer's deferred resolves (NFSMW_RESOLVE_LAZY,
// NFSMW_RESOLVE_PRESENT, NFSMW_RESOLVE_RENAME): a resolve into a texture the
// CPU locked lately keeps writing its guest bytes at once, since the CPU
// reads them every frame (the exposure's 64x64 brightness) and a deferred
// write would only make it wait. Texture memory locked once while loading
// counts no more after kLockedFor (render targets reuse it later).
#pragma once
#include <cstdint>

namespace cpulocks
{
    // A guest thread: the texture whose header is at `texture` was locked.
    void Note(uint8_t* base, uint32_t texture);

    // The command processor thread: whether the texture at physical
    // `address` (its base) was locked in the last kLockedFor.
    bool LockedLately(uint32_t address);
}
