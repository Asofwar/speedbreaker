// Tests for the pipeline key list (runtime/gpu/pipeline_keys.h): the file
// that lists the pipelines earlier sessions drew with and the guest shaders
// they need, so that the renderer makes them as the game starts (renderer.cpp,
// "Pipeline prewarm"). The file's round trip and every way it can be another
// build's, another GPU's, cut short or corrupt; the most recently used keys
// kept past the cap; a session's recorder stamping and adding keys. From the
// repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/pipeline_keys_test.cpp -o build/pipeline_keys_test
//   build/pipeline_keys_test runtime/gpu/renderer.cpp
// With the path (ctest passes it), renderer.cpp is also checked to hash
// shaders as the list does, to note every pipeline it draws with, to make
// only what this session's Draw would and to give draws' compiles priority
// over the prewarm's.
#include <gpu/pipeline_keys.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace gpu::pipeline_keys;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

constexpr uint32_t kAmd = 0x1002, kVanGogh = 0x163F;  // the Steam Deck's GPU

// A key naming shaders `vs` and `ps`, the rest of its bytes from `seed`.
static Key MakeKey(uint64_t vs, uint64_t ps, uint8_t seed)
{
    Key k{};
    std::memcpy(k.data(), &vs, 8);
    std::memcpy(k.data() + 8, &ps, 8);
    for (size_t i = 16; i < k.size(); i++)
        k[i] = uint8_t(seed + i);
    return k;
}

static Shader MakeShaderWords(uint32_t type, uint32_t first, uint32_t count)
{
    Shader s;
    s.type = type;
    for (uint32_t i = 0; i < count; i++)
        s.words.push_back(first * 0x01000193u + i);
    return s;
}

// A list of three keys over a vertex shader and two pixel shaders, as a
// session on the Deck would save it.
static List SampleList()
{
    List l;
    l.vendorID = kAmd;
    l.deviceID = kVanGogh;
    l.session = 7;
    const Shader vs = MakeShaderWords(0, 1, 40), ps1 = MakeShaderWords(1, 2, 25), ps2 = MakeShaderWords(1, 3, 3);
    const uint64_t hv = ShaderHash(0, vs.words.data(), vs.words.size());
    const uint64_t h1 = ShaderHash(1, ps1.words.data(), ps1.words.size());
    const uint64_t h2 = ShaderHash(1, ps2.words.data(), ps2.words.size());
    l.shaders[hv] = vs;
    l.shaders[h1] = ps1;
    l.shaders[h2] = ps2;
    l.keys.push_back({ MakeKey(hv, h1, 1), 7 });
    l.keys.push_back({ MakeKey(hv, h2, 2), 5 });
    l.keys.push_back({ MakeKey(hv, h1, 3), 6 });
    return l;
}

static bool SameList(const List& a, const List& b)
{
    if (a.vendorID != b.vendorID || a.deviceID != b.deviceID || a.session != b.session || a.keys.size() != b.keys.size() ||
        a.shaders.size() != b.shaders.size())
        return false;
    for (size_t i = 0; i < a.keys.size(); i++)
        if (a.keys[i].key != b.keys[i].key || a.keys[i].lastSession != b.keys[i].lastSession)
            return false;
    for (const auto& [hash, s] : a.shaders)
    {
        auto it = b.shaders.find(hash);
        if (it == b.shaders.end() || it->second.type != s.type || it->second.words != s.words)
            return false;
    }
    return true;
}

// The checksum written again after a deliberate change, so that the
// structure's own checks are what is tested.
static void Reseal(std::vector<uint8_t>& bytes)
{
    const uint64_t sum = Fnv1a(bytes.data(), bytes.size() - 8);
    for (int i = 0; i < 8; i++)
        bytes[bytes.size() - 8 + i] = uint8_t(sum >> (8 * i));
}

static void PutAt(std::vector<uint8_t>& bytes, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        bytes[at + i] = uint8_t(v >> (8 * i));
}

static void TestHash()
{
    // FNV-1a 64 over the words as loaded, seeded with the type: what the
    // renderer's LoadShader has always computed (a key list names shaders
    // by it, so a change would orphan every saved list).
    const uint32_t words[3] = { 0x12345678, 0x9ABCDEF0, 0x0F0F0F0F };
    uint64_t h = 0xCBF29CE484222325ull ^ 1;
    for (uint32_t w : words)
        h = (h ^ w) * 0x100000001B3ull;
    CHECK(ShaderHash(1, words, 3) == h, "%016llx vs %016llx", (unsigned long long)ShaderHash(1, words, 3), (unsigned long long)h);
    CHECK(ShaderHash(0, words, 3) != ShaderHash(1, words, 3), "the type is part of the hash");
    const Key k = MakeKey(0x1111222233334444ull, 0x5555666677778888ull, 9);
    CHECK(ShaderOf(k, 0) == 0x1111222233334444ull && ShaderOf(k, 1) == 0x5555666677778888ull, "a key's first 16 bytes name its shaders");
}

