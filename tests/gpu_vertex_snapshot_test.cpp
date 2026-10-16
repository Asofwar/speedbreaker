// Tests for the vertex snapshots' spans (gpu/vertex_snapshot.h): a draw
// copies only the bytes of a vertex fetch constant's range its fetches can
// read, where every fetch through it indexes with the vertex index. Up to
// v0.1.4 every range of 64 KB or less was copied whole for every draw that
// used it: 12-24 MB a frame in a race on the Steam Deck. Checked here: that
// a span covers every address a shader's fetch reads inside the range (the
// shader's addressing redone in C++, over random fetches, ranges, indices
// and offsets), and the translator's note of which fetches index with the
// vertex index, on hand-assembled Xenos shaders. From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/gpu_vertex_snapshot_test.cpp runtime/gpu/shader_translator.cpp
//     runtime/gpu/xenos/ucode.cpp -o build/gpu_vertex_snapshot_test
//   build/gpu_vertex_snapshot_test runtime/gpu/renderer.cpp runtime/gpu/shader_translator.cpp
// With the two paths (ctest passes them), Draw is also checked to copy the
// spans, and the shaders' vertex addressing to be the one modelled here.
#include <gpu/shader_translator.h>
#include <gpu/vertex_snapshot.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using gpu::vsnap::FetchUse;
using gpu::vsnap::IndexRange;
using gpu::vsnap::Span;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static FetchUse Use(bool indexed, std::initializer_list<std::pair<uint32_t, int32_t>> fetches)
{
    FetchUse u;
    for (auto [stride, offset] : fetches)
        u.Add(indexed, stride, offset);
    return u;
}

// ---------------------------------------------------------------------
// The spans.

