// Tests for the draw state the renderer records two ways (gpu/draw_state.h):
// Xenos's depth control register as Vulkan's depth and stencil tests, which
// a pipeline bakes in or a draw sets with VK_EXT_extended_dynamic_state
// (CP_OPT_DYNAMIC_STATE), and the push-constant rows a draw changes, which
// CP_OPT_KEEP_BINDINGS pushes alone. From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/gpu_draw_state_test.cpp -o build/gpu_draw_state_test
//   build/gpu_draw_state_test runtime/gpu/renderer.cpp
// With the path (ctest passes it), renderer.cpp is also checked to take both
// ways of recording the tests from FromDepthControl, and to push the changed
// rows.
#include <gpu/draw_state.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using gpu::draw_state::ChangedRows;
using gpu::draw_state::DepthStencil;
using gpu::draw_state::FromDepthControl;
using gpu::draw_state::StencilFace;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// RB_DEPTHCONTROL's fields (xenos/registers.h), for building values here.
static uint32_t DepthControl(uint32_t stencil, uint32_t z, uint32_t zWrite, uint32_t zFunc, uint32_t backface = 0, uint32_t sFunc = 0,
    uint32_t sFail = 0, uint32_t sPass = 0, uint32_t sDepthFail = 0, uint32_t bFunc = 0, uint32_t bFail = 0, uint32_t bPass = 0,
    uint32_t bDepthFail = 0)
{
    return stencil | z << 1 | zWrite << 2 | zFunc << 4 | backface << 7 | sFunc << 8 | sFail << 11 | sPass << 14 | sDepthFail << 17 |
        bFunc << 20 | bFail << 23 | bPass << 26 | bDepthFail << 29;
}

static void TestDepthStencil()
{
    // Nothing: no depth target, or one attached for the pass only.
    const DepthStencil none = FromDepthControl(0);
    CHECK(none == DepthStencil{}, "0: test %u write %u stencil %u", none.depthTest, none.depthWrite, none.stencilTest);

    // The scene: depth test and write, less-or-equal (VK_COMPARE_OP_LESS_OR_EQUAL = 3).
    const DepthStencil scene = FromDepthControl(DepthControl(0, 1, 1, 3));
    CHECK(scene.depthTest == 1 && scene.depthWrite == 1 && scene.depthCompareOp == 3 && scene.stencilTest == 0,
        "scene: test %u write %u op %u stencil %u", scene.depthTest, scene.depthWrite, scene.depthCompareOp, scene.stencilTest);

    // The transparent pass: tested, not written.
    const DepthStencil glass = FromDepthControl(DepthControl(0, 1, 0, 3));
    CHECK(glass.depthTest == 1 && glass.depthWrite == 0, "glass: test %u write %u", glass.depthTest, glass.depthWrite);

    // A write without the test writes nothing (Vulkan neither).
    const DepthStencil writeOnly = FromDepthControl(DepthControl(0, 0, 1, 7));
    CHECK(writeOnly.depthTest == 0 && writeOnly.depthWrite == 0, "write without test: test %u write %u", writeOnly.depthTest,
        writeOnly.depthWrite);

    // Stencil, one-sided: the back faces get the front's ops. Always pass,
    // replace on pass (VK_COMPARE_OP_ALWAYS = 7, VK_STENCIL_OP_REPLACE = 2).
    const DepthStencil oneSided = FromDepthControl(DepthControl(1, 1, 0, 3, 0, 7, 0, 2, 0, 3, 1, 1, 1));
    const StencilFace replace{ 0, 2, 0, 7 };
    CHECK(oneSided.stencilTest == 1 && oneSided.front == replace && oneSided.back == replace,
        "one-sided: front %u/%u/%u/%u back %u/%u/%u/%u", oneSided.front.failOp, oneSided.front.passOp, oneSided.front.depthFailOp,
        oneSided.front.compareOp, oneSided.back.failOp, oneSided.back.passOp, oneSided.back.depthFailOp, oneSided.back.compareOp);

    // Two-sided (backface_enable): the back faces' own, every field apart
    // (fail, pass, depth fail, func; VkStencilOp's increment/decrement wrap
    // are 6 and 7, VK_COMPARE_OP_NOT_EQUAL is 5).
    const DepthStencil twoSided = FromDepthControl(DepthControl(1, 1, 1, 1, 1, 5, 1, 6, 7, 2, 4, 3, 5));
    CHECK(twoSided.front == (StencilFace{ 1, 6, 7, 5 }), "two-sided front %u/%u/%u/%u", twoSided.front.failOp, twoSided.front.passOp,
        twoSided.front.depthFailOp, twoSided.front.compareOp);
    CHECK(twoSided.back == (StencilFace{ 4, 3, 5, 2 }), "two-sided back %u/%u/%u/%u", twoSided.back.failOp, twoSided.back.passOp,
        twoSided.back.depthFailOp, twoSided.back.compareOp);
    CHECK(twoSided.depthCompareOp == 1, "two-sided depth op %u", twoSided.depthCompareOp);
}

