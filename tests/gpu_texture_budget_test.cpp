// Tests for gpu/texture_budget.h: when the renderer's texture cache gives
// textures back, and which. Up to v0.1.4 it never did, and a long free roam
// (city streaming reuses guest addresses for textures of other sizes and
// formats: each a new texture) grew it until an allocation aborted the game
// or the system killed it. The rules alone, then a free roam modelled as
// the renderer drives them (a check every 30 swaps, the scene's textures
// used at every swap, a new set at each area): the cache stays bounded, no
// texture the scene draws is ever evicted, an area driven back to soon
// keeps its textures, and the old rule (never evict) is shown to grow
// without bound. With renderer.cpp's path (ctest passes it), the renderer
// is checked to mark every use and to trim at the swap.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/gpu_texture_budget_test.cpp -o build/gpu_texture_budget_test
//   build/gpu_texture_budget_test runtime/gpu/renderer.cpp
#include <gpu/texture_budget.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace gpu::texbudget;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void TestDefaults()
{
    CHECK(DefaultBytes(1024 * kMB) == 512 * kMB, "a 1 GB carve-out: half, %llu MB", (unsigned long long)(DefaultBytes(1024 * kMB) / kMB));
    CHECK(DefaultBytes(3072 * kMB) == 1536 * kMB, "3 GB: half");
    CHECK(DefaultBytes(16384 * kMB) == 2048 * kMB, "16 GB: the ceiling");
    CHECK(DefaultBytes(0) == 512 * kMB, "no device-local heap known: the floor");
    CHECK(DefaultBytes(256 * kMB) == 512 * kMB, "a small heap: still the floor (the cap only removes stale textures)");
    CHECK(DefaultBytes(8192 * kMB, kDefaultCeilingIos) == 1024 * kMB, "iOS: 1 GB at most");
    CHECK(DefaultBytes(1024 * kMB, kDefaultCeilingIos) == 512 * kMB, "iOS, a small heap: half");
    CHECK(DefaultBytes(8192 * kMB, 256 * kMB) == 256 * kMB, "a ceiling below the floor wins");
    Limits l;
    CHECK(l.minAge == kDefaultMinAge && l.bytes == 0 && l.count == 0, "Limits default to no caps and the default age");
}

static void TestOver()
{
    Limits l{ 1024 * kMB, 1000, 600 };
    CHECK(!Over(l, { 1024 * kMB, 1000, 0, 0 }).Any(), "at the caps: nothing");
    Excess e = Over(l, { 1100 * kMB, 10, 0, 0 });
    CHECK(e.Any() && e.overBytes && !e.overCount && !e.overDevice, "over the byte cap only");
    CHECK(e.bytes == 1100 * kMB - 896 * kMB && e.count == 0, "down to 7/8 of the cap: %llu MB", (unsigned long long)(e.bytes / kMB));
    e = Over(l, { 10 * kMB, 1200, 0, 0 });
    CHECK(e.overCount && !e.overBytes && e.count == 1200 - 875 && e.bytes == 0, "over the count cap: down to 7/8 (%u)", e.count);
    CHECK(!Over({ 0, 0, 600 }, { 100000 * kMB, 1000000, 0, 0 }).Any(), "no caps, no budget: nothing");
    // The device's budget: over 9/10 of it, down to 4/5 (what textures can give).
    e = Over({ 0, 0, 600 }, { 500 * kMB, 10, 950 * kMB, 1000 * kMB });
    CHECK(e.overDevice && e.bytes == 150 * kMB, "device 95%% of its budget: free 150 MB (%llu)", (unsigned long long)(e.bytes / kMB));
    CHECK(!Over({ 0, 0, 600 }, { 500 * kMB, 10, 900 * kMB, 1000 * kMB }).Any(), "device at 90%%: nothing");
    e = Over({ 0, 0, 600 }, { 50 * kMB, 10, 990 * kMB, 1000 * kMB });
    CHECK(e.overDevice && e.bytes == 50 * kMB, "the device's excess beyond the textures: all the textures can give (%llu)",
        (unsigned long long)(e.bytes / kMB));
    e = Over(l, { 1100 * kMB, 10, 990 * kMB, 1000 * kMB });
    CHECK(e.overBytes && e.overDevice && e.bytes == 204 * kMB, "both: the larger excess (%llu MB)", (unsigned long long)(e.bytes / kMB));
}

