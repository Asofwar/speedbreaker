// Tests for gpu/texture_formats.h: the texture formats that hold another's
// data, and the components a host format lacks, as the renderer samples
// them. Up to v0.1.4 the *_AS_16_16_16_16 formats (50-56) and k_8_A were
// white 1x1 placeholders (overbright materials), though each holds exactly
// a format the renderer loads. Checked against xenos/xenos.h's enum (the
// names say which base each alias has) and Xenia's host swizzles, then the
// fetch constant's swizzle through them. With renderer.cpp's path (ctest
// passes it), LookupTexture is checked to load an alias as its base and to
// view textures with the composed swizzle, and the untile's new loads to
// read the components where the vertex formats of the same names have them
// (shader_translator.cpp, its second argument).
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/gpu_texture_formats_test.cpp -o build/gpu_texture_formats_test
//   build/gpu_texture_formats_test runtime/gpu/renderer.cpp runtime/gpu/shader_translator.cpp
#include <gpu/texture_formats.h>
#include <gpu/xenos/xenos.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using xe::gpu::xenos::TextureFormat;
using namespace gpu::texformat;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t F(TextureFormat f)
{
    return uint32_t(f);
}

static void TestAliases()
{
    struct { TextureFormat alias, base; } aliases[] = {
        { TextureFormat::k_8_A, TextureFormat::k_8 },
        { TextureFormat::k_8_8_8_8_AS_16_16_16_16, TextureFormat::k_8_8_8_8 },
        { TextureFormat::k_DXT1_AS_16_16_16_16, TextureFormat::k_DXT1 },
        { TextureFormat::k_DXT2_3_AS_16_16_16_16, TextureFormat::k_DXT2_3 },
        { TextureFormat::k_DXT4_5_AS_16_16_16_16, TextureFormat::k_DXT4_5 },
        { TextureFormat::k_2_10_10_10_AS_16_16_16_16, TextureFormat::k_2_10_10_10 },
        { TextureFormat::k_10_11_11_AS_16_16_16_16, TextureFormat::k_10_11_11 },
        { TextureFormat::k_11_11_10_AS_16_16_16_16, TextureFormat::k_11_11_10 },
    };
    for (auto [alias, base] : aliases)
    {
        CHECK(SampledAs(F(alias)) == F(base), "format %u samples as %u (got %u)", F(alias), F(base), SampledAs(F(alias)));
        CHECK(HostSwizzle(SampledAs(F(alias))) == HostSwizzle(F(base)), "format %u keeps its base's host swizzle", F(alias));
    }
    for (uint32_t f = 0; f < 64; f++)
    {
        bool isAlias = false;
        for (auto [alias, base] : aliases)
            isAlias |= F(alias) == f;
        if (!isAlias)
            CHECK(SampledAs(f) == f, "format %u is its own (got %u)", f, SampledAs(f));
        CHECK(SampledAs(SampledAs(f)) == SampledAs(f), "an alias's base is no alias (%u)", f);
    }
    CHECK(SampledAs(F(TextureFormat::k_8_B)) == F(TextureFormat::k_8_B), "k_8_B: unknown (Xenia has no load either), not k_8");
}

static void TestHostSwizzles()
{
    // Xenia's kBestHostFormats (vulkan_texture_cache.cc) for the formats the
    // renderer loads, but the noted ones.
    struct { TextureFormat f; uint32_t swizzle; } table[] = {
        { TextureFormat::k_8, kRRRR },
        { TextureFormat::k_1_5_5_5, kRGBA },
        { TextureFormat::k_5_6_5, kRGBB },
        { TextureFormat::k_8_8_8_8, kRGBA },
        { TextureFormat::k_2_10_10_10, kRGBA },
        { TextureFormat::k_8_8, kRGGG },
        { TextureFormat::k_8_8_8_8_A, kRGBA },
        { TextureFormat::k_4_4_4_4, kRGBA },
        { TextureFormat::k_DXT1, kRGBA },
        { TextureFormat::k_DXT2_3, kRGBA },
        { TextureFormat::k_DXT4_5, kRGBA },
        { TextureFormat::k_24_8, kRRRR },
        { TextureFormat::k_24_8_FLOAT, kRRRR },
        { TextureFormat::k_16, kRRRR },
        { TextureFormat::k_16_16, kRGGG },
        { TextureFormat::k_16_16_16_16, kRGBA },
        { TextureFormat::k_16_FLOAT, kRRRR },
        { TextureFormat::k_16_16_FLOAT, kRGGG },
        { TextureFormat::k_16_16_16_16_FLOAT, kRGBA },
        { TextureFormat::k_32_FLOAT, kRRRR },
        { TextureFormat::k_32_32_FLOAT, kRGGG },
        { TextureFormat::k_32_32_32_32_FLOAT, kRGBA },
        { TextureFormat::k_DXN, kRGGG },
        { TextureFormat::k_DXT5A, kRRRR },
        // Xenia: RGBB; the renderer's load writes blue into alpha itself.
        { TextureFormat::k_10_11_11, kRGBA },
        { TextureFormat::k_11_11_10, kRGBA },
        // Xenia: R8 with RRRR; the renderer loads BC2 blocks with the
        // alphas in their alpha half.
        { TextureFormat::k_DXT3A, kAAAA },
    };
    for (auto [f, swizzle] : table)
        CHECK(HostSwizzle(F(f)) == swizzle, "format %u: host swizzle %03o, want %03o", F(f), HostSwizzle(F(f)), swizzle);
    CHECK(kRGBA == 0 + (1 << 3) + (2 << 6) + (3 << 9), "kRGBA is the identity");
}