static void TestSpans()
{
    const FetchUse plain = Use(true, { { 8, 0 }, { 8, 3 }, { 8, 5 } });  // 32-byte vertices, fetches at dwords 0, 3, 5
    // Vertices 10..20: from vertex 10's first dword to vertex 20's dword 5
    // plus a 4-dword fetch.
    Span s = gpu::vsnap::Needed(plain, { true, 10, 20 }, 65536);
    CHECK(s.start == 10 * 32 && s.end == 20 * 32 + 5 * 4 + 16, "vertices 10..20: %u..%u", s.start, s.end);
    // Clamped to the range.
    s = gpu::vsnap::Needed(plain, { true, 10, 20 }, 400);
    CHECK(s.start == 320 && s.end == 400, "clamped: %u..%u", s.start, s.end);
    // Entirely past the range: nothing (those fetches read guest memory).
    s = gpu::vsnap::Needed(plain, { true, 100, 120 }, 400);
    CHECK(s.Size() == 0, "past the range: %u..%u", s.start, s.end);
    // No vertex runs (every index a restart).
    s = gpu::vsnap::Needed(plain, { true, 1, 0 }, 400);
    CHECK(s.Size() == 0, "no vertices: %u..%u", s.start, s.end);
    // A negative offset reaches before the element, clamped at the range's start.
    s = gpu::vsnap::Needed(Use(true, { { 4, -2 } }), { true, 0, 3 }, 1024);
    CHECK(s.start == 0 && s.end == (3 * 4 - 2) * 4 + 16, "negative offset: %u..%u", s.start, s.end);
    // Stride 0: every vertex reads the same element.
    s = gpu::vsnap::Needed(Use(true, { { 0, 6 } }), { true, 0, 5000 }, 1024);
    CHECK(s.start == 24 && s.end == 40, "stride 0: %u..%u", s.start, s.end);
    // The whole range: a fetch with another index, an index range not
    // scanned, one so far out that an address could wrap around memory.
    s = gpu::vsnap::Needed(Use(false, { { 8, 0 } }), { true, 10, 20 }, 4096);
    CHECK(s.start == 0 && s.end == 4096, "not vertex-indexed: %u..%u", s.start, s.end);
    s = gpu::vsnap::Needed(plain, {}, 4096);
    CHECK(s.start == 0 && s.end == 4096, "indices unknown: %u..%u", s.start, s.end);
    s = gpu::vsnap::Needed(Use(true, { { 255, 4194303 } }), { true, 0, 300000 }, 4096);
    CHECK(s.start == 0 && s.end == 4096, "near the wrap: %u..%u", s.start, s.end);
    // The lowest offset (-2^22 dwords) reaches 16 MB before the range:
    // those fetches read below it, none wraps into it.
    s = gpu::vsnap::Needed(Use(true, { { 8, -4194304 } }), { true, 0, 10 }, 4096);
    CHECK(s.Size() == 0, "far below: %u..%u", s.start, s.end);
    // Merged uses span both.
    FetchUse a = Use(true, { { 8, 0 } }), b = Use(true, { { 4, 100 } });
    gpu::vsnap::Merge(a, b);
    s = gpu::vsnap::Needed(a, { true, 2, 3 }, 65536);
    CHECK(s.start <= 2 * 8 * 4 && s.start <= (2 * 4 + 100) * 4 && s.end >= 3 * 8 * 4 + 16 && s.end >= (3 * 4 + 100) * 4 + 16,
        "merged: %u..%u", s.start, s.end);
    FetchUse c = Use(true, { { 8, 0 } });
    gpu::vsnap::Merge(c, Use(false, { { 8, 0 } }));
    CHECK(!c.vertexIndexed, "a merged use with another index isn't vertex-indexed");

    // The index offset, in 32 bits (a negative one wraps back), then
    // exact as a float: past that, unknown.
    IndexRange r = gpu::vsnap::WithOffset({ true, 10, 20 }, 5);
    CHECK(r.known && r.lo == 15 && r.hi == 25, "offset 5: %u..%u", r.lo, r.hi);
    r = gpu::vsnap::WithOffset({ true, 10, 20 }, uint32_t(-10));
    CHECK(r.known && r.lo == 0 && r.hi == 10, "offset -10: %u..%u", r.lo, r.hi);
    r = gpu::vsnap::WithOffset({ true, 10, 20 }, uint32_t(-11));
    CHECK(!r.known, "offset -11 wraps an index around");
    r = gpu::vsnap::WithOffset({ true, 0, gpu::vsnap::kExactIndices - 1 }, 0);
    CHECK(r.known, "the highest exact index");
    r = gpu::vsnap::WithOffset({ true, 0, gpu::vsnap::kExactIndices - 1 }, 1);
    CHECK(!r.known, "one past it");
    r = gpu::vsnap::WithOffset({ true, 1, 0 }, 1000);
    CHECK(r.known && r.lo > r.hi, "no vertices stay none");
    r = gpu::vsnap::WithOffset({}, 0);
    CHECK(!r.known, "unknown stays unknown");
}

// The shader's addressing (shader_translator.cpp: runVertex's r0.x, the
// fetch's vbase, vmem's word address), for one fetch of the vertex with
// index buffer value `index`.
static uint32_t FetchAddress(uint32_t vf0, uint32_t index, uint32_t indexOffset, bool rounded, uint32_t stride, int32_t offset, uint32_t word)
{
    float x = float(uint32_t(index + indexOffset));
    int ivindex = int(rounded ? std::floor(x + 0.5f) : std::floor(x));
    uint32_t vbase = (vf0 & 0xFFFFFFFCu) + uint32_t(ivindex * int(stride) + offset) * 4u;
    return (vbase + word * 4) & 0x1FFFFFFFu;
}