static void TestPick()
{
    // lastUsed: 100 (old), 900 (recent), 1000 (now), 50 (oldest), 100 (old, larger)
    std::vector<Candidate> c = { { 10, 100 }, { 10, 900 }, { 10, 1000 }, { 10, 50 }, { 20, 100 } };
    Excess none;
    CHECK(Pick(c, 1000, 600, none).empty(), "nothing to give back: nothing picked");
    Excess e;
    e.bytes = 1000;
    std::vector<size_t> p = Pick(c, 1000, 600, e);
    CHECK(p == (std::vector<size_t>{ 3, 4, 0 }), "only those unused for 600 swaps, oldest first, larger first among equals (%zu picked)", p.size());
    e.bytes = 25;
    p = Pick(c, 1000, 600, e);
    CHECK(p == (std::vector<size_t>{ 3, 4 }), "stops once the bytes are covered (%zu picked)", p.size());
    e.bytes = 0;
    e.count = 2;
    p = Pick(c, 1000, 600, e);
    CHECK(p == (std::vector<size_t>{ 3, 4 }), "a count: that many (%zu picked)", p.size());
    e.bytes = 1000;
    e.count = 0;
    p = Pick(c, 1000, 0, e);
    CHECK(p.size() == 4 && std::find(p.begin(), p.end(), size_t(2)) == p.end(), "minAge 0: still never one used at `now` (%zu picked)", p.size());
    p = Pick(c, 1000, 1, e);
    CHECK(p.size() == 4 && p.back() == 1, "minAge 1 (a failed allocation): all but now's, the most recent last");
    std::vector<Candidate> future = { { 10, 2000 } };
    CHECK(Pick(future, 1000, 1, e).empty(), "a last use after `now` (never: a wrap) is not picked");
}

// A free roam as the renderer drives the policy: the scene's textures used
// at every swap (lastUsed = the swap), a check at every 30th, the picks
// removed at once (the renderer destroys them a submission or two later).
// Each area is a new set of keys: the guest addresses come round again, but
// with other sizes and formats, so nothing old is ever looked up again.
struct Roam
{
    struct Texture { uint64_t bytes, lastUsed; uint32_t area; };
    Limits limits;
    bool evict = true;              // false: the rule up to v0.1.4
    uint64_t deviceOther = 0, deviceBudget = 0;  // the rest of the process's device memory, and the budget
    uint64_t swap = 0, peakBytes = 0, evicted = 0, reloads = 0, evictedRecent = 0;
    uint32_t peakCount = 0;
    std::map<uint64_t, Texture> cache;
    uint64_t nextKey = 1;
    std::map<uint32_t, std::vector<uint64_t>> areaKeys;  // the keys each area's textures were last loaded under

    uint64_t Bytes() const
    {
        uint64_t b = 0;
        for (auto& [k, t] : cache)
            b += t.bytes;
        return b;
    }

    // `n` textures of `bytes` each; the HUD and the car are area 0, always
    // drawn. An area driven back to looks its textures up under the same
    // keys: one still cached is drawn without a load, one evicted loads again.
    void Load(uint32_t area, uint32_t n, uint64_t bytes)
    {
        std::vector<uint64_t>& keys = areaKeys[area];
        while (keys.size() < n)
            keys.push_back(nextKey++);
        for (uint64_t k : keys)
            if (!cache.count(k))
            {
                if (k < firstNew)
                    reloads++;
                cache[k] = { bytes, swap, area };
            }
        firstNew = nextKey;
    }
    uint64_t firstNew = 1;  // keys from here on are loaded for the first time

