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
//   NFSMW_BENCH_SHOTS=<step>,...    the picture of these world steps of the race
//                                   (counted as NFSMW_BENCH_FROM is) into the
//                                   report's folder as shot-<step>.ppm
//   NFSMW_BENCH=0                   a replay without the recorder (to watch one)
//
// Without NFSMW_BENCH_STEPS the window ends 60 frames before the capture's meta
// file (<file>.json) says the log stops being replayable (valid_until_loop,
// else end_loop); without a meta file, where the log ends. A window of
// NFSMW_BENCH_STEPS that reaches that point ends there, status log_ended.
//
// A shot is the front image (the renderer's picture of the frame, before the
// presenter's post-processing and scaling) of the swap that shows the race's
// first frame at or past its step: the same frame in every run of the replay,
// uncapped or not (debug/bench_shots.h says how that swap is found). Steps up
// to the window's end are taken; the report's "shots" lists them. A shot
// costs a copy of the front image on the GPU and a file written on another
// thread, nothing the game waits for. scripts/compare_shots.py compares two
// runs' shots.
#include <atomic>
#include <cstdint>

namespace bench
{
    // The next shot's swap number, 0 when none is due: the command processor
    // checks each swap against it (OnShotSwap).
    inline std::atomic<uint64_t> g_shotSwap{ 0 };
    // The command processor, at swap `swap` (gpu::FrameCount() after it)
    // once it reaches g_shotSwap, right after the swap's front image was
    // written: `gpuFront` when the renderer wrote one (not headless, not
    // NFSMW_DUMP_FRAMES). Copies it for every shot due by this swap.
    void OnShotSwap(uint64_t swap, bool gpuFront);

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