// Every address a fetch reads inside the range is inside the span (read
// from the snapshot as before): vmem's test, (address - start) < size.
static void TestCoverage()
{
    std::mt19937 rng(20261015);
    auto pick = [&](uint32_t lo, uint32_t hi) { return std::uniform_int_distribution<uint32_t>(lo, hi)(rng); };
    uint64_t checked = 0, inside = 0, spans = 0, partial = 0;
    for (int iteration = 0; iteration < 20000 && g_failures < 20; iteration++)
    {
        // The fetch constant: anywhere in guest memory (some with the
        // upper mirror bits set), up to 64 KB, clamped at its end as Draw does.
        uint32_t address = pick(0, 7) == 0 ? (1u << 29) - pick(1, 16384) * 4 : pick(0, (1u << 27) - 1) * 4;
        uint32_t vf0 = (pick(0, 3) == 0 ? 0xA0000000u : 0) | address;
        uint32_t base = vf0 & 0x1FFFFFFCu;
        uint32_t size = std::min<uint32_t>(pick(1, 16384) * 4, (1u << 29) - base);
        // One to three fetches through it (the merged use bounds them all).
        int fetchCount = int(pick(1, 3));
        std::vector<std::pair<uint32_t, int32_t>> fetches;
        FetchUse use;
        for (int f = 0; f < fetchCount; f++)
        {
            uint32_t stride = pick(0, 9) == 0 ? pick(0, 255) : pick(1, 16);
            int32_t offset = pick(0, 9) == 0 ? int32_t(pick(0, (1u << 23) - 1)) - (1 << 22) : int32_t(pick(0, 40)) - 4;
            fetches.push_back({ stride, offset });
            use.Add(true, stride, offset);
        }
        bool rounded = pick(0, 1) == 1;
        // The indices: small, or out to the exact limit; the offset small,
        // negative, or large.
        uint32_t lo = pick(0, 3) == 0 ? pick(0, gpu::vsnap::kExactIndices + 1000) : pick(0, 2000);
        uint32_t hi = lo + (pick(0, 3) == 0 ? pick(0, 60000) : pick(0, 40));
        uint32_t indexOffset = pick(0, 2) == 0 ? 0u : pick(0, 1) ? pick(0, 3000) : uint32_t(-int32_t(pick(0, 3000)));
        const IndexRange shader = gpu::vsnap::WithOffset({ true, lo, hi }, indexOffset);
        const Span span = gpu::vsnap::Needed(use, shader, size);
        CHECK(span.start <= span.end && span.end <= size, "span %u..%u of %u bytes", span.start, span.end, size);
        spans++;
        partial += span.Size() < size;
        if (!shader.known)
            continue;  // (the whole range then: nothing to check, and the float no longer exact)
        // Every vertex for short ranges, else the ends and a sample.
        std::vector<uint32_t> indices;
        if (hi - lo <= 64)
            for (uint32_t i = lo; i <= hi; i++)
                indices.push_back(i);
        else
        {
            indices = { lo, lo + 1, hi - 1, hi };
            for (int k = 0; k < 32; k++)
                indices.push_back(pick(lo, hi));
        }
        for (uint32_t index : indices)
            for (auto [stride, offset] : fetches)
                for (uint32_t word = 0; word < 4; word++)
                {
                    uint32_t a = FetchAddress(vf0, index, indexOffset, rounded, stride, offset, word);
                    checked++;
                    if (a - base >= size)
                        continue;  // outside the range: guest memory, as before
                    inside++;
                    CHECK(a - (base + span.start) < span.Size(),
                        "index %u (+%d) stride %u offset %d word %u: %08X inside %08X+%u but outside the span %u..%u", index,
                        int32_t(indexOffset), stride, offset, word, a, base, size, span.start, span.end);
                }
    }
    printf("coverage: %llu spans (%llu narrower than their range), %llu fetch words, %llu of them inside their range\n",
        (unsigned long long)spans, (unsigned long long)partial, (unsigned long long)checked, (unsigned long long)inside);
    CHECK(partial > spans / 4 && inside > checked / 20, "the cases exercise narrowed spans and in-range fetches");
}

// ---------------------------------------------------------------------
// The translator's note: hand-assembled shaders (encodings: gpu/xenos/ucode.h).