static void TestRoundTrip()
{
    const List l = SampleList();
    const std::vector<uint8_t> bytes = Encode(l);
    CHECK(bytes.size() == kHeaderBytes + 3 * 16 + 4 * (40 + 25 + 3) + 3 * (kKeyBytes + 4) + 8, "size %zu", bytes.size());
    CHECK(Encode(l) == bytes, "the same list gives the same bytes");
    const Decoded d = Decode(bytes.data(), bytes.size(), kAmd, kVanGogh);
    CHECK(d.verdict == Verdict::Usable, "verdict %d (%s)", int(d.verdict), d.why.c_str());
    CHECK(SameList(d.list, l), "the list read back is the list written");

    // Nothing to read: no list yet (a first session).
    CHECK(Decode(nullptr, 0, kAmd, kVanGogh).verdict == Verdict::Empty, "no file");
    List empty;
    empty.vendorID = kAmd;
    empty.deviceID = kVanGogh;
    const std::vector<uint8_t> none = Encode(empty);
    const Decoded e = Decode(none.data(), none.size(), kAmd, kVanGogh);
    CHECK(e.verdict == Verdict::Usable && e.list.keys.empty() && e.list.shaders.empty(), "an empty list is a list");
}

static void TestUnreadable()
{
    const std::vector<uint8_t> good = Encode(SampleList());
    auto verdict = [](const std::vector<uint8_t>& b) { return Decode(b.data(), b.size(), kAmd, kVanGogh); };

    std::vector<uint8_t> b(good.begin(), good.begin() + 20);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "shorter than a header: %s", verdict(b).why.c_str());

    b = good;
    b[0] ^= 0xFF;
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "another file's magic: %s", verdict(b).why.c_str());

    b = good;
    PutAt(b, 4, kFormat + 1);
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "another build's format (its keys mean something else): %s", verdict(b).why.c_str());

    b = good;
    PutAt(b, 8, kKeyBytes + 8);
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "another key size: %s", verdict(b).why.c_str());

    // Any changed byte fails the checksum (a write cut short, a bad disk).
    for (size_t at : { size_t(kHeaderBytes + 20), good.size() / 2, good.size() - 30 })
    {
        b = good;
        b[at] ^= 0x01;
        CHECK(verdict(b).verdict == Verdict::Unreadable && verdict(b).why == "checksum mismatch", "byte %zu changed: %s", at,
            verdict(b).why.c_str());
    }
    b.assign(good.begin(), good.end() - 1);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "cut by a byte");

    // Counts that don't fit the bytes, checksum or not.
    b = good;
    PutAt(b, 24, 1000000);
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "a million shaders in a small file: %s", verdict(b).why.c_str());
    b = good;
    PutAt(b, 28, 4);
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "one key more than written: %s", verdict(b).why.c_str());
    b = good;
    PutAt(b, 28, 2);
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "one key less than written: %s", verdict(b).why.c_str());
    b = good;
    PutAt(b, kHeaderBytes + 12, 0x7FFFFFFF);  // the first shader's dwords
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "a shader longer than the file: %s", verdict(b).why.c_str());
    b = good;
    PutAt(b, kHeaderBytes + 8, 2);  // the first shader's type
    Reseal(b);
    CHECK(verdict(b).verdict == Verdict::Unreadable, "a shader of no stage: %s", verdict(b).why.c_str());
}

static void TestOtherGpu()
{
    const std::vector<uint8_t> bytes = Encode(SampleList());
    const Decoded nvidia = Decode(bytes.data(), bytes.size(), 0x10DE, 0x2684);
    CHECK(nvidia.verdict == Verdict::OtherGpu && nvidia.list.keys.empty(), "another vendor: %s", nvidia.why.c_str());
    const Decoded other = Decode(bytes.data(), bytes.size(), kAmd, 0x1681);
    CHECK(other.verdict == Verdict::OtherGpu, "another AMD GPU: %s", other.why.c_str());

    // MoltenVK on an M1 Pro: the device ID carries the macOS version (26.5
    // 0x1a050207, 27.0 0x1b000207): an OS update is the same GPU; another
    // GPU family isn't.
    List mac = SampleList();
    mac.vendorID = kAppleVendor;
    mac.deviceID = 0x1a050207;
    const std::vector<uint8_t> macBytes = Encode(mac);
    CHECK(Decode(macBytes.data(), macBytes.size(), kAppleVendor, 0x1b000207).verdict == Verdict::Usable, "macOS 26.5 -> 27.0");
    CHECK(Decode(macBytes.data(), macBytes.size(), kAppleVendor, 0x1b000208).verdict == Verdict::OtherGpu, "another Apple GPU family");
}