    void Drive(uint32_t area, uint32_t n, uint64_t bytes, uint64_t swaps)
    {
        for (uint64_t s = 0; s < swaps; s++)
        {
            swap++;
            Load(0, 64, 256 * 1024);
            Load(area, n, bytes);
            for (uint32_t a : { 0u, area })
                for (uint64_t k : areaKeys[a])
                    cache[k].lastUsed = swap;
            if (evict && swap % 30 == 0)
                Check();
            peakBytes = std::max(peakBytes, Bytes());
            peakCount = std::max(peakCount, uint32_t(cache.size()));
        }
    }

    void Check()
    {
        std::vector<uint64_t> keys;
        std::vector<Candidate> candidates;
        Usage usage;
        for (auto& [k, t] : cache)
        {
            keys.push_back(k);
            candidates.push_back({ t.bytes, t.lastUsed });
            usage.bytes += t.bytes;
        }
        usage.count = uint32_t(cache.size());
        if (deviceBudget)
        {
            usage.deviceUsed = deviceOther + usage.bytes;
            usage.deviceBudget = deviceBudget;
        }
        for (size_t i : Pick(candidates, swap, limits.minAge, Over(limits, usage)))
        {
            const Texture& t = cache[keys[i]];
            if (swap - t.lastUsed < limits.minAge)
                evictedRecent++;
            cache.erase(keys[i]);
            evicted++;
        }
    }
};

static void TestFreeRoam()
{
    // 40 areas of 200 textures of 1 MB, 20 s each at 60 fps (a long free
    // roam: about 13 minutes), against a 512 MB cap and 1000 textures.
    Roam roam;
    roam.limits = { 512 * kMB, 1000, 600 };
    for (uint32_t area = 1; area <= 40; area++)
        roam.Drive(area, 200, kMB, 1200);
    const uint64_t scene = 200 * kMB + 64 * 256 * 1024;
    CHECK(roam.peakBytes <= 512 * kMB + scene, "bounded: at most the cap and one scene, peak %llu MB",
        (unsigned long long)(roam.peakBytes / kMB));
    CHECK(roam.peakCount <= 1000 + 264, "bounded in count too, peak %u", roam.peakCount);
    CHECK(roam.evictedRecent == 0, "no texture drawn in the last 600 swaps was evicted (%llu were)", (unsigned long long)roam.evictedRecent);
    CHECK(roam.evicted > 0, "the dead areas' textures went");
    CHECK(roam.reloads == 0, "nothing the scene draws was ever loaded again (%llu reloads)", (unsigned long long)roam.reloads);
    for (uint64_t k : roam.areaKeys[0])
        CHECK(roam.cache.count(k), "the HUD's and car's textures, drawn at every swap, stay");

    // The rule up to v0.1.4: the same drive, nothing given back.
    Roam old;
    old.limits = roam.limits;
    old.evict = false;
    for (uint32_t area = 1; area <= 40; area++)
        old.Drive(area, 200, kMB, 1200);
    CHECK(old.peakBytes >= 8000 * kMB, "never evicting grows with the drive: %llu MB after 40 areas", (unsigned long long)(old.peakBytes / kMB));

    // A scene larger than the cap: over it, but nothing it draws goes (the
    // cap is soft), and no texture is loaded twice.
    Roam big;
    big.limits = { 256 * kMB, 0, 600 };
    for (uint32_t area = 1; area <= 6; area++)
        big.Drive(area, 400, kMB, 1200);
    CHECK(big.evictedRecent == 0 && big.reloads == 0, "a scene over the cap keeps its textures (%llu evicted recent, %llu reloads)",
        (unsigned long long)big.evictedRecent, (unsigned long long)big.reloads);
    CHECK(big.peakBytes <= 2 * 400 * kMB + 64 * 256 * 1024, "and the area before goes once it is old enough (peak %llu MB)",
        (unsigned long long)(big.peakBytes / kMB));

    // Driving back to the area just left within the minimum age: its
    // textures are still there (no reload); after much longer, they were
    // given back and load again.
    Roam back;
    back.limits = { 300 * kMB, 0, 600 };
    back.Drive(1, 200, kMB, 1200);
    back.Drive(2, 200, kMB, 300);   // 5 s away
    back.Drive(1, 200, kMB, 1200);
    CHECK(back.reloads == 0, "back within 5 s: nothing loaded again (%llu reloads)", (unsigned long long)back.reloads);
    back.Drive(2, 200, kMB, 3000);  // 50 s away
    const uint64_t before = back.reloads;
    back.Drive(1, 200, kMB, 60);
    CHECK(back.reloads > before && back.reloads - before <= 200, "back after 50 s, over the cap: what went loads again (%llu reloads)",
        (unsigned long long)(back.reloads - before));

    // The device's budget: little room beside the rest of the process (a
    // Steam Deck's VRAM, an iPhone): old areas go even under the caps.
    Roam tight;
    tight.limits = { 0, 0, 600 };
    tight.deviceOther = 700 * kMB;
    tight.deviceBudget = 1200 * kMB;
    for (uint32_t area = 1; area <= 20; area++)
        tight.Drive(area, 200, kMB, 1200);
    CHECK(tight.deviceOther + tight.peakBytes <= tight.deviceBudget / 10 * 9 + 200 * kMB + 64 * 256 * 1024,
        "the device stays near its budget: %llu of %llu MB", (unsigned long long)((tight.deviceOther + tight.peakBytes) / kMB),
        (unsigned long long)(tight.deviceBudget / kMB));
    CHECK(tight.evictedRecent == 0, "and the scene is never evicted for it");
}