static void TestChangedRows()
{
    uint8_t was[112] = {}, now[112] = {};
    uint32_t lo = 999, hi = 999;
    CHECK(!ChangedRows(was, now, 112, lo, hi) && lo == 999 && hi == 999, "same: %u..%u", lo, hi);

    // One byte in row 3: that row alone.
    now[3 * 16 + 5] = 1;
    CHECK(ChangedRows(was, now, 112, lo, hi) && lo == 48 && hi == 64, "row 3: %u..%u", lo, hi);

    // Rows 1 and 5: from the first to the last, the rows between included.
    now[3 * 16 + 5] = 0;
    now[16] = 1;
    now[5 * 16 + 15] = 1;
    CHECK(ChangedRows(was, now, 112, lo, hi) && lo == 16 && hi == 96, "rows 1 and 5: %u..%u", lo, hi);

    // The first and the last byte: everything.
    memset(now, 0, sizeof(now));
    now[0] = 1;
    now[111] = 1;
    CHECK(ChangedRows(was, now, 112, lo, hi) && lo == 0 && hi == 112, "first and last: %u..%u", lo, hi);

    // The last row only (a race draw's vertex snapshot row).
    memset(now, 0, sizeof(now));
    now[100] = 7;
    CHECK(ChangedRows(was, now, 112, lo, hi) && lo == 96 && hi == 112, "last row: %u..%u", lo, hi);
}

// renderer.cpp records the tests both ways from FromDepthControl and pushes
// the changed rows.
static void TestRenderer(const char* path)
{
    std::ifstream file(path);
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();
    CHECK(!src.empty(), "%s unreadable", path);
    if (src.empty())
        return;
    auto count = [&](const std::string& what) {
        size_t n = 0;
        for (size_t at = src.find(what); at != std::string::npos; at = src.find(what, at + 1))
            n++;
        return n;
    };
    // BuildPipeline (baked) and Draw (dynamic).
    CHECK(count("draw_state::FromDepthControl(") >= 2, "FromDepthControl used %zu times", count("draw_state::FromDepthControl("));
    CHECK(count("draw_state::ChangedRows(") >= 1, "ChangedRows unused");
    // The dynamic states the pipelines leave out are the ones Draw sets.
    for (const char* state : { "VK_DYNAMIC_STATE_CULL_MODE_EXT", "VK_DYNAMIC_STATE_FRONT_FACE_EXT", "VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT",
             "VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT", "VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT", "VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT",
             "VK_DYNAMIC_STATE_STENCIL_OP_EXT" })
        CHECK(count(state) == 1, "%s named %zu times", state, count(state));
    for (const char* call : { "cmdSetCullMode(", "cmdSetFrontFace(", "cmdSetDepthTestEnable(", "cmdSetDepthWriteEnable(",
             "cmdSetDepthCompareOp(", "cmdSetStencilTestEnable(", "cmdSetStencilOp(" })
        CHECK(count(std::string("s_vk->") + call) >= 1, "Draw never calls %s", call);
}

int main(int argc, char** argv)
{
    TestDepthStencil();
    TestChangedRows();
    if (argc > 1)
        TestRenderer(argv[1]);
    if (g_failures)
    {
        printf("gpu_draw_state_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_draw_state_test: all passed\n");
    return 0;
}