static void TestPrune()
{
    // Five keys over four shaders, used last in sessions 1, 3, 2, 3, 1.
    List l;
    const uint32_t sessions[5] = { 1, 3, 2, 3, 1 };
    for (uint32_t i = 0; i < 5; i++)
    {
        const Shader ps = MakeShaderWords(1, 10 + i, 4);
        const uint64_t hp = ShaderHash(1, ps.words.data(), ps.words.size());
        l.shaders[hp] = ps;
        l.keys.push_back({ MakeKey(0xAAAA, hp, uint8_t(i)), sessions[i] });
    }
    const List before = l;
    CHECK(Prune(l, 5) == 0 && SameList(l, before), "nothing over the cap: nothing changes");

    // Three kept: the two of session 3 and the one of session 2, in their
    // order; the session-1 keys' shaders go with them.
    CHECK(Prune(l, 3) == 2, "two dropped");
    CHECK(l.keys.size() == 3 && l.keys[0].key == before.keys[1].key && l.keys[1].key == before.keys[2].key &&
            l.keys[2].key == before.keys[3].key,
        "the most recently used kept, in order");
    CHECK(l.shaders.size() == 3 && !l.shaders.count(ShaderOf(before.keys[0].key, 1)) && !l.shaders.count(ShaderOf(before.keys[4].key, 1)),
        "the dropped keys' shaders dropped (%zu left)", l.shaders.size());

    // Equally recent: the earlier listed (the boot's before a race's).
    List tie;
    for (uint8_t i = 0; i < 4; i++)
        tie.keys.push_back({ MakeKey(1, 2, i), 4 });
    Prune(tie, 2);
    CHECK(tie.keys.size() == 2 && tie.keys[0].key == MakeKey(1, 2, 0) && tie.keys[1].key == MakeKey(1, 2, 1), "ties keep the first listed");
}

static void TestRecorder()
{
    const List loaded = SampleList();  // saved by session 7
    Recorder r;
    r.Start(loaded);
    CHECK(r.Session() == 8 && !r.Dirty(), "session %u", r.Session());
    CHECK(r.Has(loaded.keys[1].key) && !r.Has(MakeKey(1, 2, 3)), "Has");

    // A listed key drawn with: stamped, not added.
    CHECK(!r.Note(loaded.keys[1].key), "a listed key isn't new");
    CHECK(r.Dirty() && r.Get().keys[1].lastSession == 8 && r.Get().keys.size() == 3, "stamped with this session");

    // A new one: at the end, its shaders added once (the depth-only pixel
    // shader has no words and isn't).
    const Shader vs = MakeShaderWords(0, 77, 12);
    const uint64_t hv = ShaderHash(0, vs.words.data(), vs.words.size());
    const Key fresh = MakeKey(hv, 0xDE97400000000001ull, 5);
    CHECK(r.Note(fresh), "a new key is new");
    CHECK(!r.Note(fresh), "once");
    r.NoteShader(hv, 0, vs.words);
    r.NoteShader(hv, 0, vs.words);
    r.NoteShader(0xDE97400000000001ull, 1, {});
    CHECK(r.Get().keys.size() == 4 && r.Get().keys[3].key == fresh && r.Get().keys[3].lastSession == 8, "appended");
    CHECK(r.Get().shaders.size() == 4 && r.Get().shaders.count(hv) && !r.Get().shaders.count(0xDE97400000000001ull),
        "%zu shaders", r.Get().shaders.size());

    // Saved: this session's number, untouched keys keep theirs; read back
    // as the next session would.
    const std::vector<uint8_t> bytes = r.Save(kAmd, kVanGogh);
    CHECK(!r.Dirty(), "saved");
    const Decoded d = Decode(bytes.data(), bytes.size(), kAmd, kVanGogh);
    CHECK(d.verdict == Verdict::Usable && d.list.session == 8 && d.list.keys.size() == 4, "read back: %d, session %u, %zu keys",
        int(d.verdict), d.list.session, d.list.keys.size());
    if (d.list.keys.size() == 4)
        CHECK(d.list.keys[0].lastSession == 7 && d.list.keys[1].lastSession == 8 && d.list.keys[2].lastSession == 6 &&
                d.list.keys[3].lastSession == 8,
            "stamps %u %u %u %u", d.list.keys[0].lastSession, d.list.keys[1].lastSession, d.list.keys[2].lastSession,
            d.list.keys[3].lastSession);
    r.MarkUnsaved();
    CHECK(r.Dirty(), "a failed write saves again");

    // Over the cap at a save: the oldest go, and coming back they are new.
    Recorder small;
    small.Start(d.list);  // session 9
    small.Note(d.list.keys[2].key);  // stamped 9
    const std::vector<uint8_t> two = small.Save(kAmd, kVanGogh, 2);
    const Decoded d2 = Decode(two.data(), two.size(), kAmd, kVanGogh);
    CHECK(d2.list.keys.size() == 2 && d2.list.keys[0].key == d.list.keys[1].key && d2.list.keys[1].key == d.list.keys[2].key,
        "kept the session-8 key before the session-9 one, in order (%zu)", d2.list.keys.size());
    CHECK(small.Note(d.list.keys[0].key), "a dropped key is new again");
}