struct Cf
{
    uint32_t w0 = 0, w1 = 0;  // w1: 16 bits
    int exec = -1;            // an exec's first instruction (patched into its address)
};

// exec/exec_end over `count` instructions from `first`; `fetches`: bit k
// set when instruction k is a fetch.
static Cf Exec(int first, uint32_t count, uint32_t fetches, bool end)
{
    uint32_t sequence = 0;
    for (uint32_t k = 0; k < count; k++)
        sequence |= ((fetches >> k) & 1) << (2 * k);
    Cf c;
    c.w0 = (count << 12) | (sequence << 16);
    c.w1 = (end ? 2u : 1u) << 12;
    c.exec = first;
    return c;
}
static Cf LoopStart(uint32_t skipTo) { return { skipTo, 7u << 12, -1 }; }  // loop constant 0
static Cf LoopEnd(uint32_t bodyAt) { return { bodyAt, 8u << 12, -1 }; }

using Ins = std::array<uint32_t, 3>;

struct VFetch
{
    uint32_t constant = 0;   // 0..95
    uint32_t src = 0, srcComponent = 0;
    bool srcRelative = false;
    uint32_t dest = 1, destSwizzle = 0x688;  // xyzw
    uint32_t stride = 8;
    int32_t offset = 0;
    uint32_t format = 57;    // k_32_32_32_FLOAT
    bool mini = false;
};

static Ins Encode(const VFetch& f)
{
    Ins i{};
    i[0] = 0u | (f.src << 5) | (uint32_t(f.srcRelative) << 11) | (f.dest << 12) | (1u << 19) | ((f.constant / 3) << 20) |
        ((f.constant % 3) << 25) | (f.srcComponent << 30);
    i[1] = f.destSwizzle | (1u << 13) | (f.format << 16) | (uint32_t(f.mini) << 30);
    i[2] = f.stride | ((uint32_t(f.offset) & 0x7FFFFF) << 8);
    return i;
}

// tfetch2D into `dest` (texture constant 0, coordinates from r1).
static Ins TFetch(uint32_t dest, bool destRelative, uint32_t destSwizzle)
{
    Ins i{};
    i[0] = 1u | (1u << 5) | (dest << 12) | (uint32_t(destRelative) << 18) | (0x24u << 26);
    i[1] = destSwizzle;
    i[2] = 1u << 14;  // 2D
    return i;
}

// A vector max of r1 with itself (a move) into `dest`, and the scalar
// unit's maxs into `scalarDest`; write masks 4 bits, export to oN.
static Ins Alu(uint32_t dest, bool destRelative, uint32_t vectorMask, uint32_t scalarDest = 0, bool scalarRelative = false,
    uint32_t scalarMask = 0, bool exportData = false)
{
    Ins i{};
    uint32_t scalarOp = scalarMask ? 5u : 50u;  // maxs, else retain_prev (no scalar work)
    i[0] = dest | (uint32_t(destRelative) << 6) | (scalarDest << 8) | (uint32_t(scalarRelative) << 14) | (uint32_t(exportData) << 15) |
        (vectorMask << 16) | (scalarMask << 20) | (scalarOp << 26);
    i[1] = 0;
    i[2] = 1u | (1u << 8) | (1u << 16) | (2u << 24) | (1u << 29) | (1u << 30) | (1u << 31);  // max(r1, r1); r1 as the scalar operand too
    return i;
}

