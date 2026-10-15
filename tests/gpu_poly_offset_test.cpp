// Tests for gpu/poly_offset.h: which of Xenos's two polygon offsets (front
// and back faces) a draw gets, and when its bias is set in unorm24's steps.
// Up to v0.1.4 the renderer took the front registers whenever the front or
// the para bit was on: a pass culling front faces and offsetting the back
// ones got the front's values, or none when only the back's bit was on. And
// on the float32 host depth the constant bias fell with the depth (half of
// it below 0.5), where the console's unorm24 depth keeps it. From the repo
// root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/gpu_poly_offset_test.cpp -o build/gpu_poly_offset_test
//   build/gpu_poly_offset_test runtime/gpu/renderer.cpp
// With renderer.cpp's path (ctest passes it), Draw is also checked to take
// its pipeline's depth bias enable and its bias values from the choice, and
// the steps from BiasInUnorm24Steps.
#include <gpu/poly_offset.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using xe::gpu::xenos::DepthRenderTargetFormat;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Distinct values, so a case can tell which face's registers it got.
static gpu::PolyOffsetRegs Regs(bool front, bool back, bool cullFront, bool cullBack)
{
    gpu::PolyOffsetRegs r;
    r.frontEnable = front;
    r.backEnable = back;
    r.cullFront = cullFront;
    r.cullBack = cullBack;
    r.frontScale = 1.0f;
    r.frontOffset = 1e-5f;
    r.backScale = 2.0f;
    r.backOffset = 3e-5f;
    return r;
}

enum class Want { None, Front, Back };

static void Case(const char* what, const gpu::PolyOffsetRegs& r, Want want)
{
    const gpu::PolyOffset o = gpu::PreferredPolyOffset(r);
    const Want got = !o.enable ? Want::None : o.scale == r.frontScale && o.offset == r.frontOffset ? Want::Front
        : o.scale == r.backScale && o.offset == r.backOffset ? Want::Back : Want::None;
    auto name = [](Want w) { return w == Want::Front ? "front" : w == Want::Back ? "back" : "none"; };
    CHECK(got == want && (o.enable || (o.scale == 0.0f && o.offset == 0.0f)), "%s: %s (enable %d, scale %g, offset %g), want %s", what, name(got),
        o.enable, o.scale, o.offset, name(want));
}

static void TestChoice()
{
    // Every combination of the two enables and the two culls, both faces'
    // registers non-zero: the front's when it is on and drawn, else the
    // back's when it is on and drawn.
    for (int bits = 0; bits < 16; bits++)
    {
        const bool front = bits & 1, back = bits & 2, cullFront = bits & 4, cullBack = bits & 8;
        const Want want = front && !cullFront ? Want::Front : back && !cullBack ? Want::Back : Want::None;
        char what[96];
        snprintf(what, sizeof(what), "front %d back %d cull front %d cull back %d", front, back, cullFront, cullBack);
        Case(what, Regs(front, back, cullFront, cullBack), want);
    }
    // The cases the old rule (front registers on the front or para bit) got
    // wrong: front faces culled, the back faces offset.
    Case("shadow casters: front culled, back offset", Regs(false, true, true, false), Want::Back);
    Case("front culled, both offset", Regs(true, true, true, false), Want::Back);
    Case("front culled, only the front's on", Regs(true, false, true, false), Want::None);
    // Both drawn and both on: the front's (Vulkan has one per draw).
    Case("both drawn, both on", Regs(true, true, false, false), Want::Front);
    // The front's on with both values 0: the back's, if on and drawn.
    gpu::PolyOffsetRegs zero = Regs(true, true, false, false);
    zero.frontScale = zero.frontOffset = 0.0f;
    Case("front's values 0", zero, Want::Back);
    zero.backEnable = false;
    const gpu::PolyOffset z = gpu::PreferredPolyOffset(zero);
    CHECK(z.enable && z.scale == 0.0f && z.offset == 0.0f, "front's on with 0 and no back's: enabled, 0 (enable %d)", z.enable);
    // Only a slope, or only a constant, still counts as set.
    gpu::PolyOffsetRegs slope = Regs(true, true, false, false);
    slope.frontOffset = 0.0f;
    const gpu::PolyOffset s = gpu::PreferredPolyOffset(slope);
    CHECK(s.enable && s.scale == slope.frontScale && s.offset == 0.0f, "a front slope alone: the front's (scale %g offset %g)", s.scale, s.offset);
    // Points and lines: none (Vulkan biases only polygons).
    gpu::PolyOffsetRegs lines = Regs(true, true, false, false);
    lines.polygonal = false;
    Case("a line list", lines, Want::None);
    static_assert(gpu::PreferredPolyOffset({ .frontEnable = true, .frontScale = 1.0f }).enable, "usable in constant expressions");
    printf("face choice: 16 combinations and the edge cases checked\n");
}