// The source without spaces, tabs and newlines (formatting can't break a check).
static std::string Squash(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r')
            r += c;
    return r;
}

static std::string Between(const std::string& all, const std::string& from, const std::string& to)
{
    size_t a = all.find(from);
    if (a == std::string::npos)
        return {};
    size_t b = all.find(to, a + from.size());
    return b == std::string::npos ? std::string() : all.substr(a, b - a);
}

static void TestRenderer(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "cannot read %s", path);
    if (!f.good())
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string all = Squash(ss.str());
    auto has = [&](const std::string& in, const std::string& what) { return in.find(what) != std::string::npos; };

    // Shaders are named in keys by the list's hash, and keep their words.
    const std::string load = Between(all, "voidLoadShader(", "voidDraw(");
    CHECK(has(load, "gpu::pipeline_keys::ShaderHash(type,w,n)"), "LoadShader hashes as the key list does");
    const std::string make = Between(all, "Shader*MakeShader(", "//IM_LOADshaders");
    CHECK(has(make, "slot->words.assign(words,words+dwords);"), "a shader keeps the words it was made from");

    // Every pipeline drawn with is noted: a new one when a draw takes it in,
    // a prewarmed one at its first draw.
    const std::string get = Between(all, "VkPipelineGetPipeline(", "//Pipelineprewarm");
    CHECK(has(get, "if(job->pipeline)NotePipelineKey(key,job->vs,job->ps);"), "GetPipeline notes the pipelines it takes in");
    CHECK(has(get, "if(it->second.prewarmed)[[unlikely]]{") && has(get, "it->second.prewarmed=false;NotePipelineKey("),
        "a prewarmed pipeline's first draw stamps it");
    CHECK(has(get, "PromotePrewarmJob(job);"), "a draw waiting for a prewarm job queues it with the draws'");

    // The workers take the draws' jobs first, and one worker of two or
    // more is always free for them.
    CHECK(has(all, "prewarm=s_compileQueue.empty();auto&queue=prewarm?s_prewarmQueue:s_compileQueue;"),
        "draws' compiles before the prewarm's");
    CHECK(has(all, "constuint32_ts_prewarmWorkers=std::max(s_pipelineThreads,2u)-1;") &&
            has(all, "return!s_prewarmQueue.empty()&&s_prewarmActive<s_prewarmWorkers;"),
        "the prewarm leaves a worker for draws");

    // Only what this session's Draw makes, from shaders that are the ones
    // the key names.
    const std::string usable = Between(all, "boolPrewarmKeyUsable(", "voidLoadPipelineKeys(");
    CHECK(has(usable, "if(((key.raster>>6)&1)!=uint32_t(s_dynamicState)"), "dynamic state on or off as this session");
    CHECK(has(usable, "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT") && has(usable, "VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT"),
        "formats the device renders to");
    const std::string shader = Between(all, "Shader*PrewarmShader(", "boolPrewarmKeyUsable(");
    CHECK(has(shader, "if(gpu::pipeline_keys::ShaderHash(type,words.data(),words.size())!=hash)returnnullptr;"),
        "a listed shader is made only from words that hash to its name");

    // Saved with the pipeline cache, periodically and at exit.
    const std::string shutdown = Between(all, "voidShutdown(){", "voidHitchReport(");
    CHECK(has(shutdown, "s_prewarmQueue.clear();") && has(shutdown, "SavePipelineCache();SavePipelineKeys();"), "Shutdown saves the list");
    CHECK(has(all, "SavePipelineCache();SavePipelineKeys();saving=false;"), "the periodic save writes it too");
    CHECK(has(all, "CreatePipelineCache();LoadPipelineKeys();"), "Initialize loads it");
}

int main(int argc, char** argv)
{
    TestHash();
    TestRoundTrip();
    TestUnreadable();
    TestOtherGpu();
    TestPrune();
    TestRecorder();
    if (argc > 1)
        TestRenderer(argv[1]);
    else
        printf("(no renderer.cpp given: its checks skipped)\n");
    if (g_failures)
    {
        printf("pipeline_keys_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("pipeline_keys_test: all passed\n");
    return 0;
}
