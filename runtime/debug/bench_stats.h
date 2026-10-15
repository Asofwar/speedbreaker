// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The benchmark recorder's arithmetic and file formats (debug/bench.h), apart
// from the game so tests/bench_stats_test.cpp can check them: no Vulkan, no
// SDL, standard headers only.
//
//   - Percentiles are nearest-rank (the smallest value with at least q% of
//     the values at or below it), as CapFrameX and PresentMon's tools give
//     them: always a frame that happened, never an interpolation between two.
//   - A "1% low" is the frame rate of the slowest 1% of frames taken
//     together (1000 / their mean time), the "1% low average" frame time
//     tools print, not the 99th percentile's rate (that is p99_fps). With
//     fewer than 10000 frames the 0.1% low rests on fewer than 10 frames: it
//     is reported, flagged unreliable.
//   - The trace hash is FNV-1a 64 over each frame's counters as fixed-width
//     little-endian bytes, so a run's hash is the same on every platform and
//     two runs of one replay agree exactly when every frame's counters do.
//   - JSON and CSV are written by hand: a few hundred lines of output don't
//     justify a library, and the escaping rules needed are short.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bench::stats
{
    // --- Distributions ---

    // The nearest-rank q-th percentile (0..100) of values sorted ascending:
    // the value at rank ceil(q/100 * n), counting from 1. 0 when empty. The
    // rank is taken a hair low before rounding up: 99.9 isn't exact in
    // binary, and 99.9/100 * 1000 comes out at 999.0000000000001, which
    // would round to the 1000th value instead of the 999th.
    inline double PercentileSorted(std::span<const double> sorted, double q)
    {
        if (sorted.empty())
            return 0.0;
        double rank = std::ceil(std::clamp(q, 0.0, 100.0) * double(sorted.size()) / 100.0 - 1e-9);
        size_t index = rank < 1.0 ? 0 : size_t(rank) - 1;
        return sorted[std::min(index, sorted.size() - 1)];
    }

    // How many frames the slowest `fraction` of n is: at least one, and
    // rounded up, so a "1% low" of 150 frames averages the slowest two.
    inline size_t SlowestCount(size_t n, double fraction)
    {
        if (n == 0)
            return 0;
        size_t count = size_t(std::ceil(double(n) * fraction - 1e-9));
        return std::clamp<size_t>(count, 1, n);
    }

    // The frame rate of the slowest `fraction` of frames together: 1000 over
    // the mean of the slowest frame times (ms, sorted ascending). 0 when empty.
    inline double LowAverageFps(std::span<const double> sortedMs, double fraction)
    {
        size_t count = SlowestCount(sortedMs.size(), fraction);
        if (count == 0)
            return 0.0;
        double sum = 0.0;
        for (size_t i = sortedMs.size() - count; i < sortedMs.size(); i++)
            sum += sortedMs[i];
        double mean = sum / double(count);
        return mean > 0.0 ? 1000.0 / mean : 0.0;
    }

    // Fewer frames than this make the 0.1% low fewer than 10 frames.
    inline constexpr size_t kReliableLow01Frames = 10000;

    // A series of frame times (ms), in the order they happened.
    struct Series
    {
        size_t n = 0;
        double p50 = 0, p95 = 0, p99 = 0, p999 = 0, max = 0;
        double mean = 0, stdev = 0;  // population standard deviation
        // The mean change from one frame to the next: how uneven the pacing
        // is, which neither the mean nor the percentiles show (16/17/16/17
        // and 16/16/17/17 have the same of both).
        double meanAbsDelta = 0;
        double low1Fps = 0, low01Fps = 0;
        bool low01Unreliable = true;
    };

    inline Series Summarize(std::span<const double> ms)
    {
        Series s;
        s.n = ms.size();
        if (ms.empty())
            return s;
        double sum = 0.0, deltas = 0.0;
        for (size_t i = 0; i < ms.size(); i++)
        {
            sum += ms[i];
            if (i > 0)
                deltas += std::abs(ms[i] - ms[i - 1]);
        }
        s.mean = sum / double(ms.size());
        double squares = 0.0;
        for (double v : ms)
            squares += (v - s.mean) * (v - s.mean);
        s.stdev = std::sqrt(squares / double(ms.size()));
        s.meanAbsDelta = ms.size() > 1 ? deltas / double(ms.size() - 1) : 0.0;
        std::vector<double> sorted(ms.begin(), ms.end());
        std::sort(sorted.begin(), sorted.end());
        s.p50 = PercentileSorted(sorted, 50.0);
        s.p95 = PercentileSorted(sorted, 95.0);
        s.p99 = PercentileSorted(sorted, 99.0);
        s.p999 = PercentileSorted(sorted, 99.9);
        s.max = sorted.back();
        s.low1Fps = LowAverageFps(sorted, 0.01);
        s.low01Fps = LowAverageFps(sorted, 0.001);
        s.low01Unreliable = sorted.size() < kReliableLow01Frames;
        return s;
    }

    // --- FNV-1a 64 ---

    inline constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
    inline constexpr uint64_t kFnvPrime = 0x00000100000001B3ull;

    inline uint64_t Fnv1a64(const void* data, size_t size, uint64_t hash = kFnvOffset)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++)
        {
            hash ^= bytes[i];
            hash *= kFnvPrime;
        }
        return hash;
    }

    // Integers folded in as fixed-width little-endian bytes, whatever the
    // host's byte order, so the same counters give the same hash anywhere.
    class Fnv64
    {
    public:
        void Add(uint32_t v)
        {
            for (int i = 0; i < 4; i++)
                Byte(uint8_t(v >> (8 * i)));
        }
        void Add(uint64_t v)
        {
            for (int i = 0; i < 8; i++)
                Byte(uint8_t(v >> (8 * i)));
        }
        void Add(int64_t v) { Add(uint64_t(v)); }
        uint64_t Value() const { return hash; }

    private:
        void Byte(uint8_t b)
        {
            hash ^= b;
            hash *= kFnvPrime;
        }
        uint64_t hash = kFnvOffset;
    };

    // 16 lower-case hex digits.
    inline std::string Hex64(uint64_t v)
    {
        char text[17];
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)v);
        return text;
    }

    // --- JSON ---

    // A JSON string's contents: quotes, backslashes and control characters
    // escaped; other bytes (UTF-8) as they are.
    inline std::string JsonEscape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + 8);
        for (char c : text)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char code[8];
                    snprintf(code, sizeof(code), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                    out += code;
                }
                else
                    out += c;
            }
        }
        return out;
    }

    // An indented JSON document written front to back: Begin/End nest
    // objects and arrays, Field writes "key": value in an object (Item, a
    // value in an array). Commas and indentation follow from the nesting.
    // Numbers are fixed-point with trailing zeros dropped; NaN and infinity,
    // which JSON can't hold, become null.
    class Json
    {
    public:
        Json() { BeginObject(); }

        Json& BeginObject(std::string_view key = {}) { return Open(key, '{'); }
        Json& BeginArray(std::string_view key = {}) { return Open(key, '['); }
        Json& End()
        {
            if (depth.empty())
                return *this;
            bool empty = depth.back().first;
            char close = depth.back().second ? ']' : '}';
            depth.pop_back();
            if (!empty)
                NewLine();
            text += close;
            return *this;
        }
        // The document, closed (any open object or array with it).
        std::string Finish()
        {
            while (!depth.empty())
                End();
            text += '\n';
            return text;
        }

        Json& Field(std::string_view key, std::string_view value) { Key(key); text += '"' + JsonEscape(value) + '"'; return *this; }
        Json& Field(std::string_view key, const char* value) { return value ? Field(key, std::string_view(value)) : Null(key); }
        Json& Field(std::string_view key, const std::string& value) { return Field(key, std::string_view(value)); }
        Json& Field(std::string_view key, bool value) { Key(key); text += value ? "true" : "false"; return *this; }
        // Any integer type (size_t is unsigned long on macOS and Linux alike,
        // but uint64_t is unsigned long long on macOS: fixed overloads would
        // be ambiguous there).
        template <typename T>
            requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
        Json& Field(std::string_view key, T value) { Key(key); text += std::to_string(value); return *this; }
        Json& Field(std::string_view key, double value, int decimals = 3) { Key(key); text += Number(value, decimals); return *this; }
        Json& Null(std::string_view key) { Key(key); text += "null"; return *this; }

        // Array items.
        Json& Item(std::string_view value) { Key({}); text += '"' + JsonEscape(value) + '"'; return *this; }
        Json& Item(double value, int decimals = 3) { Key({}); text += Number(value, decimals); return *this; }

        static std::string Number(double value, int decimals)
        {
            if (!std::isfinite(value))
                return "null";
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%.*f", std::clamp(decimals, 0, 9), value);
            std::string s = buffer;
            if (s.find('.') != std::string::npos)
            {
                while (s.back() == '0')
                    s.pop_back();
                if (s.back() == '.')
                    s.pop_back();
            }
            if (s == "-0")
                s = "0";
            return s;
        }

    private:
        Json& Open(std::string_view key, char open)
        {
            if (!depth.empty())
                Key(key);
            text += open;
            depth.push_back({ true, open == '[' });
            return *this;
        }
        // The separator before a value, and "key": inside an object.
        void Key(std::string_view key)
        {
            if (depth.empty())
                return;
            if (!depth.back().first)
                text += ',';
            depth.back().first = false;
            NewLine();
            if (!depth.back().second)
                text += '"' + JsonEscape(key) + "\": ";
        }
        void NewLine()
        {
            text += '\n';
            text.append(depth.size() * 2, ' ');
        }

        std::string text;
        std::vector<std::pair<bool, bool>> depth;  // per level: nothing in it yet, an array
    };

    // A number's value at "key" anywhere in a JSON document (the first one),
    // for the few fields read back from the timedemo's own meta file; no
    // general parser. nullopt when the key is missing or not a number.
    inline std::optional<double> JsonNumber(std::string_view json, std::string_view key)
    {
        std::string quoted = '"' + std::string(key) + '"';
        for (size_t at = json.find(quoted); at != std::string_view::npos; at = json.find(quoted, at + 1))
        {
            size_t i = at + quoted.size();
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
                i++;
            if (i >= json.size() || json[i] != ':')
                continue;  // the text as a value, not a key
            i++;
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
                i++;
            std::string number;
            while (i < json.size() && (std::isdigit(static_cast<unsigned char>(json[i])) || json[i] == '-' || json[i] == '+'
                || json[i] == '.' || json[i] == 'e' || json[i] == 'E'))
                number += json[i++];
            if (number.empty())
                return std::nullopt;
            char* end = nullptr;
            double value = std::strtod(number.c_str(), &end);
            if (end != number.c_str() + number.size())
                return std::nullopt;
            return value;
        }
        return std::nullopt;
    }

    // --- CSV ---

    // Integer columns only (every column the recorder writes is a count or
    // microseconds): a header line, then one line per row.
    class Csv
    {
    public:
        explicit Csv(std::initializer_list<std::string_view> columns, size_t rows = 0)
        {
            text.reserve((rows + 1) * columns.size() * 8);
            bool first = true;
            for (std::string_view c : columns)
            {
                if (!first)
                    text += ',';
                text += c;
                first = false;
            }
            text += '\n';
            width = columns.size();
        }
        // A row of exactly as many values as there are columns (short rows
        // are padded with empty fields, long ones cut, so the file stays
        // rectangular).
        void Row(std::initializer_list<int64_t> values)
        {
            size_t i = 0;
            for (int64_t v : values)
            {
                if (i == width)
                    break;
                if (i > 0)
                    text += ',';
                text += std::to_string(v);
                i++;
            }
            for (; i < width; i++)
                if (i > 0)
                    text += ',';
            text += '\n';
        }
        const std::string& Text() const { return text; }

    private:
        std::string text;
        size_t width = 0;
    };
}