static std::vector<uint32_t> Assemble(std::vector<Cf> cf, const std::vector<Ins>& ins)
{
    if (cf.size() & 1)
        cf.push_back({});  // a nop
    const uint32_t firstAddress = uint32_t(cf.size() / 2);
    std::vector<uint32_t> w;
    for (size_t k = 0; k < cf.size(); k += 2)
    {
        Cf a = cf[k], b = cf[k + 1];
        if (a.exec >= 0)
            a.w0 |= firstAddress + uint32_t(a.exec);
        if (b.exec >= 0)
            b.w0 |= firstAddress + uint32_t(b.exec);
        w.push_back(a.w0);
        w.push_back((a.w1 & 0xFFFF) | (b.w0 << 16));
        w.push_back((b.w0 >> 16) | (b.w1 << 16));
    }
    for (const Ins& i : ins)
        w.insert(w.end(), i.begin(), i.end());
    return w;
}

// One flat exec_end over the instructions (`fetches` as for Exec).
static gpu::TranslatedShader Flat(const std::vector<Ins>& ins, uint32_t fetches, gpu::ShaderStage stage = gpu::ShaderStage::Vertex)
{
    std::vector<uint32_t> w = Assemble({ Exec(0, uint32_t(ins.size()), fetches, true) }, ins);
    return gpu::TranslateShader(stage, w.data(), w.size());
}

static const FetchUse* Find(const gpu::TranslatedShader& t, uint32_t constant)
{
    for (const FetchUse& u : t.vertexFetches)
        if (u.constant == constant)
            return &u;
    return nullptr;
}

static void ExpectUse(const char* what, const gpu::TranslatedShader& t, uint32_t constant, bool indexed, uint32_t minStride = 0,
    uint32_t maxStride = 0, int32_t minOffset = 0, int32_t maxOffset = 0)
{
    CHECK(t.ok, "%s: translation failed: %s", what, t.error.c_str());
    const FetchUse* u = Find(t, constant);
    CHECK(u, "%s: no use of constant %u", what, constant);
    if (!u)
        return;
    CHECK(u->vertexIndexed == indexed, "%s: constant %u vertex-indexed %d, want %d", what, constant, u->vertexIndexed, indexed);
    if (indexed)
        CHECK(u->minStride == minStride && u->maxStride == maxStride && u->minOffset == minOffset && u->maxOffset == maxOffset,
            "%s: strides %u..%u offsets %d..%d, want %u..%u %d..%d", what, u->minStride, u->maxStride, u->minOffset, u->maxOffset, minStride,
            maxStride, minOffset, maxOffset);
}

