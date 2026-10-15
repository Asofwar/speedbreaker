// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Texture formats as the renderer (gpu/renderer.cpp, GetTextureFormat and
// TextureView) samples them, apart from their Vulkan formats: which guest
// formats hold another's data, and how the components a host format lacks
// are filled. Values are xenos::TextureFormat's (xenos/xenos.h); Xenia's
// texture cache (vulkan_texture_cache.cc, kBestHostFormats) is the
// reference for both. Vulkan-free: tests/gpu_texture_formats_test.cpp
// checks it alone.
//
// Up to v0.1.4 a format the renderer had no entry for was sampled as a
// white 1x1 placeholder (overbright materials). Among them were the
// *_AS_16_16_16_16 formats (50-56), which hold exactly their base format's
// data (the name is about the precision the texture unit filters them at),
// and k_8_A (8), which Xenia samples as k_8.
#pragma once
#include <cstdint>

namespace gpu::texformat
{
    // The format whose data, layout and sampling `format` shares (itself
    // for most).
    constexpr uint32_t SampledAs(uint32_t format)
    {
        switch (format)
        {
        case 8: return 2;    // k_8_A: k_8 (Xenia's kLoadShaderIndex8bpb, R8)
        case 50: return 6;   // k_8_8_8_8_AS_16_16_16_16: k_8_8_8_8
        case 51: return 18;  // k_DXT1_AS_16_16_16_16: k_DXT1
        case 52: return 19;  // k_DXT2_3_AS_16_16_16_16: k_DXT2_3
        case 53: return 20;  // k_DXT4_5_AS_16_16_16_16: k_DXT4_5
        case 54: return 7;   // k_2_10_10_10_AS_16_16_16_16: k_2_10_10_10
        case 55: return 16;  // k_10_11_11_AS_16_16_16_16: k_10_11_11
        case 56: return 17;  // k_11_11_10_AS_16_16_16_16: k_11_11_10
        default: return format;
        }
    }

    // Host swizzles, 3 bits a component (x, y, z, w): the host component
    // (0-3: R, G, B, A) each of the guest's four reads.
    constexpr uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
    {
        return x | (y << 3) | (z << 6) | (w << 9);
    }
    constexpr uint32_t kRGBA = Swizzle(0, 1, 2, 3);
    constexpr uint32_t kRRRR = Swizzle(0, 0, 0, 0);
    constexpr uint32_t kRGGG = Swizzle(0, 1, 1, 1);
    constexpr uint32_t kRGBB = Swizzle(0, 1, 2, 2);
    constexpr uint32_t kAAAA = Swizzle(3, 3, 3, 3);

    // The texture unit fills a format's missing components with its last
    // one (single-component formats repeat their only one) before the fetch
    // constant's swizzle (xenos/xenos.h, at k_8_A): a host format's own
    // fill (0 for green and blue, 1 for alpha) is never what the guest
    // reads. `sampledAs`: SampledAs(format). As Xenia's host swizzles, but
    // k_10_11_11 and k_11_11_10, whose loads write blue into alpha
    // themselves, and k_DXT3A, loaded as the alpha of BC2 blocks.
    constexpr uint32_t HostSwizzle(uint32_t sampledAs)
    {
        switch (sampledAs)
        {
        case 2:   // k_8
        case 22:  // k_24_8 (depth, as R32F)
        case 23:  // k_24_8_FLOAT
        case 24:  // k_16
        case 30:  // k_16_FLOAT
        case 36:  // k_32_FLOAT
        case 59:  // k_DXT5A (BC4)
            return kRRRR;
        case 10:  // k_8_8
        case 25:  // k_16_16
        case 31:  // k_16_16_FLOAT
        case 37:  // k_32_32_FLOAT
        case 49:  // k_DXN (BC5)
            return kRGGG;
        case 4:  // k_5_6_5
            return kRGBB;
        case 58:  // k_DXT3A (BC2's alpha)
            return kAAAA;
        default:
            return kRGBA;
        }
    }

    // The fetch constant's swizzle (3 bits a component: 0-3 X-W, 4 zero,
    // 5 one, 6-7 the host component as it is) through a host swizzle: the
    // swizzle of the host image's view.
    constexpr uint32_t Compose(uint32_t guest, uint32_t host)
    {
        uint32_t out = 0;
        for (uint32_t c = 0; c < 4; c++)
        {
            uint32_t s = (guest >> (3 * c)) & 7;
            if (s < 4)
                s = (host >> (3 * s)) & 7;
            out |= s << (3 * c);
        }
        return out;
    }
}
