// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "cpu_locks.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace cpulocks
{
    namespace
    {
        constexpr auto kLockedFor = std::chrono::seconds(2);
        std::mutex s_mutex;
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_locked;  // texture base (physical) -> last lock
    }

    void Note(uint8_t* base, uint32_t texture)
    {
        if (!texture)
            return;
        // A level whose type bits are 4 stands for its parent at +44 (as
        // sub_82589398 takes it; game/exposure_lock.cpp).
        if ((PPC_LOAD_U32(texture) & 0x00070000) == 0x00040000 && PPC_LOAD_U32(texture + 44))
            texture = PPC_LOAD_U32(texture + 44);
        // D3DBaseTexture: the fetch constant at +16; its second word holds
        // the base address (>> 12), in a physical window (A0000000 and up).
        const uint32_t physical = ((PPC_LOAD_U32(texture + 20) >> 12) << 12) & 0x1FFFFFFF;
        std::lock_guard lock(s_mutex);
        s_locked[physical] = std::chrono::steady_clock::now();
    }

    bool LockedLately(uint32_t address)
    {
        address &= 0x1FFFFFFF;
        std::lock_guard lock(s_mutex);
        auto it = s_locked.find(address);
        return it != s_locked.end() && std::chrono::steady_clock::now() - it->second < kLockedFor;
    }
}