static void TestTranslator()
{
    // The usual vertex declaration: a full fetch by r0.x and minis.
    {
        VFetch full{ .constant = 0, .stride = 9, .offset = 0 };
        VFetch mini1{ .constant = 0, .dest = 2, .offset = 3, .format = 37, .mini = true };
        VFetch mini2{ .constant = 0, .dest = 3, .offset = 5, .format = 6, .mini = true };
        auto t = Flat({ Encode(full), Encode(mini1), Encode(mini2) }, 0b111);
        ExpectUse("full and minis", t, 0, true, 9, 9, 0, 5);
        CHECK(t.vertexFetches.size() == 1, "one constant, %zu uses", t.vertexFetches.size());
    }
    // Two constants, listed in order; 2 streams by r0.x.
    {
        auto t = Flat({ Encode({ .constant = 5, .stride = 4 }), Encode({ .constant = 2, .dest = 2, .stride = 3, .offset = 1 }) }, 0b11);
        CHECK(t.vertexFetches.size() == 2 && t.vertexFetches[0].constant == 2 && t.vertexFetches[1].constant == 5, "in constant order");
        ExpectUse("stream 2", t, 2, true, 3, 3, 1, 1);
        ExpectUse("stream 5", t, 5, true, 4, 4, 0, 0);
        uint32_t bits = t.vertexFetchConstants[0];
        CHECK(bits == ((1u << 2) | (1u << 5)), "the mask agrees (%08X)", bits);
    }
    // r0.x written before the fetch: by the vector unit, the scalar unit,
    // a relative write, a texture fetch, an earlier vertex fetch into r0.
    ExpectUse("vector write to r0.x first", Flat({ Alu(0, false, 0b0001), Encode({}) }, 0b10), 0, false);
    ExpectUse("scalar write to r0.x first", Flat({ Alu(4, false, 0, 0, false, 0b0001), Encode({}) }, 0b10), 0, false);
    ExpectUse("relative write first", Flat({ Alu(7, true, 0b0001), Encode({}) }, 0b10), 0, false);
    ExpectUse("texture fetch into r0.x first", Flat({ TFetch(0, false, 0x688), Encode({}) }, 0b11), 0, false);
    ExpectUse("full fetch into r0, then a mini", Flat({ Encode({ .constant = 0, .dest = 0 }), Encode({ .constant = 0, .dest = 2, .offset = 3, .mini = true }) }, 0b11), 0,
        false);
    // ... while other components, other registers, exports and writes
    // after the fetch change nothing.
    ExpectUse("vector write to r0.yzw first", Flat({ Alu(0, false, 0b1110), Encode({}) }, 0b10), 0, true, 8, 8, 0, 0);
    ExpectUse("relative write to .y first", Flat({ Alu(7, true, 0b0010), Encode({}) }, 0b10), 0, true, 8, 8, 0, 0);
    ExpectUse("write to r1.x first", Flat({ Alu(1, false, 0b0001), Encode({}) }, 0b10), 0, true, 8, 8, 0, 0);
    ExpectUse("export first", Flat({ Alu(62, false, 0b1111, 0, false, 0, true), Encode({}) }, 0b10), 0, true, 8, 8, 0, 0);
    ExpectUse("texture fetch into r0.y first", Flat({ TFetch(0, false, 0xFCF), Encode({}) }, 0b11), 0, true, 8, 8, 0, 0);
    ExpectUse("write to r0.x after", Flat({ Encode({}), Alu(0, false, 0b0001) }, 0b01), 0, true, 8, 8, 0, 0);
    {
        // The fetch before the write keeps the vertex index, the one after doesn't.
        auto t = Flat({ Encode({ .constant = 0 }), Alu(0, false, 0b0001), Encode({ .constant = 1, .dest = 2 }) }, 0b101);
        ExpectUse("before the write", t, 0, true, 8, 8, 0, 0);
        ExpectUse("after the write", t, 1, false);
    }
    // Other indices: another component, register, a relative source; a
    // pixel shader's fetch.
    ExpectUse("indexed by r0.y", Flat({ Encode({ .srcComponent = 1 }) }, 0b1), 0, false);
    ExpectUse("indexed by r1.x", Flat({ Encode({ .src = 1 }) }, 0b1), 0, false);
    ExpectUse("relative index", Flat({ Encode({ .srcRelative = true }) }, 0b1), 0, false);
    ExpectUse("pixel shader", Flat({ Encode({}) }, 0b1, gpu::ShaderStage::Pixel), 0, false);
    {
        // One vertex-indexed fetch and one not, through the same constant.
        auto t = Flat({ Encode({ .constant = 3 }), Encode({ .constant = 3, .src = 2, .dest = 2 }) }, 0b11);
        ExpectUse("mixed fetches", t, 3, false);
    }
    // Loops: an instruction emitted later can run first. A write to r0.x
    // anywhere spoils every fetch; a program without one keeps them.
    {
        std::vector<Cf> cf = { LoopStart(3), Exec(0, 1, 0b1, false), LoopEnd(1), Exec(1, 1, 0b0, true) };
        std::vector<uint32_t> w = Assemble(cf, { Encode({}), Alu(0, false, 0b0001) });
        auto t = gpu::TranslateShader(gpu::ShaderStage::Vertex, w.data(), w.size());
        ExpectUse("loop, r0.x written after it", t, 0, false);
        w = Assemble(cf, { Encode({}), Alu(0, false, 0b0010) });
        t = gpu::TranslateShader(gpu::ShaderStage::Vertex, w.data(), w.size());
        ExpectUse("loop, r0.y written after it", t, 0, true, 8, 8, 0, 0);
    }
}