static void TestCompose()
{
    const uint32_t xyzw = Swizzle(0, 1, 2, 3);
    CHECK(Compose(xyzw, kRGBA) == xyzw, "identity through identity");
    CHECK(Compose(xyzw, kRRRR) == kRRRR, "a one-component format: every component its only one");
    CHECK(Compose(xyzw, kRGGG) == kRGGG, "two components: the last repeated");
    // 4B4E083C's text (k_8_A): 111W, the only component as alpha.
    CHECK(Compose(Swizzle(5, 5, 5, 3), kRRRR) == Swizzle(5, 5, 5, 0), "111W of k_8: 1, 1, 1 and the red channel (got %04o)",
        Compose(Swizzle(5, 5, 5, 3), kRRRR));
    CHECK(Compose(Swizzle(0, 0, 0, 5), kRRRR) == Swizzle(0, 0, 0, 5), "XXX1 (D3D's L8) unchanged");
    CHECK(Compose(Swizzle(3, 2, 1, 0), kRGBB) == Swizzle(2, 2, 1, 0), "WZYX of 5_6_5: blue for the missing alpha");
    CHECK(Compose(Swizzle(4, 5, 6, 7), kAAAA) == Swizzle(4, 5, 6, 7), "0, 1 and the 'as is' selectors pass through");
    CHECK(Compose(Swizzle(0, 1, 2, 3), kAAAA) == kAAAA, "DXT3A: every component BC2's alpha");
}

static std::string Read(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "cannot read %s", path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string Squash(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (c != ' ' && c != '\n' && c != '\t')
            r += c;
    return r;
}

static void TestRenderer(const char* path)
{
    const std::string all = Squash(Read(path));
    CHECK(all.find("uint32_tformat=texformat::SampledAs(uint32_t(f.format));") != std::string::npos,
        "LookupTexture loads (and keys) an alias as its base");
    CHECK(all.find("constuint32_tswizzle=texformat::Compose(f.swizzle,texformat::HostSwizzle(format));") != std::string::npos,
        "LookupTexture composes the fetch constant's swizzle with the host format's");
    CHECK(all.find("TextureView(t,f.swizzle)") == std::string::npos && all.find("returnTextureView(t,swizzle);") != std::string::npos,
        "every view LookupTexture hands out has the composed swizzle");
    for (const char* entry : { "case16:out={VK_FORMAT_R16G16B16A16_UNORM,2,1,5};", "case17:out={VK_FORMAT_R16G16B16A16_UNORM,2,1,6};",
             "case58:out={VK_FORMAT_BC2_UNORM_BLOCK,3,4,7};", "case59:out={VK_FORMAT_BC4_UNORM_BLOCK,3,4,0};" })
        CHECK(all.find(entry) != std::string::npos, "GetTextureFormat has %s", entry);
    // The wider loads never take the direct path (32-bit texels through an
    // R32_UINT view), and are branched to before the 32-bit conversions.
    CHECK(all.find("fi.block==1&&LoadedBpbLog2(fi)==2&&") != std::string::npos, "only 32-bit loaded texels are stored directly");
    const size_t wide = all.find("if(p.convert>=5u)"), narrow = all.find("if(p.convert!=0u)");
    CHECK(wide != std::string::npos && narrow != std::string::npos && wide < narrow, "the untile takes converts 5-7 before 1-4");
}

// k_10_11_11 and k_11_11_10: the untile reads red from the low bits, as the
// vertex formats of those names are read.
static void TestLayouts(const char* renderer, const char* translator)
{
    const std::string r = Squash(Read(renderer)), t = Squash(Read(translator));
    CHECK(t.find("casexenos::VertexFormat::k_10_11_11:value=packed({{0,11},{11,11},{22,10}},1);") != std::string::npos,
        "the vertex format k_10_11_11: 11 bits from 0, 11 from 11, 10 from 22");
    CHECK(t.find("casexenos::VertexFormat::k_11_11_10:value=packed({{0,10},{10,11},{21,11}},1);") != std::string::npos,
        "the vertex format k_11_11_10: 10 bits from 0, 11 from 10, 11 from 21");
    CHECK(r.find("if(p.convert==5u){r=expand16(w&0x7FFu,11u);g=expand16((w>>11)&0x7FFu,11u);b=expand16(w>>22,10u);}") != std::string::npos,
        "the untile's k_10_11_11: the same fields");
    CHECK(r.find("else{r=expand16(w&0x3FFu,10u);g=expand16((w>>10)&0x7FFu,11u);b=expand16(w>>21,11u);}") != std::string::npos,
        "the untile's k_11_11_10: the same fields");
}

int main(int argc, char** argv)
{
    TestAliases();
    TestHostSwizzles();
    TestCompose();
    if (argc > 1)
        TestRenderer(argv[1]);
    else
        printf("(no renderer.cpp given: its checks skipped)\n");
    if (argc > 2)
        TestLayouts(argv[1], argv[2]);
    if (g_failures)
    {
        printf("gpu_texture_formats_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_texture_formats_test: all passed\n");
    return 0;
}