static void TestSteps()
{
    // Only a biased draw into a D24S8 target on a device that can.
    CHECK(gpu::BiasInUnorm24Steps(true, true, DepthRenderTargetFormat::kD24S8, true), "D24S8, biased, FORCE_UNORM: unorm24 steps");
    CHECK(!gpu::BiasInUnorm24Steps(true, true, DepthRenderTargetFormat::kD24FS8, true), "D24FS8 keeps the float steps");
    CHECK(!gpu::BiasInUnorm24Steps(true, true, DepthRenderTargetFormat::kD24S8, false), "no FORCE_UNORM: the float steps, as before");
    CHECK(!gpu::BiasInUnorm24Steps(false, true, DepthRenderTargetFormat::kD24S8, true), "not biased: nothing to set");
    CHECK(!gpu::BiasInUnorm24Steps(true, false, DepthRenderTargetFormat::kD24S8, true), "no depth target");

    // What the constant factor gives, by Vulkan's rule (the spec's Depth Bias
    // Computation): on a float32 depth buffer a step is 2^(e - 23), e the
    // exponent of the primitive's largest depth (2^e <= z < 2^(e+1));
    // FORCE_UNORM makes it 2^-24 (mantissa bits plus one) at every depth.
    // The register's offset (depth units) is what unorm24 adds anywhere.
    const float offset = 3.0f / 16777216.0f;  // three unorm24 steps
    const double constant = double(offset) * gpu::kUnorm24Steps;
    CHECK(constant == 3.0, "the constant factor: %g unorm24 steps, want 3", constant);
    auto floatSteps = [](double z) {
        int e = 0;
        std::frexp(z, &e);  // z = m 2^e, m in [0.5, 1): z's exponent is e - 1
        return std::ldexp(1.0, (e - 1) - 23);
    };
    const double unormStep = std::ldexp(1.0, -24);
    for (double z : { 0.99, 0.75, 0.5, 0.49, 0.3, 0.2, 0.01 })
    {
        const double asFloat = constant * floatSteps(z), asUnorm = constant * unormStep;
        printf("depth %.2f: float steps %.3g (%.3f of the offset), unorm24 steps %.3g (%.3f)\n", z, asFloat, asFloat / offset, asUnorm,
            asUnorm / offset);
        CHECK(asUnorm == double(offset), "depth %.2f: unorm24 steps give the offset (%g, want %g)", z, asUnorm, double(offset));
        // The float steps: the offset only from 0.5 up, less below.
        CHECK(z >= 0.5 ? asFloat == double(offset) : asFloat < double(offset), "depth %.2f: float steps %g against the offset %g", z, asFloat,
            double(offset));
    }
}

// renderer.cpp's Draw, comments left out, without white space.
static std::string DrawBody(const std::string& source)
{
    std::string code;
    std::istringstream in(source);
    for (std::string line; std::getline(in, line);)
    {
        size_t c = line.find("//");
        code += (c == std::string::npos ? line : line.substr(0, c)) + "\n";
    }
    size_t start = code.find("void Draw(uint32_t initiatorValue");
    if (start == std::string::npos)
        return {};
    size_t end = code.find("void Submit()", start);
    std::string draw = code.substr(start, end == std::string::npos ? std::string::npos : end - start), squashed;
    for (char ch : draw)
        if (ch != ' ' && ch != '\n' && ch != '\t' && ch != '\r')
            squashed += ch;
    return squashed;
}

static void TestRenderer(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "cannot read %s", path);
    if (!f.good())
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string draw = DrawBody(ss.str());
    CHECK(!draw.empty(), "Draw not found in %s", path);
    if (draw.empty())
        return;
    const size_t choice = draw.find("constgpu::PolyOffsetpolyOffset=gpu::PreferredPolyOffset(");
    const size_t enable = draw.find("constbooldepthBias=polyOffset.enable;");
    const size_t key = draw.find("(depthBias?16u:0u)");
    CHECK(choice != std::string::npos, "Draw chooses the face's offset with gpu::PreferredPolyOffset");
    CHECK(enable != std::string::npos && choice < enable, "the pipeline's depth bias enable is the choice's");
    CHECK(key != std::string::npos && enable < key, "and goes into the pipeline key");
    CHECK(draw.find(".backScale=RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),.backOffset=RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET)") !=
            std::string::npos,
        "the back face's registers are read");
    CHECK(draw.find("bias[0]=polyOffset.offset*gpu::kUnorm24Steps;") != std::string::npos, "the constant is the chosen offset in unorm24 steps");
    CHECK(draw.find("bias[2]=polyOffset.scale/16.0f*") != std::string::npos, "the slope is the chosen scale");
    CHECK(draw.find("POLY_OFFSET_FRONT_OFFSET)*") == std::string::npos, "no front register read straight into the bias");
    CHECK(draw.find("constboolunormBias=gpu::BiasInUnorm24Steps(depthBias,") != std::string::npos, "the steps from BiasInUnorm24Steps");
    CHECK(draw.find("VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT") != std::string::npos,
        "unorm24 steps through FORCE_UNORM");
    CHECK(draw.find("bound.unormBias!=unormBias") != std::string::npos, "a change of steps alone sets the bias again");
}

int main(int argc, char** argv)
{
    TestChoice();
    TestSteps();
    if (argc > 1)
        TestRenderer(argv[1]);
    else
        printf("(no renderer.cpp given: its checks skipped)\n");
    if (g_failures)
    {
        printf("gpu_poly_offset_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_poly_offset_test: all passed\n");
    return 0;
}
