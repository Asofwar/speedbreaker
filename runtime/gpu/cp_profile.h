// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// study/hle-renderer instrumentation (never merged): what the command
// processor executes, by packet kind, dumped with d3d_profile.h's D3DPROF_DUMP
// list (at those command processor frames: XE_SWAP count).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <debug/d3d_profile.h>

namespace cpprof
{
    struct Counters
    {
        uint64_t dwords = 0;                 // every dword executed (headers included)
        uint64_t ibDwords = 0;               // of which inside indirect buffers
        uint64_t ibs = 0;
        uint64_t t0Packets = 0, t0Regs = 0;  // type 0
        uint64_t t1Packets = 0;
        uint64_t t2Dwords = 0;
        uint64_t t0ByRange[6] = {};          // registers: 0x2000-23FF, 0x4000-47FF, 0x4800-48BF, 0x4900-4927, 0x0000-1FFF, other
        uint64_t t3Packets[128] = {}, t3Dwords[128] = {}, t3Skipped[128] = {};
        uint64_t setConstDwords[8] = {};     // SET_CONSTANT payload by type (0 ALU, 1 fetch, 2 bool, 3 loop, 4 registers)
        uint64_t loadAluDwords = 0;          // LOAD_ALU_CONSTANT: constants read from memory
        uint64_t draws = 0, drawsSkipped = 0;
    };
    inline Counters g_c;
    inline int g_ibDepth = 0;

    inline int RangeOf(uint32_t reg)
    {
        if (reg >= 0x2000 && reg < 0x2400) return 0;
        if (reg >= 0x4000 && reg < 0x4800) return 1;
        if (reg >= 0x4800 && reg < 0x48C0) return 2;
        if (reg >= 0x4900 && reg < 0x4928) return 3;
        if (reg < 0x2000) return 4;
        return 5;
    }

    inline void Dump(uint64_t frame)
    {
        static std::vector<uint64_t> dumps = [] {
            std::vector<uint64_t> d;
            if (const char* v = std::getenv("D3DPROF_DUMP"))
            {
                std::string s = v;
                size_t p = 0;
                while (p < s.size())
                {
                    size_t q = s.find(',', p);
                    if (q == std::string::npos) q = s.size();
                    d.push_back(strtoull(s.substr(p, q - p).c_str(), nullptr, 10));
                    p = q + 1;
                }
            }
            return d;
        }();
        if (std::find(dumps.begin(), dumps.end(), frame) == dumps.end())
            return;
        const char* path = std::getenv("D3DPROF_OUT");
        FILE* f = path ? fopen(path, "a") : stderr;
        if (!f)
            return;
        const Counters& c = g_c;
        fprintf(f, "# cpprof frame %llu\n", (unsigned long long)frame);
        fprintf(f, "dwords %llu ibDwords %llu ibs %llu t0Packets %llu t0Regs %llu t1Packets %llu t2Dwords %llu draws %llu drawsSkipped %llu loadAluDwords %llu\n",
            (unsigned long long)c.dwords, (unsigned long long)c.ibDwords, (unsigned long long)c.ibs, (unsigned long long)c.t0Packets,
            (unsigned long long)c.t0Regs, (unsigned long long)c.t1Packets, (unsigned long long)c.t2Dwords, (unsigned long long)c.draws,
            (unsigned long long)c.drawsSkipped, (unsigned long long)c.loadAluDwords);
        fprintf(f, "t0ByRange %llu %llu %llu %llu %llu %llu\n", (unsigned long long)c.t0ByRange[0], (unsigned long long)c.t0ByRange[1],
            (unsigned long long)c.t0ByRange[2], (unsigned long long)c.t0ByRange[3], (unsigned long long)c.t0ByRange[4], (unsigned long long)c.t0ByRange[5]);
        fprintf(f, "setConst %llu %llu %llu %llu %llu %llu %llu %llu\n", (unsigned long long)c.setConstDwords[0], (unsigned long long)c.setConstDwords[1],
            (unsigned long long)c.setConstDwords[2], (unsigned long long)c.setConstDwords[3], (unsigned long long)c.setConstDwords[4],
            (unsigned long long)c.setConstDwords[5], (unsigned long long)c.setConstDwords[6], (unsigned long long)c.setConstDwords[7]);
        for (int op = 0; op < 128; op++)
            if (c.t3Packets[op])
                fprintf(f, "t3 %02X packets %llu dwords %llu skipped %llu\n", op, (unsigned long long)c.t3Packets[op],
                    (unsigned long long)c.t3Dwords[op], (unsigned long long)c.t3Skipped[op]);
        fprintf(f, "# end\n");
        if (f != stderr)
            fclose(f);
    }
}
