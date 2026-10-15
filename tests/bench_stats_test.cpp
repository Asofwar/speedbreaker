// Tests for the benchmark recorder's statistics and file formats
// (runtime/debug/bench_stats.h): nearest-rank percentiles, the 1% and 0.1%
// low averages, the frame time summary, FNV-1a 64, and the JSON and CSV
// writers. From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/bench_stats_test.cpp -o build/bench_stats_test
//   build/bench_stats_test
#include <debug/bench_stats.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace bench::stats;

namespace
{
    bool Near(double a, double b, double eps = 1e-9)
    {
        return std::abs(a - b) <= eps;
    }

    void Percentiles()
    {
        // Nearest rank: the value at rank ceil(q/100 * n), from 1.
        std::vector<double> v{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        CHECK(Near(PercentileSorted(v, 50), 5), "p50 of 1..10 = %g", PercentileSorted(v, 50));
        CHECK(Near(PercentileSorted(v, 90), 9), "p90 of 1..10 = %g", PercentileSorted(v, 90));
        CHECK(Near(PercentileSorted(v, 91), 10), "p91 of 1..10 = %g (rank 9.1 rounds up)", PercentileSorted(v, 91));
        CHECK(Near(PercentileSorted(v, 99.9), 10), "p99.9 of 1..10 = %g", PercentileSorted(v, 99.9));
        CHECK(Near(PercentileSorted(v, 100), 10), "p100 = %g", PercentileSorted(v, 100));
        CHECK(Near(PercentileSorted(v, 0), 1), "p0 = %g (the smallest)", PercentileSorted(v, 0));
        CHECK(Near(PercentileSorted(v, 10), 1), "p10 = %g", PercentileSorted(v, 10));
        CHECK(Near(PercentileSorted(std::vector<double>{ 42 }, 50), 42), "one value");
        CHECK(PercentileSorted(std::vector<double>{}, 50) == 0, "empty: 0");
        // Values that happened only, never between two: p50 of 4 values is the 2nd.
        std::vector<double> four{ 10, 20, 30, 40 };
        CHECK(Near(PercentileSorted(four, 50), 20), "p50 of 4 = %g, not 25", PercentileSorted(four, 50));
        // 1000 frames: p99 is the 990th, p99.9 the 999th.
        std::vector<double> thousand;
        for (int i = 1; i <= 1000; i++)
            thousand.push_back(i);
        CHECK(Near(PercentileSorted(thousand, 99), 990), "p99 = %g", PercentileSorted(thousand, 99));
        CHECK(Near(PercentileSorted(thousand, 99.9), 999), "p99.9 = %g", PercentileSorted(thousand, 99.9));
    }

    void Lows()
    {
        CHECK(SlowestCount(0, 0.01) == 0, "none of none");
        CHECK(SlowestCount(50, 0.01) == 1, "at least one frame: %zu", SlowestCount(50, 0.01));
        CHECK(SlowestCount(100, 0.01) == 1, "1%% of 100: %zu", SlowestCount(100, 0.01));
        CHECK(SlowestCount(150, 0.01) == 2, "1%% of 150 rounds up: %zu", SlowestCount(150, 0.01));
        CHECK(SlowestCount(10000, 0.001) == 10, "0.1%% of 10000: %zu", SlowestCount(10000, 0.001));
        CHECK(SlowestCount(7200, 0.01) == 72, "1%% of 7200: %zu", SlowestCount(7200, 0.01));

        // 990 frames of 10 ms and 10 of 20 ms: the 1% low is the 10 slow
        // ones (50 fps), though p99 (the 990th) is still 10 ms (100 fps).
        std::vector<double> ms(990, 10.0);
        ms.insert(ms.end(), 10, 20.0);
        CHECK(Near(LowAverageFps(ms, 0.01), 50.0), "1%% low = %g fps", LowAverageFps(ms, 0.01));
        CHECK(Near(PercentileSorted(ms, 99), 10.0), "p99 = %g", PercentileSorted(ms, 99));
        // The 0.1% low of 1000 frames is the single slowest.
        ms.back() = 40.0;
        CHECK(Near(LowAverageFps(ms, 0.001), 25.0), "0.1%% low = %g fps", LowAverageFps(ms, 0.001));
        // Mean of the slowest, not the rate of the mean of all.
        std::vector<double> mix{ 10, 10, 10, 30, 50 };  // sorted
        CHECK(Near(LowAverageFps(mix, 0.4), 1000.0 / 40.0), "40%% low = %g", LowAverageFps(mix, 0.4));
        CHECK(LowAverageFps(std::vector<double>{}, 0.01) == 0, "empty: 0");
    }

    void Summary()
    {
        // 16, 17, 16, 17: mean 16.5, stdev 0.5, deltas all 1.
        std::vector<double> alt{ 16, 17, 16, 17 };
        Series a = Summarize(alt);
        CHECK(a.n == 4, "n = %zu", a.n);
        CHECK(Near(a.mean, 16.5), "mean = %g", a.mean);
        CHECK(Near(a.stdev, 0.5), "stdev = %g", a.stdev);
        CHECK(Near(a.meanAbsDelta, 1.0), "mean |delta| = %g", a.meanAbsDelta);
        CHECK(Near(a.max, 17), "max = %g", a.max);
        CHECK(Near(a.p50, 16), "p50 = %g", a.p50);
        // The same values in another order: same mean and percentiles,
        // smoother pacing.
        std::vector<double> grouped{ 16, 16, 17, 17 };
        Series g = Summarize(grouped);
        CHECK(Near(g.mean, a.mean) && Near(g.p50, a.p50) && Near(g.p99, a.p99), "order changes no distribution figure");
        CHECK(Near(g.meanAbsDelta, 1.0 / 3.0), "grouped mean |delta| = %g", g.meanAbsDelta);
        CHECK(a.low01Unreliable, "4 frames: 0.1%% low unreliable");

        std::vector<double> many(10000, 16.0);
        many[5000] = 100.0;
        Series m = Summarize(many);
        CHECK(!m.low01Unreliable, "10000 frames: reliable");
        CHECK(Near(m.max, 100.0), "max = %g", m.max);
        CHECK(Near(m.p999, 16.0), "p99.9 = %g (one slow frame in 10000 is above it)", m.p999);
        // 0.1% of 10000 = 10 frames: the 100 ms one and nine of 16 ms.
        CHECK(Near(m.low01Fps, 1000.0 / ((100.0 + 9 * 16.0) / 10.0)), "0.1%% low = %g", m.low01Fps);
        CHECK(Near(m.meanAbsDelta, (84.0 * 2) / 9999.0), "mean |delta| = %g", m.meanAbsDelta);

        Series e = Summarize(std::vector<double>{});
        CHECK(e.n == 0 && e.mean == 0 && e.max == 0, "empty");
        Series one = Summarize(std::vector<double>{ 20 });
        CHECK(one.meanAbsDelta == 0 && Near(one.low1Fps, 50.0), "one frame");
    }

    void Fnv()
    {
        // Published FNV-1a 64 test vectors.
        CHECK(Fnv1a64("", 0) == 0xCBF29CE484222325ull, "empty: %016llx", (unsigned long long)Fnv1a64("", 0));
        CHECK(Fnv1a64("a", 1) == 0xAF63DC4C8601EC8Cull, "\"a\": %016llx", (unsigned long long)Fnv1a64("a", 1));
        CHECK(Fnv1a64("foobar", 6) == 0x85944171F73967E8ull, "\"foobar\": %016llx", (unsigned long long)Fnv1a64("foobar", 6));
        // Integers as little-endian bytes, whatever the host's order.
        Fnv64 h;
        h.Add(uint32_t(0x64636261));  // "abcd"
        CHECK(h.Value() == Fnv1a64("abcd", 4), "u32 as LE bytes");
        Fnv64 h2;
        h2.Add(uint64_t(0x6867666564636261ull));  // "abcdefgh"
        CHECK(h2.Value() == Fnv1a64("abcdefgh", 8), "u64 as LE bytes");
        Fnv64 h3;
        h3.Add(int64_t(-1));
        const uint8_t ones[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        CHECK(h3.Value() == Fnv1a64(ones, 8), "i64 -1 as eight 0xFF");
        // Order matters: a frame trace swapped in two places differs.
        Fnv64 x, y;
        x.Add(uint32_t(1)); x.Add(uint32_t(2));
        y.Add(uint32_t(2)); y.Add(uint32_t(1));
        CHECK(x.Value() != y.Value(), "order-sensitive");
        CHECK(Hex64(0x9F3C00000000E1ull) == "009f3c00000000e1", "hex: %s", Hex64(0x9F3C00000000E1ull).c_str());
    }

    void JsonWriter()
    {
        CHECK(JsonEscape("a\"b\\c\nd\te") == "a\\\"b\\\\c\\nd\\te", "escapes: %s", JsonEscape("a\"b\\c\nd\te").c_str());
        CHECK(JsonEscape(std::string("x\x01y")) == "x\\u0001y", "control: %s", JsonEscape(std::string("x\x01y")).c_str());
        CHECK(JsonEscape("caf\xC3\xA9") == "caf\xC3\xA9", "UTF-8 kept");

        CHECK(Json::Number(16.667, 3) == "16.667", "%s", Json::Number(16.667, 3).c_str());
        CHECK(Json::Number(60.0, 3) == "60", "%s", Json::Number(60.0, 3).c_str());
        CHECK(Json::Number(0.5, 3) == "0.5", "%s", Json::Number(0.5, 3).c_str());
        CHECK(Json::Number(-0.0001, 3) == "0", "%s", Json::Number(-0.0001, 3).c_str());
        CHECK(Json::Number(1.0 / 3.0, 2) == "0.33", "%s", Json::Number(1.0 / 3.0, 2).c_str());
        CHECK(Json::Number(NAN, 3) == "null", "NaN");
        CHECK(Json::Number(INFINITY, 3) == "null", "inf");
        CHECK(Json::Number(1500.0, 0) == "1500", "%s (no decimals: no zeros trimmed)", Json::Number(1500.0, 0).c_str());

        Json j;
        j.Field("schema", 1).Field("status", "ok").Field("label", std::string("A \"quoted\""));
        j.BeginObject("fps").Field("avg", 73.25, 2).Field("unreliable", true).End();
        j.BeginObject("empty").End();
        j.BeginArray("worst");
        j.BeginObject().Field("swap", uint64_t(12)).Field("ms", 48.2).End();
        j.BeginObject().Field("swap", size_t(40)).Field("ms", 31.0).End();
        j.End();
        j.BeginArray("names").Item("a").Item(2.5).End();
        j.Null("divergence");
        j.Field("big", uint64_t(0xFFFFFFFFFFFFFFFFull)).Field("neg", int64_t(-5));
        std::string text = j.Finish();
        const char* expected =
            "{\n"
            "  \"schema\": 1,\n"
            "  \"status\": \"ok\",\n"
            "  \"label\": \"A \\\"quoted\\\"\",\n"
            "  \"fps\": {\n"
            "    \"avg\": 73.25,\n"
            "    \"unreliable\": true\n"
            "  },\n"
            "  \"empty\": {},\n"
            "  \"worst\": [\n"
            "    {\n"
            "      \"swap\": 12,\n"
            "      \"ms\": 48.2\n"
            "    },\n"
            "    {\n"
            "      \"swap\": 40,\n"
            "      \"ms\": 31\n"
            "    }\n"
            "  ],\n"
            "  \"names\": [\n"
            "    \"a\",\n"
            "    2.5\n"
            "  ],\n"
            "  \"divergence\": null,\n"
            "  \"big\": 18446744073709551615,\n"
            "  \"neg\": -5\n"
            "}\n";
        CHECK(text == expected, "document:\n%s", text.c_str());

        // Reading numbers back from a meta file.
        std::string meta = "{\"schema\": 1, \"note\": \"race_loop\", \"race_loop\" : 1234,\n \"end_loop\":-5, \"x\": {\"valid_until_loop\": 9.5e3}, \"s\": \"a\"}";
        auto race = JsonNumber(meta, "race_loop");
        CHECK(race && Near(*race, 1234), "race_loop (past the same text as a value): %g", race ? *race : -1.0);
        auto end = JsonNumber(meta, "end_loop");
        CHECK(end && Near(*end, -5), "end_loop: %g", end ? *end : 0.0);
        auto valid = JsonNumber(meta, "valid_until_loop");
        CHECK(valid && Near(*valid, 9500), "nested, exponent: %g", valid ? *valid : 0.0);
        CHECK(!JsonNumber(meta, "missing"), "missing key");
        CHECK(!JsonNumber(meta, "s"), "a string is not a number");
        CHECK(!JsonNumber(meta, "x"), "an object is not a number");
    }

    void CsvWriter()
    {
        Csv csv({ "swap", "t_us", "late" }, 2);
        csv.Row({ 1, 16667, 0 });
        csv.Row({ 2, -3, 1 });
        csv.Row({ 3 });              // padded
        csv.Row({ 4, 5, 6, 7 });     // cut
        const char* expected = "swap,t_us,late\n1,16667,0\n2,-3,1\n3,,\n4,5,6\n";
        CHECK(csv.Text() == expected, "csv:\n%s", csv.Text().c_str());
    }
}

int main()
{
    Percentiles();
    Lows();
    Summary();
    Fnv();
    JsonWriter();
    CsvWriter();
    if (g_failures)
    {
        printf("bench_stats_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("bench_stats_test: all passed\n");
    return 0;
}
