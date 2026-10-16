// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The pipeline key list (GetCachePath()/pipeline_keys.bin): the pipelines
// earlier sessions drew with, and the guest shaders they need, so that the
// renderer makes them in the background while the game starts instead of
// when a draw first needs one (renderer.cpp, "Pipeline prewarm").
// Vulkan-free: tests/pipeline_keys_test.cpp checks it alone; renderer.cpp
// keeps a Recorder, loads and saves it beside the pipeline cache and
// prewarms what it loaded.
//
// Why a list besides the pipeline cache: the driver's cache (pipelines.bin)
// makes creating a pipeline cheaper, it doesn't create it, so every session
// still created each pipeline when a draw first used it. In the Steam Deck's
// replayed race that was 19 of the session's ~90 pipelines while driving:
// each one's shaders translated on the command processor and the pipeline
// made on a worker while the command processor waited, a few tenths of a
// millisecond with a warm cache; with a cold one (a driver update, or a
// build whose shaders translate differently) 20-80 ms each, and the draws
// using it skipped meanwhile.
//
// A key is renderer.cpp's PipelineKey, kept as its bytes (the host's, every
// supported host is little-endian); its first 16 bytes are its vertex and
// pixel shaders' hashes. A shader is kept as the guest's microcode words, as
// loaded (big-endian, the hash's input), not as its translation: the
// translator may change between builds, and translating the words again
// gives exactly what this build makes (the SPIR-V has its own disk cache,
// keyed by source). The renderer's own depth-only pixel shader has no words
// and isn't kept (renderer.cpp knows its hash).
//
// The file, little-endian:
//    0 magic "SBPK"   4 format (kFormat)   8 key size (kKeyBytes)
//   12 vendorID      16 deviceID          20 sessions that saved it
//   24 shader count  28 key count
//   32 shaders: hash (8), type (4: 0 vertex, 1 pixel), dwords (4), words
//      keys: key (kKeyBytes), the last session that used it (4)
//      then the FNV-1a 64 of every byte before it (8)
// Keys are in the order they were first used (the boot's, the menus', a
// race's), which is the order they are made again. The formats in a key are
// this GPU's (a depth format another vendor lacks), so a list from another
// GPU is set aside; a driver update keeps it, and the pipelines are then
// compiled anew in the background, which is when making them early matters
// most. MoltenVK's device ID on an Apple GPU carries the macOS version
// (gpu/pipeline_cache_header.h): only its GPU part is compared.
//
// The list keeps at most kMaxKeys keys: past that, the ones used longest
// ago go (a session stamps every key it draws with), and with them the
// shaders no kept key needs. A race on the Deck uses ~90.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gpu::pipeline_keys
{
    constexpr uint32_t kMagic = 0x4B504253;  // "SBPK"
    // Bump when renderer.cpp's PipelineKey changes its layout or what a
    // field means: an older list is then set aside, not misread.
    constexpr uint32_t kFormat = 1;
    constexpr size_t kKeyBytes = 80;
    constexpr size_t kMaxKeys = 2048;
    constexpr uint32_t kMaxShaderDwords = 0xFFFF;  // IM_LOAD's size field
    constexpr size_t kHeaderBytes = 32;
    constexpr uint32_t kAppleVendor = 0x106B;

    using Key = std::array<uint8_t, kKeyBytes>;

    inline uint64_t ShaderOf(const Key& key, int stage)
    {
        uint64_t hash;
        std::memcpy(&hash, key.data() + 8 * stage, 8);
        return hash;
    }

    // A guest shader's identity, as the renderer's LoadShader computes it:
    // FNV-1a over its words as loaded (guest order), seeded with its type.
    inline uint64_t ShaderHash(uint32_t type, const uint32_t* words, size_t count)
    {
        uint64_t h = 0xCBF29CE484222325ull ^ type;
        for (size_t i = 0; i < count; i++)
            h = (h ^ words[i]) * 0x100000001B3ull;
        return h;
    }

    struct Shader
    {
        uint32_t type = 0;
        std::vector<uint32_t> words;  // guest order
    };

    struct Entry
    {
        Key key{};
        uint32_t lastSession = 0;
    };

    struct List
    {
        uint32_t vendorID = 0, deviceID = 0;
        uint32_t session = 0;          // sessions that saved it
        std::vector<Entry> keys;       // in first use order
        std::unordered_map<uint64_t, Shader> shaders;  // by hash
    };

    // One GPU, as far as its pipelines' formats go (a driver update isn't
    // another GPU). MoltenVK on an Apple GPU: OS major and minor in the
    // device ID's top 16 bits, the GPU families below.
    inline bool SameGpu(uint32_t vendorA, uint32_t deviceA, uint32_t vendorB, uint32_t deviceB)
    {
        if (vendorA != vendorB)
            return false;
        if (vendorA == kAppleVendor)
            return (deviceA & 0xFFFF) == (deviceB & 0xFFFF);
        return deviceA == deviceB;
    }

    inline uint64_t Fnv1a(const uint8_t* data, size_t size)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; i++)
            h = (h ^ data[i]) * 0x100000001B3ull;
        return h;
    }

    inline void PutLE32(std::vector<uint8_t>& out, uint32_t v)
    {
        for (int i = 0; i < 4; i++)
            out.push_back(uint8_t(v >> (8 * i)));
    }

    inline void PutLE64(std::vector<uint8_t>& out, uint64_t v)
    {
        for (int i = 0; i < 8; i++)
            out.push_back(uint8_t(v >> (8 * i)));
    }

    inline uint32_t GetLE32(const uint8_t* p)
    {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }

    inline uint64_t GetLE64(const uint8_t* p)
    {
        return uint64_t(GetLE32(p)) | uint64_t(GetLE32(p + 4)) << 32;
    }

    // The file's bytes for `list`, its shaders in hash order (so the same
    // list always gives the same bytes).
    inline std::vector<uint8_t> Encode(const List& list)
    {
        std::vector<uint64_t> hashes;
        hashes.reserve(list.shaders.size());
        for (const auto& [hash, shader] : list.shaders)
            hashes.push_back(hash);
        std::sort(hashes.begin(), hashes.end());
        std::vector<uint8_t> out;
        PutLE32(out, kMagic);
        PutLE32(out, kFormat);
        PutLE32(out, uint32_t(kKeyBytes));
        PutLE32(out, list.vendorID);
        PutLE32(out, list.deviceID);
        PutLE32(out, list.session);
        PutLE32(out, uint32_t(hashes.size()));
        PutLE32(out, uint32_t(list.keys.size()));
        for (uint64_t hash : hashes)
        {
            const Shader& s = list.shaders.at(hash);
            PutLE64(out, hash);
            PutLE32(out, s.type);
            PutLE32(out, uint32_t(s.words.size()));
            for (uint32_t w : s.words)
                PutLE32(out, w);
        }
        for (const Entry& e : list.keys)
        {
            out.insert(out.end(), e.key.begin(), e.key.end());
            PutLE32(out, e.lastSession);
        }
        PutLE64(out, Fnv1a(out.data(), out.size()));
        return out;
    }

    enum class Verdict : uint8_t
    {
        Empty,       // no file: nothing listed yet
        Usable,      // this GPU's
        OtherGpu,    // another GPU's: its formats may not exist here
        Unreadable,  // not a list of this format (another build's), cut short, or corrupt
    };

    struct Decoded
    {
        Verdict verdict = Verdict::Empty;
        List list;         // Usable only
        std::string why;   // OtherGpu and Unreadable: for the log
    };

    // Reads a file's bytes. Any count or size it holds is checked against
    // the bytes there before it is used, and the checksum covers them all.
    inline Decoded Decode(const uint8_t* data, size_t bytes, uint32_t vendorID, uint32_t deviceID)
    {
        Decoded d;
        if (bytes == 0)
            return d;
        d.verdict = Verdict::Unreadable;
        if (bytes < kHeaderBytes + 8)
        {
            d.why = std::to_string(bytes) + " bytes, shorter than its header";
            return d;
        }
        if (GetLE32(data) != kMagic)
        {
            d.why = "not a pipeline key list";
            return d;
        }
        if (GetLE32(data + 4) != kFormat || GetLE32(data + 8) != kKeyBytes)
        {
            d.why = "format " + std::to_string(GetLE32(data + 4)) + " with " + std::to_string(GetLE32(data + 8)) +
                "-byte keys, this build reads format " + std::to_string(kFormat);
            return d;
        }
        if (GetLE64(data + bytes - 8) != Fnv1a(data, bytes - 8))
        {
            d.why = "checksum mismatch";
            return d;
        }
        List& list = d.list;
        list.vendorID = GetLE32(data + 12);
        list.deviceID = GetLE32(data + 16);
        list.session = GetLE32(data + 20);
        const uint32_t shaderCount = GetLE32(data + 24), keyCount = GetLE32(data + 28);
        const size_t end = bytes - 8;
        size_t at = kHeaderBytes;
        for (uint32_t i = 0; i < shaderCount; i++)
        {
            if (end - at < 16)
            {
                d.why = "shader " + std::to_string(i) + " cut short";
                return d;
            }
            const uint64_t hash = GetLE64(data + at);
            Shader s;
            s.type = GetLE32(data + at + 8);
            const uint32_t dwords = GetLE32(data + at + 12);
            at += 16;
            if (s.type > 1 || dwords == 0 || dwords > kMaxShaderDwords || (end - at) / 4 < dwords)
            {
                d.why = "shader " + std::to_string(i) + " malformed";
                return d;
            }
            s.words.resize(dwords);
            for (uint32_t w = 0; w < dwords; w++)
                s.words[w] = GetLE32(data + at + 4 * size_t(w));
            at += 4 * size_t(dwords);
            list.shaders[hash] = std::move(s);
        }
        if ((end - at) / (kKeyBytes + 4) < keyCount || end - at != size_t(keyCount) * (kKeyBytes + 4))
        {
            d.why = std::to_string(keyCount) + " keys don't fit the file";
            return d;
        }
        list.keys.resize(keyCount);
        for (Entry& e : list.keys)
        {
            std::memcpy(e.key.data(), data + at, kKeyBytes);
            e.lastSession = GetLE32(data + at + kKeyBytes);
            at += kKeyBytes + 4;
        }
        if (!SameGpu(list.vendorID, list.deviceID, vendorID, deviceID))
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "vendor %04x device %08x, this GPU %04x %08x", list.vendorID, list.deviceID, vendorID,
                deviceID);
            d.why = buf;
            d.verdict = Verdict::OtherGpu;
            d.list = {};
            return d;
        }
        d.verdict = Verdict::Usable;
        return d;
    }

    // At most `maxKeys` keys, the most recently used (the earlier listed
    // first among equally recent ones), in their order; then only the
    // shaders a kept key names. The number of keys dropped.
    inline size_t Prune(List& list, size_t maxKeys)
    {
        size_t dropped = 0;
        if (list.keys.size() > maxKeys)
        {
            std::vector<size_t> order(list.keys.size());
            for (size_t i = 0; i < order.size(); i++)
                order[i] = i;
            std::stable_sort(order.begin(), order.end(),
                [&](size_t a, size_t b) { return list.keys[a].lastSession > list.keys[b].lastSession; });
            std::vector<bool> keep(list.keys.size(), false);
            for (size_t i = 0; i < maxKeys; i++)
                keep[order[i]] = true;
            std::vector<Entry> kept;
            kept.reserve(maxKeys);
            for (size_t i = 0; i < list.keys.size(); i++)
                if (keep[i])
                    kept.push_back(list.keys[i]);
            dropped = list.keys.size() - kept.size();
            list.keys = std::move(kept);
        }
        std::unordered_set<uint64_t> named;
        for (const Entry& e : list.keys)
        {
            named.insert(ShaderOf(e.key, 0));
            named.insert(ShaderOf(e.key, 1));
        }
        for (auto it = list.shaders.begin(); it != list.shaders.end();)
            it = named.count(it->first) ? std::next(it) : list.shaders.erase(it);
        return dropped;
    }

    struct KeyHash
    {
        size_t operator()(const Key& k) const { return size_t(Fnv1a(k.data(), k.size())); }
    };

    // The list as a session keeps it: what it loaded, then every key it
    // draws with, new ones at the end, each stamped with this session. Not
    // synchronized (renderer.cpp holds a mutex around it).
    class Recorder
    {
    public:
        // This session starts from `loaded` (empty without a usable file).
        void Start(List loaded)
        {
            list_ = std::move(loaded);
            session_ = list_.session + 1;
            index_.clear();
            for (size_t i = 0; i < list_.keys.size(); i++)
                index_.emplace(list_.keys[i].key, i);
            dirty_ = false;
        }

        const List& Get() const { return list_; }
        uint32_t Session() const { return session_; }
        bool Dirty() const { return dirty_; }
        bool Has(const Key& key) const { return index_.count(key) != 0; }

        // A key drawn with: stamped with this session, and added at the end
        // when the list doesn't have it (true then: the caller adds its
        // shaders' words).
        bool Note(const Key& key)
        {
            if (auto it = index_.find(key); it != index_.end())
            {
                Entry& e = list_.keys[it->second];
                if (e.lastSession != session_)
                {
                    e.lastSession = session_;
                    dirty_ = true;
                }
                return false;
            }
            index_.emplace(key, list_.keys.size());
            list_.keys.push_back({ key, session_ });
            dirty_ = true;
            return true;
        }

        // A shader a noted key names, unless listed already (or wordless:
        // the renderer's own).
        void NoteShader(uint64_t hash, uint32_t type, const std::vector<uint32_t>& words)
        {
            if (words.empty() || words.size() > kMaxShaderDwords || list_.shaders.count(hash))
                return;
            list_.shaders[hash] = { type, words };
            dirty_ = true;
        }

        // The file's bytes as of now: this session's number, at most
        // `maxKeys` keys (Prune); the list is then saved (not dirty).
        std::vector<uint8_t> Save(uint32_t vendorID, uint32_t deviceID, size_t maxKeys = kMaxKeys)
        {
            list_.vendorID = vendorID;
            list_.deviceID = deviceID;
            list_.session = session_;
            if (Prune(list_, maxKeys))
            {
                index_.clear();
                for (size_t i = 0; i < list_.keys.size(); i++)
                    index_.emplace(list_.keys[i].key, i);
            }
            dirty_ = false;
            return Encode(list_);
        }

        // The bytes Save gave couldn't be written: the next save tries again.
        void MarkUnsaved() { dirty_ = true; }

    private:
        List list_;
        std::unordered_map<Key, size_t, KeyHash> index_;
        uint32_t session_ = 1;
        bool dirty_ = false;
    };
}
