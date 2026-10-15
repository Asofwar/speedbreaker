#pragma once
// The timedemo's benchmark recorder (game/timedemo.h): during a replay it
// measures a window of the race frame by frame, writes a report
// (summary.json, frames.csv, main.csv, a [bench] log line) and ends the run.
//
//   NFSMW_BENCH_FROM=<world step>   the window's start after the race loaded (0)
//   NFSMW_BENCH_STEPS=<n>           its length in world steps (default: to the log's end)
//   NFSMW_BENCH_LABEL=<text>        the run's label in the report
//   NFSMW_BENCH_OUT=<dir>           where the report goes (default: <user>/logs/bench/<time>_<label>/)
//   NFSMW_BENCH_TIMEOUT=<s>         a partial report and exit after this long (900)
//   NFSMW_BENCH_EXIT=0|1            exit once the report is written (default 1 in replay)
//   NFSMW_BENCH=0                   a replay without the recorder (to watch one)
//
// Without NFSMW_BENCH_STEPS the window ends 60 frames before the capture's meta
// file (<file>.json) says the log stops being replayable (valid_until_loop,
// else end_loop); without a meta file, where the log ends. A window of
// NFSMW_BENCH_STEPS that reaches that point ends there, status log_ended.
#include <cstdint>

namespace bench
{
    // A replay with the recorder on.
    bool Enabled();
    // The window is being measured (Running) or finished on the command
    // processor's side (Draining): an uncapped replay drops the vblank only
    // then (game/movie_pacing.cpp).
    bool Measuring();

    // The game's main loop, once a frame, on the guest main thread
    // (game/frame_time.cpp's GameFrameTime hook).
    void OnMainFrame();
    // The command processor, once a swap, after its frame statistics
    // (gpu/command_processor.cpp, after HitchReport).
    void OnSwap();
    // The same with what only the command processor knows: the swap's
    // number (gpu::FrameCount() after it), when it was processed (guest ns,
    // the time the [perf] line's frame times use) and the draws the game has
    // issued so far (every DRAW_INDX, drawn or skipped: the same in every
    // run of a replay). OnSwap() reads the first two itself and takes the
    // renderer's draw count, which misses draws that never reached it.
    void OnSwap(uint64_t swap, int64_t swapNs, uint64_t draws);
    // The command processor's [perf] window (every 120 swaps): the device
    // memory in use (renderer::MemoryStats::deviceUsed), for the report's
    // peak. 0 (no VK_EXT_memory_budget) is ignored.
    void NoteDeviceMemory(uint64_t bytes);

    // The game's checksum found the replay out of step (game/timedemo.cpp).
    void Diverged(const char* kind, uint32_t loop, int64_t step);
    // The runtime's own UI interrupted the replay.
    void Interrupted();

    // main.cpp: the process exit code for a run with the recorder on
    // (0 ok, 2 setup/log ended/timeout, 3 diverged, 4 interrupted).
    // 0 with the recorder off, so main.cpp can always exit with it; a run
    // that quits before its report (the window closed) counts as interrupted.
    int ExitCode();
}
