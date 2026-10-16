// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Parts of a draw's Vulkan state the renderer (gpu/renderer.cpp) can record
// two ways, kept here so the two ways agree. Vulkan-free:
// tests/gpu_draw_state_test.cpp checks it alone.
//
// Xenos's depth control register as Vulkan's depth and stencil tests: baked
// into a pipeline (BuildPipeline), or set per draw with
// VK_EXT_extended_dynamic_state (Draw, CP_OPT_DYNAMIC_STATE), where draws
// that differ only in them share a pipeline: in a race 1 draw in 20 bound
// another pipeline only for them or for the cull mode and front face, and a
// pipeline bind cost RADV about 1 us of command-processor time.
//
// Push constants pushed in part (CP_OPT_KEEP_BINDINGS): only the 16-byte
// rows from the first that changed to the last; nearly every race draw
// changes only the vertex snapshots' rows (DrawConstants::vfRedirect).
#pragma once
#include "xenos/registers.h"

#include <cstdint>
#include <cstring>

namespace gpu::draw_state
{
    // A face's stencil ops and test, in VkStencilOpState's order. The values
    // are Vulkan's: Xenos encodes StencilOp and CompareFunction as
    // VkStencilOp and VkCompareOp do.
    struct StencilFace
    {
        uint32_t failOp, passOp, depthFailOp, compareOp;

        bool operator==(const StencilFace&) const = default;
    };

    struct DepthStencil
    {
        uint32_t depthTest, depthWrite, depthCompareOp, stencilTest;
        StencilFace front, back;

        bool operator==(const DepthStencil&) const = default;
    };

    // RB_DEPTHCONTROL as Vulkan's tests: depth writes only with the depth
    // test on, the back faces' stencil as the front's unless
    // backface_enable. 0 (the key without a depth target, or with one
    // attached for the pass only) tests and writes nothing.
    inline DepthStencil FromDepthControl(uint32_t depthControl)
    {
        xe::gpu::reg::RB_DEPTHCONTROL dc{};
        dc.value = depthControl;
        DepthStencil d{};
        d.depthTest = dc.z_enable;
        d.depthWrite = dc.z_enable && dc.z_write_enable;
        d.depthCompareOp = uint32_t(dc.zfunc);
        d.stencilTest = dc.stencil_enable;
        d.front = { uint32_t(dc.stencilfail), uint32_t(dc.stencilzpass), uint32_t(dc.stencilzfail), uint32_t(dc.stencilfunc) };
        d.back = dc.backface_enable
            ? StencilFace{ uint32_t(dc.stencilfail_bf), uint32_t(dc.stencilzpass_bf), uint32_t(dc.stencilzfail_bf), uint32_t(dc.stencilfunc_bf) }
            : d.front;
        return d;
    }

    // The bytes [lo, hi) of two `size`-byte blocks (a multiple of 16) from
    // the first 16-byte row where they differ to the last; false, with
    // nothing set, when they are the same.
    inline bool ChangedRows(const void* was, const void* now, uint32_t size, uint32_t& lo, uint32_t& hi)
    {
        const uint8_t* a = static_cast<const uint8_t*>(was);
        const uint8_t* b = static_cast<const uint8_t*>(now);
        uint32_t first = 0, end = size;
        while (first < end && memcmp(a + first, b + first, 16) == 0)
            first += 16;
        if (first == end)
            return false;
        while (memcmp(a + end - 16, b + end - 16, 16) == 0)  // (stops at the first changed row)
            end -= 16;
        lo = first;
        hi = end;
        return true;
    }
}
