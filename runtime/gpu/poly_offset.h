// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Polygon offset (depth bias): which of the guest's two offsets a draw gets,
// and in what steps the host sets it. Vulkan-free: tests/gpu_poly_offset_test.cpp
// checks it alone; gpu/renderer.cpp's Draw sets its pipeline's depth bias
// enable and its vkCmdSetDepthBias(2EXT) from it.
//
// Xenos has an offset per face (PA_SU_POLY_OFFSET_FRONT_SCALE and _OFFSET
// for front faces, _BACK_ for back faces), each switched on by its own bit
// of PA_SU_SC_MODE_CNTL; poly_offset_para_enable switches the front one on
// for points and lines. Vulkan has one per draw, and biases only polygons.
// Up to v0.1.4 the renderer took the front registers whenever the front or
// the para bit was set: a pass that culls front faces and offsets the back
// ones (a common way to draw shadow casters) got the front's values, or no
// offset at all when only the back's bit was set (a candidate for GitHub
// issue #13's shadow artifacts, with the steps below). Now as Xenia does
// (draw_util.cc, GetPreferredFacePolygonOffset): the front face's offset
// when it is on and front faces aren't culled, else the back face's when it
// is on and back faces aren't culled. With both faces drawn and both offset,
// both get the front's (the faces usually drawn; shadow volumes aside).
//
// The steps: the registers hold depth units, and the constant factor is
// that times 2^24, unorm24's steps (D24S8's, the console's). The host's
// depth is float32 for both guest formats, where Vulkan's step is 2^(e - 23)
// for e the exponent of the primitive's largest depth: right for depths
// 0.5..1, half the offset below 0.5, a quarter below 0.25. A D24S8 target's
// bias is set in unorm24's steps where the device can
// (VK_EXT_depth_bias_control, FORCE_UNORM: a constant 2^-24 a step over the
// whole range, as on the console). A D24FS8 target's (20e4 float) stays in
// the float steps: 20e4's own precision falls with the exponent the same
// way, three bits coarser (Xenia's factor for it on a float32 host is
// 2^21 x 8, the same 2^24), so it was already right.
#pragma once
#include "xenos/xenos.h"

#include <cstdint>

namespace gpu
{
    // What the choice reads: the draw's primitive kind and PA_SU_SC_MODE_CNTL's
    // bits, and the four offset registers.
    struct PolyOffsetRegs
    {
        bool polygonal = true;  // triangles, quads or rectangles (not points or lines)
        bool frontEnable = false, backEnable = false;  // poly_offset_front_enable, _back_enable
        bool cullFront = false, cullBack = false;
        float frontScale = 0.0f, frontOffset = 0.0f;  // PA_SU_POLY_OFFSET_FRONT_SCALE, _OFFSET
        float backScale = 0.0f, backOffset = 0.0f;    // PA_SU_POLY_OFFSET_BACK_SCALE, _OFFSET
    };

    // The draw's offset, in the registers' units (the scale per 1/16
    // subpixel, the offset in depth). `enable`: the pipeline's
    // depthBiasEnable (a face's offset is on, even when its values are 0).
    struct PolyOffset
    {
        bool enable = false;
        float scale = 0.0f, offset = 0.0f;
    };

    // The face's offset (above): the front's if on and drawn, else the
    // back's if on and drawn (also when the front's is on with both values
    // 0). Points and lines get none (Vulkan biases only polygons, so their
    // para bit is not looked at).
    constexpr PolyOffset PreferredPolyOffset(const PolyOffsetRegs& r)
    {
        PolyOffset o;
        if (!r.polygonal)
            return o;
        if (r.frontEnable && !r.cullFront)
            o = { true, r.frontScale, r.frontOffset };
        if (r.backEnable && !r.cullBack && o.scale == 0.0f && o.offset == 0.0f)
            o = { true, r.backScale, r.backOffset };
        return o;
    }

    // Unorm24's steps in a unit of depth: the constant factor is the
    // register's offset times this.
    inline constexpr float kUnorm24Steps = float(1u << 24);

    // Whether the draw's bias is set in unorm24's steps
    // (VK_EXT_depth_bias_control's FORCE_UNORM representation) rather than
    // in the float32 depth's own: biased, into a D24S8 target, on a device
    // that can (`forceUnorm`).
    constexpr bool BiasInUnorm24Steps(bool enable, bool depthTarget, xe::gpu::xenos::DepthRenderTargetFormat format, bool forceUnorm)
    {
        return enable && depthTarget && forceUnorm && format == xe::gpu::xenos::DepthRenderTargetFormat::kD24S8;
    }
}