// ---------------------------------------------------------------------
// The sources.

// `path`'s code, comments left out, without white space.
static std::string Squashed(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "cannot read %s", path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string code, squashed;
    std::istringstream in(ss.str());
    for (std::string line; std::getline(in, line);)
    {
        size_t c = line.find("//");
        code += (c == std::string::npos ? line : line.substr(0, c)) + "\n";
    }
    for (char ch : code)
        if (ch != ' ' && ch != '\n' && ch != '\t' && ch != '\r')
            squashed += ch;
    return squashed;
}

static void TestSources(const char* rendererPath, const char* translatorPath)
{
    const std::string renderer = Squashed(rendererPath);
    size_t draw = renderer.find("voidDraw(uint32_tinitiatorValue");
    CHECK(draw != std::string::npos, "Draw not found in %s", rendererPath);
    if (draw == std::string::npos)
        return;
    const std::string body = renderer.substr(draw, renderer.find("voidSubmit()", draw) - draw);
    CHECK(renderer.find("vsnap::IndexRangeConvertIndices(") != std::string::npos, "ConvertIndices returns the indices' range");
    CHECK(body.find("for(constvsnap::FetchUse&use:shader->info.vertexFetches)") != std::string::npos, "the snapshots follow the shaders' fetch uses");
    CHECK(body.find("vsnap::Merge(same->use,r.use);") != std::string::npos, "a range read twice is copied once, for both");
    CHECK(body.find("vertices=ConvertIndices(") != std::string::npos, "the converted indices' range is kept");
    CHECK(body.find("vertices=vsnap::WithOffset(vertices,s_regs[XE_GPU_REG_VGT_INDX_OFFSET]);") != std::string::npos,
        "with the index offset the shader adds");
    const size_t needed = body.find("constvsnap::Spanspan=vsnap::Needed(copies[i].use,vertices,copies[i].size);");
    const size_t copy = body.find("memcpy(s_ringHost+snapshot,s_sharedHost+copies[i].base+start,size);");
    const size_t redirect = body.find("pc.vfRedirect[i][0]=copies[i].base+start;pc.vfRedirect[i][1]=size;");
    CHECK(needed != std::string::npos && copy != std::string::npos && redirect != std::string::npos && needed < copy && copy < redirect,
        "each copy is its span, and the shader is redirected to just that");

    // The addressing modelled in FetchAddress, and the redirect test.
    const std::string translator = Squashed(translatorPath);
    CHECK(translator.find("r[0].x=float(fetchIndex(logicalIndex)+pc.indexOffset);") != std::string::npos, "r0.x is the vertex index");
    CHECK(translator.find("intivindex=int(floor(vindex+0.5));") != std::string::npos &&
            translator.find("intivindex=int(floor(vindex));") != std::string::npos,
        "the index rounds or floors");
    CHECK(translator.find("uintvbase=(vf0&0xFFFFFFFCu)+uint(ivindex*{}+({}))*4u;") != std::string::npos, "the element's address");
    CHECK(translator.find("gpuSwap(vmem(vbase+{}u),vendian)") != std::string::npos, "words from there");
    CHECK(translator.find("uinta=byteAddress&0x1FFFFFFFu;") != std::string::npos &&
            translator.find("uintd=a-pc.vfRedirect[r].x;if(d<pc.vfRedirect[r].y)returng_ring[(pc.vfRedirect[r].z+d)>>2];") !=
                std::string::npos,
        "a redirect serves the words inside it");
}

int main(int argc, char** argv)
{
    TestSpans();
    TestCoverage();
    TestTranslator();
    if (argc > 2)
        TestSources(argv[1], argv[2]);
    else
        printf("(no renderer.cpp and shader_translator.cpp given: their checks skipped)\n");
    if (g_failures)
    {
        printf("gpu_vertex_snapshot_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_vertex_snapshot_test: all passed\n");
    return 0;
}
