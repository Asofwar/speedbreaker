// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See bench.h.
//
// The benchmark recorder's interface with the recorder off: a replay runs
// as a plain replay until the recorder lands here.
#include "bench.h"

namespace bench
{
    bool Enabled() { return false; }
    bool Measuring() { return false; }
    void OnMainFrame() {}
    void OnSwap() {}
    void OnSwap(uint64_t, int64_t, uint64_t) {}
    void NoteDeviceMemory(uint64_t) {}
    void Diverged(const char*, uint32_t, int64_t) {}
    void Interrupted() {}
    int ExitCode() { return 0; }
}