static std::string Squash(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (c != ' ' && c != '\n' && c != '\t')
            r += c;
    return r;
}

// The renderer: every use of a cached texture marks it (a texture the frame
// being recorded uses must never look old: a failed allocation evicts
// anything older than the last swap and destroys it once the submitted work
// is done), and the check runs at the swap, after s_frame moves on.
static void TestRenderer(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "cannot read %s", path);
    if (!f.good())
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string all = Squash(ss.str());
    auto count = [&](const std::string& what) {
        size_t n = 0;
        for (size_t at = all.find(what); at != std::string::npos; at = all.find(what, at + 1))
            n++;
        return n;
    };
    CHECK(count("t.lastUsed=s_frame;") >= 2, "LookupTexture marks a texture at its creation (before its image) and at each lookup");
    CHECK(count("if(e.frame!=s_frame){e.frame=s_frame;e.texture->lastUsed=s_frame;}") == 1,
        "GetTexture marks a texture its kept lookup hands out (through each entry once a frame)");
    CHECK(count("e.frame=s_frame;//(thelookupsetitslastUsed)") == 1, "an entry a full lookup made has marked its texture this frame");
    // The draw memo hands out its draw's textures again only within the
    // frame its lookups marked them in, and an eviction forgets it.
    CHECK(all.find("m.frame!=s_frame") != std::string::npos && count("m.frame=s_frame;") == 1,
        "the draw memo serves only the frame its lookups marked their textures in");
    CHECK(all.find("c.memo.layout=nullptr;c.evictions++;") != std::string::npos, "an eviction forgets the draw memo");
    CHECK(count("fused->lastUsed=s_frame;") == 1, "a resolve storing into a cached texture marks it");
    const size_t swapAt = all.find("s_frame=frame;if(s_vk)TrimTextureCache();");
    CHECK(swapAt != std::string::npos, "OnSwap trims the cache right after s_frame moves on");
    CHECK(all.find("texbudget::Pick(candidates,s_frame,1,excess)") != std::string::npos,
        "a failed allocation evicts nothing used since the last swap");
    CHECK(all.find("ReclaimTextureMemory(req.size,round)") != std::string::npos, "CreateImage asks the cache for room when out of memory");
    CHECK(all.find("ForgetDirectTexture(*it->second);") != std::string::npos, "an evicted texture leaves the resolves' tables at once");
}

int main(int argc, char** argv)
{
    TestDefaults();
    TestOver();
    TestPick();
    TestFreeRoam();
    if (argc > 1)
        TestRenderer(argv[1]);
    else
        printf("(no renderer.cpp given: its checks skipped)\n");
    if (g_failures)
    {
        printf("gpu_texture_budget_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_texture_budget_test: all passed\n");
    return 0;
}
