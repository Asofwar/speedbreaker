// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Vertex data snapshots: which bytes of a vertex fetch constant's range a
// draw can read. Vulkan-free: tests/gpu_vertex_snapshot_test.cpp checks it
// alone; gpu/shader_translator.cpp notes each shader's fetches (FetchUse)
// and gpu/renderer.cpp's Draw copies only the bytes Needed() names.
//
// The guest's fences are published when the command processor reaches
// them (renderer::g_eagerWrites), so the game may reuse vertex memory
// before the GPU has run the draws that read it. A draw therefore copies
// its small vertex ranges (64 KB or less) into the upload ring as it is
// recorded, and its shaders read the copy (vmem() in shader_translator.cpp).
// Copied whole, those ranges were 12-24 MB a frame in a race on the Steam
// Deck (some 4,000 draws): most of the command processor's copying, which
// took a fifth of its time, and the command processor limits the frame
// rate there. Spans copy about a third less (most draws read most of their
// range): 118.0 -> 120.0 fps, command processor 7.95 -> 7.77 ms a frame.
//
// A fetch reads up to 4 dwords from element `index * stride + offset`
// (dwords) of the constant's data. When the index is the vertex's own index
// (r0.x as the vertex shader starts, written by nothing that runs before
// the fetch: FetchUse::vertexIndexed), the draw reads only the elements of
// the vertices its indices name, from the lowest index's element to the
// end of the highest's. That index is r0.x = float(index + VGT_INDX_OFFSET)
// (the shader's runVertex), exact only while it stays well below 2^24, and
// the fetch's address wraps around guest memory (2^29 bytes): outside the
// bounds below, and for any other index (computed, a pixel shader's, an
// index range not scanned), the whole range is copied as before.
//
// A fetch outside the constant's own range reads guest memory (vmem's
// fallback), as it did before: only reads inside it are the snapshot's,
// and every one of those lies in the span.
#pragma once
#include <algorithm>
#include <cstdint>

namespace gpu::vsnap
{
    // r0.x stays an exact integer through float and floor(x + 0.5) below
    // 2^23; indices this far below it, offset included, are spanned.
    constexpr uint32_t kExactIndices = 1u << 22;
    // A span's ends stay this close to the range (bytes): no fetch address
    // can wrap around guest memory into it from beyond them.
    constexpr int64_t kNoWrap = int64_t(1) << 28;

    // One shader's vertex fetches through one fetch constant (0..95): the
    // strides and offsets they address with (dwords), and whether every
    // one of them indexes with the vertex index.
    struct FetchUse
    {
        uint32_t constant = 0;
        bool vertexIndexed = true;
        uint32_t minStride = UINT32_MAX, maxStride = 0;
        int32_t minOffset = INT32_MAX, maxOffset = INT32_MIN;

        void Add(bool indexed, uint32_t stride, int32_t offset)
        {
            vertexIndexed = vertexIndexed && indexed;
            minStride = std::min(minStride, stride);
            maxStride = std::max(maxStride, stride);
            minOffset = std::min(minOffset, offset);
            maxOffset = std::max(maxOffset, offset);
        }
    };

    // Two uses of one range (both shaders' fetches, or two constants with
    // the same range): every element either can read. (Indices are never
    // negative, so the lowest stride and offset bound every fetch from
    // below, the highest from above.)
    inline void Merge(FetchUse& into, const FetchUse& use)
    {
        into.vertexIndexed = into.vertexIndexed && use.vertexIndexed;
        into.minStride = std::min(into.minStride, use.minStride);
        into.maxStride = std::max(into.maxStride, use.maxStride);
        into.minOffset = std::min(into.minOffset, use.minOffset);
        into.maxOffset = std::max(into.maxOffset, use.maxOffset);
    }

    // The vertex indices a draw runs its vertex shader for, inclusive, as
    // its index buffer holds them (restarts left out). `known` false: not
    // scanned. lo > hi: none (every index a restart).
    struct IndexRange
    {
        bool known = false;
        uint32_t lo = 0, hi = 0;
    };

    // As the shader sees them: + VGT_INDX_OFFSET in 32 bits (a negative
    // offset wraps back), then a float. Unknown when not exact.
    inline IndexRange WithOffset(IndexRange r, uint32_t indexOffset)
    {
        if (!r.known || r.lo > r.hi)
            return r;
        int64_t lo = int64_t(r.lo) + int32_t(indexOffset), hi = int64_t(r.hi) + int32_t(indexOffset);
        if (lo < 0 || hi >= int64_t(kExactIndices))
            return {};
        return { true, uint32_t(lo), uint32_t(hi) };
    }

    // Bytes [start, end) of the constant's range (`size` bytes).
    struct Span
    {
        uint32_t start = 0, end = 0;
        uint32_t Size() const { return end > start ? end - start : 0; }
    };

    // What the fetches of `use` can read of the range for the vertex
    // indices `r` (WithOffset's): the whole range unless both are known.
    inline Span Needed(const FetchUse& use, IndexRange r, uint32_t size)
    {
        const Span whole{ 0, size };
        if (!use.vertexIndexed || !r.known || use.minStride > use.maxStride)  // (no fetch noted)
            return whole;
        if (r.lo > r.hi)
            return {};  // no vertex runs
        // The lowest index's first dword, the highest's last (4 dwords
        // from its element: the largest fetch).
        int64_t from = (int64_t(r.lo) * use.minStride + use.minOffset) * 4;
        int64_t to = (int64_t(r.hi) * use.maxStride + use.maxOffset) * 4 + 16;
        if (from <= -kNoWrap || to >= kNoWrap)
            return whole;
        from = std::clamp<int64_t>(from, 0, size);
        to = std::clamp<int64_t>(to, 0, size);
        if (from >= to)
            return {};  // every fetch lands outside the range
        return { uint32_t(from), uint32_t(to) };
    }
}
