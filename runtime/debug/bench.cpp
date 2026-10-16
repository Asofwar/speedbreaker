// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See bench.h.
//
// The recorder goes through five states. Off until the game's main loop first
// runs in a replay; Armed while the replay drives boot, the front end and the
// race's loading; Running from the first frame of the loaded race (game flow
// 6) at or past NFSMW_BENCH_FROM world steps; Draining once the window has
// ended on the main thread, while the command processor works through the
// frames the game already handed it (it runs one to three behind); Done
// while the report is written and after.
//
// Two series, each written by one thread into an array sized and touched up
// front: no lock, allocation or file I/O while the window runs.
//   - main.csv, the game's main loop (OnMainFrame, from GameFrameTime: the
//     guest main thread at the start of each frame, before the world steps).
//     A row per loop iteration, timed from one call to the next, with the
//     time the iteration spent in the guest's Sleep: a replay waits for its
//     loads to finish where the capture's did (QUEUEDFILE polls Sleep(8)),
//     and that is streaming, not rendering, so it is a measure of its own.
//   - frames.csv, the command processor (OnSwap, right after HitchReport):
//     a row per swap, deltas since the last swap of the counters the [perf]
//     and [hitch] lines already keep.
// A row is stored before the count that publishes it (release), so the
// report's thread reads only whole rows, even when a timeout cuts the window
// while both threads are still writing.
//
// The window's ends are decided on the main thread, where the game's own
// counters are exact. The command processor's window is the swaps between
// the swap counts at those two moments: the same number of frames give or
// take its lag, the same frames shifted by it. Three swaps after the end the
// GPU's counters (accounted at completion, a frame or two late) have caught
// up, and the report is written on a thread of its own, "nfsmw-bench", so
// neither the game nor the command processor waits for files. Then the game
// quits the way a window close does (main.cpp saves the pipeline cache, so
// the next run starts warm), with a hard exit 15 s later if that hangs.
//
// The trace hash folds each main row's RealLoopCounter, WorldLoopCounter and
// 4000 Hz ticks (u32, i64, u32 as little-endian bytes into FNV-1a 64:
// bench_stats.h): in a replay these follow the log, not the host's speed, so
// two runs of one log hash the same exactly when they stayed in step frame
// for frame (the game's own checksum, timedemo.cpp's DIVERGENCE, says where
// they didn't).
//
// Shots (NFSMW_BENCH_SHOTS, bench_shots.h): at each frame's start the main
// thread notes the game's swaps against its frames, and when a shot's step
// comes names the swap that will be that frame's picture; the command
// processor copies that swap's front image as it executes it (OnShotSwap),
// and a thread of the renderer's writes the file once the copy has landed.
// Nothing waits for any of it but the report, for the files.
#include <stdafx.h>
#include "bench.h"
#include "bench_shots.h"
#include "bench_stats.h"

#include <cerrno>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <sys/resource.h>
#include <unistd.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>
#include <game/timedemo.h>
#include <gpu/command_processor.h>
#include <gpu/renderer.h>
#include <install/sha256.h>
#include <kernel/function.h>
#include <kernel/vfs.h>
#include <report/report.h>
#include <user/settings.h>
#include <video/presenter.h>
#include <video/vblank_lock.h>

extern char** environ;

namespace bench
{
    namespace
    {
        namespace renderer = gpu::renderer;

        // Guest addresses, the supported default.xex (USA) only, as in
        // game/timedemo.h.
        constexpr uint32_t kReplayOn = 0x82A2CC84;  // be32: Joylog replay; the game clears it at the log's end or a divergence
        constexpr uint32_t kTicks = 0x82A399AC;     // be32: the 4000 Hz real-time ticks (AdvanceRealTime)
        constexpr uint32_t kRaceFlow = 6;           // GameFlowState once a race has loaded

        constexpr uint64_t kDrainSwaps = 3;
        // Frames the default window keeps clear of where the log stops being
        // replayable (the design's valid_until_loop - 60).
        constexpr int64_t kLimitMargin = 60;
        // Rows reserved when the window's length is unknown (no
        // NFSMW_BENCH_STEPS and no meta file), and the most ever: 80 + 32
        // bytes a frame for the two series, so at most 59 MB, for a window
        // of almost two and a half hours at 60 fps.
        constexpr size_t kUnknownRows = 131072, kMinRows = 4096, kMaxRows = 524288;

        uint32_t ReadGuest32(uint32_t address)
        {
            return ByteSwap(*static_cast<const uint32_t*>(g_memory.Translate(address)));
        }

        enum class State : uint8_t { Off, Armed, Running, Draining, Done };
        enum class Status : uint8_t { None, Ok, LogEnded, Timeout, Diverged, Interrupted };
        // Why the window ended.
        enum class End : uint8_t { None, Steps, Limit, Flow, LogEnded, Diverged, Capacity, Timeout };

        const char* StatusName(Status status)
        {
            switch (status)
            {
            case Status::Ok: return "ok";
            case Status::LogEnded: return "log_ended";
            case Status::Timeout: return "timeout";
            case Status::Diverged: return "diverged";
            case Status::Interrupted: return "interrupted";
            default: return "none";
            }
        }

        int StatusCode(Status status)
        {
            switch (status)
            {
            case Status::Ok: return 0;
            case Status::LogEnded:
            case Status::Timeout: return 2;
            case Status::Diverged: return 3;
            default: return 4;  // interrupted, or quit before the report
            }
        }

        const char* EndName(End end)
        {
            switch (end)
            {
            case End::Steps: return "steps";            // NFSMW_BENCH_STEPS reached
            case End::Limit: return "limit";            // the meta's replayable end
            case End::Flow: return "flow";              // the race left (game flow no longer 6)
            case End::LogEnded: return "log_ended";     // the game dropped the replay without a divergence
            case End::Diverged: return "diverged";
            case End::Capacity: return "capacity";      // the reserved rows ran out
            case End::Timeout: return "timeout";
            default: return "none";
            }
        }

        // NFSMW_BENCH_* (bench.h), read once.
        struct Config
        {
            int64_t from = 0;
            int64_t steps = -1;  // -1: to the meta's limit, or the log's end
            double timeoutS = 900;
            bool exit = true;
            std::string label;
            std::filesystem::path out;  // empty: the default under the logs
            std::vector<int64_t> shots; // NFSMW_BENCH_SHOTS, ascending
        };

        bool ParseInt(const char* text, int64_t& value)
        {
            if (!text || !*text)
                return false;
            char* end = nullptr;
            long long v = std::strtoll(text, &end, 10);
            if (*end != '\0' || v < 0)
                return false;
            value = v;
            return true;
        }

        Config ReadConfig()
        {
            Config c;
            if (const char* v = std::getenv("NFSMW_BENCH_FROM"); v && !ParseInt(v, c.from))
                fprintf(stderr, "[bench] NFSMW_BENCH_FROM=%s isn't a step count: from step 0\n", v);
            if (const char* v = std::getenv("NFSMW_BENCH_STEPS"); v && !ParseInt(v, c.steps))
            {
                fprintf(stderr, "[bench] NFSMW_BENCH_STEPS=%s isn't a step count: to the log's end\n", v);
                c.steps = -1;
            }
            if (const char* v = std::getenv("NFSMW_BENCH_TIMEOUT"))
            {
                char* end = nullptr;
                double s = std::strtod(v, &end);
                if (*v && *end == '\0' && s >= 0)
                    c.timeoutS = s;
                else
                    fprintf(stderr, "[bench] NFSMW_BENCH_TIMEOUT=%s isn't seconds: %.0f s\n", v, c.timeoutS);
            }
            if (const char* v = std::getenv("NFSMW_BENCH_EXIT"))
                c.exit = v[0] != '0';
            if (const char* v = std::getenv("NFSMW_BENCH_LABEL"); v && *v)
                c.label = v;
            else
                c.label = timedemo::File().stem().string();  // the race's file: "race"
            if (c.label.empty())
                c.label = "bench";
            if (const char* v = std::getenv("NFSMW_BENCH_OUT"); v && *v)
                c.out = v;
            if (const char* v = std::getenv("NFSMW_BENCH_SHOTS"); v && *v)
            {
                std::string bad;
                c.shots = shots::ParseSteps(v, &bad);
                if (!bad.empty())
                    fprintf(stderr, "[bench] NFSMW_BENCH_SHOTS: %s isn't a step count: left out\n", bad.c_str());
            }
            return c;
        }

        const Config& GetConfig()
        {
            static const Config config = ReadConfig();
            return config;
        }

        // What the capture wrote next to its log (<file>.json, timedemo.cpp's
        // FinishCapture): the loop the race loaded at, where the log ends and
        // where it stops being replayable (the runtime's own UI paused the
        // game there, which the log doesn't hold). -1: not there.
        struct Meta
        {
            bool found = false;
            int64_t raceLoop = -1, endLoop = -1, validUntilLoop = -1;
        };

        Meta ReadMeta(std::filesystem::path path)
        {
            Meta meta;
            path += ".json";
            FILE* f = fopen(path.c_str(), "rb");
            if (!f)
                return meta;
            std::string text;
            char buffer[4096];
            for (size_t n; (n = fread(buffer, 1, sizeof(buffer), f)) > 0;)
                text.append(buffer, n);
            fclose(f);
            meta.found = true;
            auto number = [&](const char* key) {
                auto v = stats::JsonNumber(text, key);
                return v ? int64_t(*v) : int64_t(-1);
            };
            meta.raceLoop = number("race_loop");
            meta.endLoop = number("end_loop");
            meta.validUntilLoop = number("valid_until_loop");
            return meta;
        }

        // The rows. Times are guest ns turned to us: the clock the [perf]
        // line uses, which stands still while the game is suspended (iOS).
        struct MainRow
        {
            int64_t tUs;       // since the window began
            uint32_t loop;     // RealLoopCounter
            int32_t step;      // WorldLoopCounter
            uint32_t frameUs;  // since the last call: this loop iteration
            uint32_t waitUs;   // of it in the guest's Sleep
            uint32_t flow;     // GameFlowState
            uint32_t ticks;    // the 4000 Hz ticks
        };

        struct SwapRow
        {
            uint64_t swap;
            int64_t tUs;       // since the window began (the main thread's start)
            uint32_t frameUs;  // swap to swap
            uint32_t draws;    // DRAW_INDX packets
            uint32_t busyUs;   // renderer::g_cpBusyNs
            uint32_t waitUs[renderer::CP_WAIT_KINDS];
            uint32_t gpuBusyUs, gpuExecUs;  // accounted at completion: a frame or two late
            uint32_t loop;     // the main loop's RealLoopCounter at the swap (for the worst frames)
            uint16_t late;     // HitchReport's verdict on this swap
            uint16_t pipelines, pipelineSkips, dropped, repeated;
        };

        template <typename Row>
        struct Rows
        {
            std::unique_ptr<Row[]> rows;
            size_t capacity = 0;
            std::atomic<size_t> count{ 0 };

            // One writer thread.
            bool Append(const Row& row)
            {
                size_t n = count.load(std::memory_order_relaxed);
                if (n >= capacity)
                    return false;
                rows[n] = row;
                count.store(n + 1, std::memory_order_release);
                return true;
            }
            bool Full() const { return count.load(std::memory_order_relaxed) >= capacity; }
        };

        std::atomic<State> s_state{ State::Off };
        Rows<MainRow> s_main;
        Rows<SwapRow> s_swaps;

        // Set on the main thread before the state store that publishes them
        // (release); read after loading the state (acquire).
        std::atomic<int64_t> s_t0{ 0 };                 // guest ns at the window's start
        std::atomic<uint64_t> s_firstSwap{ 0 };          // the window's swaps are after this one...
        std::atomic<uint64_t> s_lastSwap{ UINT64_MAX };  // ...up to this one (once Draining)
        std::atomic<int64_t> s_startStep{ -1 }, s_raceLoop{ -1 };
        std::atomic<End> s_end{ End::None };
        std::atomic<bool> s_raceLoopMismatch{ false };
        std::atomic<uint32_t> s_loopNow{ 0 };  // the main loop's latest RealLoopCounter, for the swap rows
        Meta s_meta;                           // Arm: before the Armed store
        int64_t s_untilLoop = -1;              // likewise: where the log stops being replayable
        int64_t s_limitLoop = -1;              // likewise: where the default window ends
        std::filesystem::path s_outDir;        // likewise
        std::chrono::steady_clock::time_point s_deadline;

        // Main thread only.
        thread_local bool t_mainThread = false;
        int64_t s_prevNs = 0;   // the last OnMainFrame
        int64_t s_waitNs = 0;   // in the guest's Sleep since then (sub_82812D38)
        bool s_replaySeen = false;

        // Events from other threads.
        std::atomic<bool> s_diverged{ false }, s_interrupted{ false }, s_logEnded{ false }, s_timedOut{ false };
        std::mutex s_divergenceMutex;
        struct Divergence
        {
            std::string kind;
            uint32_t loop = 0;
            int64_t step = -1;
        } s_divergence;

        std::atomic<uint64_t> s_devicePeak{ 0 };  // command processor
        std::atomic<Status> s_status{ Status::None };

        // The report thread's wake-up.
        std::mutex s_reportMutex;
        std::condition_variable s_reportReady;
        bool s_reportRequested = false;

        // Command processor only: the counters at the last swap.
        struct CpTotals
        {
            int64_t ns = 0;
            uint64_t draws = 0, busyNs = 0, waitNs[renderer::CP_WAIT_KINDS] = {};
            uint64_t gpuBusyUs = 0, gpuExecUs = 0, late = 0, pipelines = 0, pipelineSkips = 0, dropped = 0, repeated = 0;
            bool valid = false;
        };
        CpTotals s_cpLast;

        // Shots (see the top and bench_shots.h). One per NFSMW_BENCH_SHOTS
        // step, made at Arm; then each changes under s_shotMutex.
        struct Shot
        {
            enum class State : uint8_t { Waiting, Queued, Copying, Written, Failed };
            int64_t step = 0;          // asked for
            uint32_t loop = 0;         // the race's first frame at or past it (RealLoop at its start)
            int64_t frameStep = -1;    // and its world step then
            uint64_t swap = 0;         // its picture's swap
            uint64_t copied = 0;       // the swap the command processor copied (late: past `swap`)
            uint32_t width = 0, height = 0;
            bool ahead = false;        // the game had already made `swap` when it was named: the model failed
            State state = State::Waiting;
            std::string why;           // Failed: why
        };
        std::mutex s_shotMutex;
        std::condition_variable s_shotChanged;  // a shot was written or failed
        std::vector<Shot> s_shots;
        std::deque<size_t> s_shotQueue;         // Queued shots, by swap (frames come in order)
        shots::SwapOffset s_swapOffset;         // the main thread's, under the mutex (the report reads it)
        size_t s_shotNext = 0;                  // main thread: the first shot whose step hasn't come

        uint32_t Clamp32(int64_t v)
        {
            return uint32_t(std::clamp<int64_t>(v, 0, int64_t(UINT32_MAX)));
        }

        uint16_t Clamp16(uint64_t v)
        {
            return uint16_t(std::min<uint64_t>(v, UINT16_MAX));
        }

        // "2026-10-15_21-04-33", as the session log's name (report::SessionName,
        // which is empty without a session log).
        std::string TimeStamp()
        {
            if (const char* session = report::SessionName(); session && *session)
                return session;
            std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_r(&now, &local);
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &local);
            return text;
        }

        // A label as part of a folder name: letters, digits, '.', '-', '_'.
        std::string FolderSafe(std::string_view label)
        {
            std::string out;
            for (char c : label)
                out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
            return out;
        }

        void ReportThread();

        // The main loop's first frame in a replay: what the window will be,
        // the rows reserved (zeroed now, so the window never faults a page in),
        // and the report thread, waiting.
        void Arm()
        {
            const Config& config = GetConfig();
            s_meta = ReadMeta(timedemo::File());
            int64_t until = s_meta.validUntilLoop > 0 ? s_meta.validUntilLoop : s_meta.endLoop;
            s_untilLoop = until > 0 ? until : -1;
            s_limitLoop = until > 0 ? std::max<int64_t>(until - kLimitMargin, 0) : -1;
            int64_t frames = config.steps >= 0 ? config.steps
                : (s_limitLoop >= 0 && s_meta.raceLoop >= 0 ? s_limitLoop - s_meta.raceLoop - config.from : -1);
            size_t rows = frames >= 0 ? size_t(frames) * 2 + kMinRows : kUnknownRows;
            rows = std::clamp(rows, kMinRows, kMaxRows);
            s_main.rows.reset(new MainRow[rows]());
            s_main.capacity = rows;
            s_swaps.rows.reset(new SwapRow[rows]());
            s_swaps.capacity = rows;
            s_outDir = !config.out.empty() ? config.out
                : report::LogDirectory() / "bench" / (TimeStamp() + "_" + FolderSafe(config.label));

            std::string window = config.steps >= 0 ? std::format("{} steps", config.steps)
                : s_limitLoop >= 0 ? std::format("to loop {} (the meta's {} less {})", s_limitLoop, until, kLimitMargin)
                : std::string("to the log's end (no meta file)");
            fprintf(stderr, "[bench] armed \"%s\": from step %lld of the race, %s; %zu rows reserved; report to %s%s\n",
                config.label.c_str(), (long long)config.from, window.c_str(), rows, s_outDir.c_str(),
                config.timeoutS > 0 ? std::format("; timeout {:.0f} s", config.timeoutS).c_str() : "; no timeout");
            if (!s_meta.found)
                fprintf(stderr, "[bench] no meta file next to %s: race_loop isn't checked\n", timedemo::File().c_str());

            if (!config.shots.empty())
            {
                s_shots.resize(config.shots.size());
                std::string steps;
                for (size_t i = 0; i < config.shots.size(); i++)
                {
                    s_shots[i].step = config.shots[i];
                    steps += (i ? "," : "") + std::to_string(config.shots[i]);
                }
                fprintf(stderr, "[bench] shots at steps %s, into %s\n", steps.c_str(), s_outDir.c_str());
            }

            s_deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(int64_t(std::min(config.timeoutS, 1e9) * 1000.0));
            s_state.store(State::Armed, std::memory_order_release);
            std::thread(ReportThread).detach();
        }

        // --- Shots: the main thread's side ---

        // Main thread, Armed or Running (a replay's frames up to the window's
        // end), at the start of each race frame: the game's swaps against its
        // frames noted (bench_shots.h), and each shot whose step has come
        // named by its swap and queued for the command processor. The first
        // shot keeps the offset. Nothing here waits.
        void ShotsOnMainFrame(uint32_t loop, int64_t step, uint32_t flow)
        {
            if (flow != kRaceFlow || step < 0)
                return;
            const uint64_t swaps = gpu::g_guestSwaps.load(std::memory_order_relaxed);
            std::lock_guard lock(s_shotMutex);
            s_swapOffset.Note(swaps, loop);
            while (s_shotNext < s_shots.size() && step >= s_shots[s_shotNext].step)
            {
                if (!s_swapOffset.Frozen())
                {
                    s_swapOffset.Freeze();
                    fprintf(stderr, "[bench] shots: the game's swaps run %lld ahead of its frames (at %llu of the race's %llu "
                        "frames so far)\n", (long long)s_swapOffset.Offset(), (unsigned long long)s_swapOffset.CaughtUp(),
                        (unsigned long long)s_swapOffset.Frames());
                }
                const uint64_t swap = shots::PictureSwap(s_swapOffset.Offset(), loop);
                const uint64_t executed = gpu::FrameCount();
                Shot& shot = s_shots[s_shotNext++];
                shot.loop = loop;
                shot.frameStep = step;
                shot.swap = swap;
                // At this frame's start the game can't have made its picture
                // yet: it did only if the offset is wrong.
                shot.ahead = swaps >= swap;
                if (executed >= swap)
                {
                    shot.state = Shot::State::Failed;
                    shot.why = std::format("its swap {} had been executed already (at {})", swap, executed);
                    fprintf(stderr, "[bench] shot %lld: %s\n", (long long)shot.step, shot.why.c_str());
                    s_shotChanged.notify_all();
                    continue;
                }
                shot.state = Shot::State::Queued;
                s_shotQueue.push_back(size_t(&shot - s_shots.data()));
                g_shotSwap.store(s_shots[s_shotQueue.front()].swap, std::memory_order_relaxed);
            }
        }

        // Writes a copied picture (null: none) into each due shot's file;
        // a renderer thread (ReadBackFrontImage).
        void WriteShots(const std::vector<size_t>& due, const uint8_t* rgba, uint32_t width, uint32_t height)
        {
            std::string ppm, error;
            if (!rgba)
                error = "no front image to copy (headless, NFSMW_DUMP_FRAMES, or no memory for the copy)";
            else
            {
                ppm = shots::Ppm(rgba, width, height, size_t(width) * 4);
                std::error_code ec;
                std::filesystem::create_directories(s_outDir, ec);
                if (ec)
                    error = std::format("can't create {}: {}", s_outDir.string(), ec.message());
            }
            for (size_t i : due)
            {
                int64_t step;
                {
                    std::lock_guard lock(s_shotMutex);
                    step = s_shots[i].step;
                }
                std::filesystem::path path = s_outDir / shots::FileName(step);
                std::string failed = !error.empty() ? error : vfs::WriteAtomically(path, ppm.data(), ppm.size());
                std::lock_guard lock(s_shotMutex);
                Shot& shot = s_shots[i];
                shot.width = width;
                shot.height = height;
                shot.state = failed.empty() ? Shot::State::Written : Shot::State::Failed;
                shot.why = failed;
                if (failed.empty())
                    fprintf(stderr, "[bench] shot %lld: frame at loop %u (step %lld), swap %llu: %s (%ux%u)\n", (long long)step, shot.loop,
                        (long long)shot.frameStep, (unsigned long long)shot.swap, path.c_str(), width, height);
                else
                    fprintf(stderr, "[bench] shot %lld: %s\n", (long long)step, failed.c_str());
            }
            s_shotChanged.notify_all();
        }

        // The report thread: waits (10 s at most) for every named shot to be
        // written or failed, so the report says what is on disk. The game
        // goes on meanwhile: a shot named in the window's last frames has its
        // swap a few frames later.
        void WaitForShots()
        {
            if (s_shots.empty())
                return;
            std::unique_lock lock(s_shotMutex);
            bool done = s_shotChanged.wait_for(lock, std::chrono::seconds(10), [] {
                for (const Shot& shot : s_shots)
                    if (shot.state == Shot::State::Queued || shot.state == Shot::State::Copying)
                        return false;
                return true;
            });
            if (!done)
                fprintf(stderr, "[bench] shots: still copying after 10 s: reported as pending\n");
        }

        // Main thread: from one state to the next, unless the timeout got
        // there first (Done: the report thread has stopped the writers).
        bool Advance(State from, State to)
        {
            return s_state.compare_exchange_strong(from, to, std::memory_order_acq_rel);
        }

        // Main thread: the window is over; the command processor finishes it.
        void EndWindow(State state, End end, uint32_t loop, int64_t step)
        {
            uint64_t swap = gpu::FrameCount();
            if (state == State::Armed)
                s_firstSwap.store(swap, std::memory_order_relaxed);  // an empty window
            s_lastSwap.store(swap, std::memory_order_relaxed);
            s_end.store(end, std::memory_order_relaxed);
            if (!Advance(state, State::Draining))
                return;
            fprintf(stderr, "[bench] window ended (%s) at loop %u, step %lld, swap %llu: %zu frames\n", EndName(end), loop,
                (long long)step, (unsigned long long)swap, s_main.count.load(std::memory_order_relaxed));
        }

        // Command processor: the drain is over.
        void RequestReport()
        {
            State expected = State::Draining;
            if (!s_state.compare_exchange_strong(expected, State::Done, std::memory_order_acq_rel))
                return;
            {
                std::lock_guard lock(s_reportMutex);
                s_reportRequested = true;
            }
            s_reportReady.notify_one();
        }

        CpTotals TakeCpTotals(int64_t swapNs, uint64_t draws)
        {
            renderer::Stats st = renderer::GetStats();
            video::PresentStats shown = video::GetPresentStats();
            CpTotals t;
            t.ns = swapNs;
            t.draws = draws;
            t.busyNs = renderer::g_cpBusyNs;
            std::copy(std::begin(renderer::g_cpWaitKindNs), std::end(renderer::g_cpWaitKindNs), t.waitNs);
            t.gpuBusyUs = st.gpuBusyUs;
            t.gpuExecUs = st.gpuExecUs;
            t.late = st.lateFrames;
            t.pipelines = st.pipelines;
            t.pipelineSkips = st.pipelineSkips;
            t.dropped = shown.dropped;
            t.repeated = shown.repeated;
            t.valid = true;
            return t;
        }

        // --- The report (the nfsmw-bench thread) ---

        bool WriteFile(const std::filesystem::path& path, const std::string& text)
        {
            FILE* f = fopen(path.c_str(), "wb");
            if (!f)
            {
                fprintf(stderr, "[bench] can't write %s: %s\n", path.c_str(), strerror(errno));
                return false;
            }
            bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
            ok = fclose(f) == 0 && ok;
            if (!ok)
                fprintf(stderr, "[bench] writing %s failed: %s\n", path.c_str(), strerror(errno));
            return ok;
        }

        std::string FileSha256(const std::filesystem::path& path)
        {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f)
                return {};
            install::Sha256 sha;
            std::vector<uint8_t> buffer(1 << 20);
            for (size_t n; (n = fread(buffer.data(), 1, buffer.size(), f)) > 0;)
                sha.Update(buffer.data(), n);
            fclose(f);
            return install::ToHex(sha.Finish());
        }

        // The system summary's one-line items ("OS: ...", "GPU: ..."), by key.
        std::map<std::string, std::string> SystemItems()
        {
            std::map<std::string, std::string> items;
            std::string text = report::SystemSummary();
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find('\n', start);
                std::string_view line(text.data() + start, (end == std::string::npos ? text.size() : end) - start);
                start = end == std::string::npos ? text.size() : end + 1;
                size_t colon = line.find(": ");
                if (colon != std::string_view::npos && colon > 0 && line[0] != ' ')
                    items.emplace(std::string(line.substr(0, colon)), std::string(line.substr(colon + 2)));
            }
            return items;
        }

        void Distribution(stats::Json& json, const char* key, const stats::Series& s)
        {
            json.BeginObject(key)
                .Field("p50", s.p50).Field("p95", s.p95).Field("p99", s.p99).Field("p999", s.p999).Field("max", s.max)
                .Field("mean", s.mean).Field("stdev", s.stdev).Field("mean_abs_delta", s.meanAbsDelta)
                .End();
        }

        Status FinalStatus()
        {
            if (s_diverged.load() || s_raceLoopMismatch.load())
                return Status::Diverged;
            if (s_interrupted.load())
                return Status::Interrupted;
            if (s_timedOut.load())
                return Status::Timeout;
            if (s_logEnded.load())
                return Status::LogEnded;
            return Status::Ok;
        }

        void Quit(int code);

        // The report's "shots": the game's swaps against its frames
        // (bench_shots.h) and each shot, with its status: ok (written, and
        // verified: no race frame after the first shot went past the
        // offset), unverified (written, but not that, or the game was past
        // its swap when it was named), failed (with why), pending (still
        // being copied), not_reached (the window ended before its step).
        // Returns the [bench] line's summary.
        std::string ShotsJson(stats::Json& json)
        {
            std::lock_guard lock(s_shotMutex);
            const shots::SwapOffset& o = s_swapOffset;
            const bool verified = o.Verified();
            json.BeginObject("shots").Field("verified", verified);
            if (o.Frozen())
                json.Field("swap_offset", o.Offset());
            else
                json.Null("swap_offset");
            json.Field("race_frames", o.Frames()).Field("frames_at_offset", o.CaughtUp()).Field("frames_past_offset", o.Past());
            if (o.Past())
                json.Field("past_by", o.PastBy());
            json.BeginArray("taken");
            size_t ok = 0, unverified = 0, failed = 0, other = 0;
            for (const Shot& shot : s_shots)
            {
                const char* status = "not_reached";
                switch (shot.state)
                {
                case Shot::State::Written:
                    status = verified && !shot.ahead ? "ok" : "unverified";
                    break;
                case Shot::State::Failed: status = "failed"; break;
                case Shot::State::Queued:
                case Shot::State::Copying: status = "pending"; break;
                default: break;
                }
                ok += strcmp(status, "ok") == 0;
                unverified += strcmp(status, "unverified") == 0;
                failed += strcmp(status, "failed") == 0;
                other += strcmp(status, "pending") == 0 || strcmp(status, "not_reached") == 0;
                json.BeginObject().Field("step", shot.step).Field("status", status);
                if (shot.state != Shot::State::Waiting)
                    json.Field("loop", shot.loop).Field("frame_step", shot.frameStep).Field("swap", shot.swap);
                if (shot.state == Shot::State::Written)
                    json.Field("file", shots::FileName(shot.step)).Field("width", shot.width).Field("height", shot.height);
                if (shot.ahead)
                    json.Field("ahead", true);
                if (!shot.why.empty())
                    json.Field("why", shot.why);
                json.End();
            }
            json.End().End();
            return std::format("{} ok, {} unverified, {} failed, {} not taken", ok, unverified, failed, other);
        }

        void WriteReport()
        {
            const Config& config = GetConfig();
            // The shots' files first: the report names what is on disk.
            WaitForShots();
            // Whole rows only (see the top), and the command processor's up
            // to the window's last swap: one that raced the end is dropped.
            size_t mainCount = s_main.count.load(std::memory_order_acquire);
            size_t swapCount = s_swaps.count.load(std::memory_order_acquire);
            uint64_t lastSwap = s_lastSwap.load(std::memory_order_relaxed);
            while (swapCount > 0 && s_swaps.rows[swapCount - 1].swap > lastSwap)
                swapCount--;
            std::span<const MainRow> mains(s_main.rows.get(), mainCount);
            std::span<const SwapRow> swaps(s_swaps.rows.get(), swapCount);
            End end = s_end.load(std::memory_order_relaxed);
            if (s_timedOut.load() && end == End::None)
                end = End::Timeout;
            Status status = FinalStatus();
            int64_t startStep = s_startStep.load();
            int64_t raceLoop = s_raceLoop.load();

            // The command processor's frames.
            std::vector<double> frameMs, gameMs;
            frameMs.reserve(swaps.size());
            double swapSeconds = 0, busyMs = 0, idleMs = 0, gpuBusyMs = 0, gpuExecMs = 0;
            double waitMs[renderer::CP_WAIT_KINDS] = {};
            uint64_t late = 0, draws = 0, pipelines = 0, pipelineSkips = 0, dropped = 0, repeated = 0;
            for (const SwapRow& r : swaps)
            {
                double ms = double(r.frameUs) / 1000.0;
                frameMs.push_back(ms);
                swapSeconds += double(r.frameUs) / 1e6;
                busyMs += double(r.busyUs) / 1000.0;
                double frameWaitMs = 0;
                for (uint32_t k = 0; k < renderer::CP_WAIT_KINDS; k++)
                {
                    waitMs[k] += double(r.waitUs[k]) / 1000.0;
                    if (k != renderer::CP_WAIT_SUSPENDED)
                        frameWaitMs += double(r.waitUs[k]) / 1000.0;
                }
                // Idle: the ring was empty, the processor waited for the game
                // (as the [hitch] line works it out).
                idleMs += std::max(0.0, ms - double(r.busyUs) / 1000.0 - frameWaitMs);
                gpuBusyMs += double(r.gpuBusyUs) / 1000.0;
                gpuExecMs += double(r.gpuExecUs) / 1000.0;
                late += r.late;
                draws += r.draws;
                pipelines += r.pipelines;
                pipelineSkips += r.pipelineSkips;
                dropped += r.dropped;
                repeated += r.repeated;
            }
            stats::Series frames = stats::Summarize(frameMs);
            double perSwap = swaps.empty() ? 0.0 : 1.0 / double(swaps.size());
            uint64_t over2x = 0, over50 = 0;
            for (double ms : frameMs)
            {
                over2x += ms > 2.0 * frames.p50;
                over50 += ms >= 50.0;
            }
            std::vector<size_t> worst(swaps.size());
            for (size_t i = 0; i < worst.size(); i++)
                worst[i] = i;
            size_t worstCount = std::min<size_t>(worst.size(), 10);
            std::partial_sort(worst.begin(), worst.begin() + worstCount, worst.end(),
                [&](size_t a, size_t b) { return swaps[a].frameUs > swaps[b].frameUs; });

            // The main loop's frames, the world's steps and the trace.
            gameMs.reserve(mains.size());
            double mainSeconds = 0, waitTotalMs = 0, waitMaxMs = 0;
            uint64_t noStep = 0, multiStep = 0;
            int64_t previousStep = startStep;
            stats::Fnv64 trace;
            for (const MainRow& r : mains)
            {
                gameMs.push_back(double(r.frameUs) / 1000.0);
                mainSeconds += double(r.frameUs) / 1e6;
                waitTotalMs += double(r.waitUs) / 1000.0;
                waitMaxMs = std::max(waitMaxMs, double(r.waitUs) / 1000.0);
                int64_t steps = int64_t(r.step) - previousStep;
                noStep += steps == 0;
                multiStep += steps >= 2;
                previousStep = r.step;
                trace.Add(r.loop);
                trace.Add(int64_t(r.step));
                trace.Add(r.ticks);
            }
            stats::Series game = stats::Summarize(gameMs);
            int64_t worldSteps = mains.empty() ? 0 : int64_t(mains.back().step) - startStep;
            double timedemoFps = mainSeconds > 0 ? double(mains.size()) / mainSeconds : 0.0;
            double avgFps = swapSeconds > 0 ? double(swaps.size()) / swapSeconds : 0.0;
            std::string traceHex = stats::Hex64(trace.Value());

            Divergence divergence;
            {
                std::lock_guard lock(s_divergenceMutex);
                divergence = s_divergence;
            }

            // summary.json
            stats::Json json;
            json.Field("schema", 1).Field("status", StatusName(status)).Field("label", config.label)
                .Field("version", report::VersionString()).Field("build_id", report::BuildId()).Field("build", report::BuildString());
            json.Field("timedemo_fps", timedemoFps, 2);
            json.BeginObject("timedemo")
                .Field("file", timedemo::File().string())
                .Field("file_sha256", FileSha256(timedemo::File()));
            if (raceLoop >= 0)
                json.Field("race_loop", raceLoop);
            else
                json.Null("race_loop");
            if (s_meta.raceLoop >= 0)
                json.Field("meta_race_loop", s_meta.raceLoop);
            if (s_untilLoop >= 0)
                json.Field("valid_until_loop", s_untilLoop);
            if (s_limitLoop >= 0 && config.steps < 0)
                json.Field("limit_loop", s_limitLoop);
            json.Field("from", config.from).Field("steps", worldSteps);
            if (config.steps >= 0)
                json.Field("requested_steps", config.steps);
            json.End();
            json.BeginObject("window")
                .Field("frames", mains.size()).Field("swaps", swaps.size())
                .Field("seconds", swapSeconds).Field("main_seconds", mainSeconds)
                .Field("end", EndName(end));
            if (!mains.empty())
                json.Field("first_loop", mains.front().loop).Field("last_loop", mains.back().loop)
                    .Field("first_step", startStep).Field("last_step", int64_t(mains.back().step));
            if (!swaps.empty())
                json.Field("first_swap", swaps.front().swap).Field("last_swap", swaps.back().swap);
            json.Field("timeout_s", config.timeoutS, 0).End();
            json.BeginObject("fps")
                .Field("avg", avgFps, 2).Field("low_1pct", frames.low1Fps, 2).Field("low_01pct", frames.low01Fps, 2)
                .Field("low_01pct_unreliable", frames.low01Unreliable)
                .Field("p99_fps", frames.p99 > 0 ? 1000.0 / frames.p99 : 0.0, 2)
                .End();
            Distribution(json, "frame_ms", frames);
            Distribution(json, "game_frame_ms", game);
            json.BeginObject("wait_ms").Field("total", waitTotalMs).Field("max", waitMaxMs)
                .Field("mean", mains.empty() ? 0.0 : waitTotalMs / double(mains.size())).End();
            json.BeginObject("cpu").Field("cp_busy_mean", busyMs * perSwap).Field("cp_idle_mean", idleMs * perSwap);
            json.BeginObject("cp_waits")
                .Field("throttle", waitMs[renderer::CP_WAIT_THROTTLE] * perSwap)
                .Field("regmem", waitMs[renderer::CP_WAIT_REGMEM] * perSwap)
                .Field("shadow", waitMs[renderer::CP_WAIT_SHADOW] * perSwap)
                .Field("flush", waitMs[renderer::CP_WAIT_FLUSH] * perSwap)
                .Field("pipeline", waitMs[renderer::CP_WAIT_PIPELINE] * perSwap)
                .Field("slot", waitMs[renderer::CP_WAIT_SLOT] * perSwap)
                .Field("suspended", waitMs[renderer::CP_WAIT_SUSPENDED] * perSwap)
                .End().End();
            json.BeginObject("gpu").Field("busy_mean", gpuBusyMs * perSwap);
            if (gpuExecMs > 0)  // timestamps only with NFSMW_GPU_TIMING=1
                json.Field("exec_mean", gpuExecMs * perSwap);
            json.End();
            json.BeginObject("hitches").Field("late", late).Field("over_2x_median", over2x).Field("over_50ms", over50);
            json.BeginArray("worst");
            for (size_t i = 0; i < worstCount; i++)
            {
                const SwapRow& r = swaps[worst[i]];
                json.BeginObject().Field("swap", r.swap).Field("loop", r.loop).Field("ms", double(r.frameUs) / 1000.0).End();
            }
            json.End().End();
            json.BeginObject("display").Field("dropped", dropped).Field("repeated", repeated).End();
            json.BeginObject("world").Field("steps", worldSteps).Field("no_step", noStep).Field("multi_step", multiStep).End();
            json.BeginObject("pipelines").Field("created", pipelines).Field("skips", pipelineSkips).End();
            {
                json.BeginObject("memory");
                struct rusage usage{};
                if (getrusage(RUSAGE_SELF, &usage) == 0)
                {
#ifdef __APPLE__
                    double peakMb = double(usage.ru_maxrss) / 1048576.0;  // bytes on Apple platforms
#else
                    double peakMb = double(usage.ru_maxrss) / 1024.0;     // KB on Linux
#endif
                    json.Field("rss_peak_mb", peakMb, 1);
                }
                if (uint64_t device = s_devicePeak.load(); device > 0)
                    json.Field("device_peak_mb", double(device) / 1048576.0, 1);
                json.End();
            }
            json.BeginObject("determinism");
            if (s_diverged.load())
                json.BeginObject("divergence").Field("kind", divergence.kind).Field("loop", divergence.loop)
                    .Field("step", divergence.step).End();
            else
                json.Null("divergence");
            json.Field("race_loop_matches", !s_raceLoopMismatch.load())
                .Field("trace_hash", traceHex).Field("draws_total", draws).End();
            std::string shotsLine = s_shots.empty() ? std::string() : ShotsJson(json);
            // What makes two runs comparable, in the bench script's words (the
            // settings their NFSMW_* variables set: NFSMW_RESOLUTION is
            // Internal Resolution, NFSMW_SCREEN_RESOLUTION the present
            // scale), and the guest vblank's rate a capped run is held to.
            auto setting = [](settings::Id id) { return settings::FormatValue(id, settings::GetValue(id)); };
            json.BeginObject("mode")
                .Field("uncapped", timedemo::Uncapped())
                .Field("vsync", settings::GetBool(settings::Id::VSync))
                .Field("frame_rate", setting(settings::Id::FrameRate))
                .Field("resolution", setting(settings::Id::InternalResolution))
                .Field("screen_resolution", setting(settings::Id::PresentScale))
                .Field("aspect", setting(settings::Id::AspectRatio))
                .Field("anti_aliasing", settings::GetBool(settings::Id::AntiAliasing))
                .Field("async_pipelines", settings::GetBool(settings::Id::AsyncShaders))
                .Field("vblank_hz", 1e9 / std::max(video::GuestVblank().PeriodNs(), 1.0), 3)
                .End();
            json.BeginObject("settings");
            for (const settings::Option& o : settings::Options())
                json.Field(std::format("{}.{}", o.table, o.key), settings::FormatValue(o.id, settings::GetValue(o.id)));
            json.End();
            {
                std::vector<std::string> vars;
                for (char** e = environ; e && *e; e++)
                    if (strncmp(*e, "NFSMW_", 6) == 0)
                        vars.push_back(*e);
                std::sort(vars.begin(), vars.end());
                json.BeginObject("env");
                for (const std::string& v : vars)
                {
                    size_t eq = v.find('=');
                    json.Field(v.substr(0, eq), eq == std::string::npos ? std::string() : v.substr(eq + 1));
                }
                json.End();
            }
            {
                auto items = SystemItems();
                json.BeginObject("system");
                for (const char* key : { "OS", "Machine", "CPU", "RAM", "GPU", "Display", "Session" })
                    if (auto it = items.find(key); it != items.end())
                    {
                        std::string name = key;
                        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                        json.Field(name, it->second);
                    }
                json.End();
            }

            // frames.csv and main.csv
            stats::Csv framesCsv({ "swap", "t_us", "frame_us", "draws", "cp_busy_us", "cp_wait_throttle_us", "cp_wait_other_us",
                "gpu_busy_us", "late" }, swaps.size());
            for (const SwapRow& r : swaps)
            {
                int64_t other = 0;
                for (uint32_t k = 0; k < renderer::CP_WAIT_KINDS; k++)
                    if (k != renderer::CP_WAIT_THROTTLE && k != renderer::CP_WAIT_SUSPENDED)
                        other += r.waitUs[k];
                framesCsv.Row({ int64_t(r.swap), r.tUs, r.frameUs, r.draws, r.busyUs, r.waitUs[renderer::CP_WAIT_THROTTLE], other,
                    r.gpuBusyUs, r.late });
            }
            stats::Csv mainCsv({ "loop", "step", "t_us", "frame_us", "wait_us", "flow" }, mains.size());
            for (const MainRow& r : mains)
                mainCsv.Row({ r.loop, r.step, r.tUs, r.frameUs, r.waitUs, r.flow });

            std::error_code error;
            std::filesystem::create_directories(s_outDir, error);
            bool written = !error;
            if (error)
                fprintf(stderr, "[bench] can't create %s: %s\n", s_outDir.c_str(), error.message().c_str());
            else
            {
                written = WriteFile(s_outDir / "summary.json", json.Finish()) && written;
                written = WriteFile(s_outDir / "frames.csv", framesCsv.Text()) && written;
                written = WriteFile(s_outDir / "main.csv", mainCsv.Text()) && written;
            }

            if (!shotsLine.empty())
                fprintf(stderr, "[bench] shots: %s (summary.json's \"shots\")\n", shotsLine.c_str());

            // The one line: the classic timedemo figure (main loop frames over
            // their time), then the command processor's frame times.
            std::string why;
            if (s_diverged.load())
                why = std::format(" | divergence {} at loop {} step {}", divergence.kind, divergence.loop, divergence.step);
            else if (s_raceLoopMismatch.load())
                why = std::format(" | race loaded at loop {}, the meta says {}", raceLoop, s_meta.raceLoop);
            if (end != End::Steps && end != End::Limit)
                why += std::format(" | ended: {}", EndName(end));
            fprintf(stderr, "[bench] %s %s: %zu frames %.1f s | timedemo %.1f fps, 1%% low %.1f | p50 %.1f p99 %.1f max %.1f ms"
                " | wait %.1f ms | trace %.4s..%s%s | %s%s\n",
                config.label.c_str(), StatusName(status), mains.size(), mainSeconds, timedemoFps, frames.low1Fps,
                frames.p50, frames.p99, frames.max, waitTotalMs, traceHex.c_str(), traceHex.substr(14).c_str(), why.c_str(),
                s_outDir.c_str(), written ? "" : " (not written)");

            s_status.store(status, std::memory_order_release);
            if (config.exit)
                Quit(StatusCode(status));
            else
                fprintf(stderr, "[bench] NFSMW_BENCH_EXIT=0: the game goes on\n");
        }

        // The window close's way out (main.cpp: settings, staged saves, the
        // renderer's shutdown with its pipeline cache, the log, the exit code
        // from ExitCode). Headless there is no event loop to take it: then
        // the launcher-gone exit's steps, here. A shutdown that hangs gets a
        // hard exit after 15 s.
        void Quit(int code)
        {
#if defined(__APPLE__) && TARGET_OS_IPHONE
            // An iPhone or iPad app doesn't quit itself (ui/quit_game.h).
            (void)code;
            fprintf(stderr, "[bench] iOS: the game stays open\n");
#else
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            e.quit.timestamp = SDL_GetTicksNS();
            if (SDL_PushEvent(&e))
                fprintf(stderr, "[bench] quitting (as closing the window does), exit code %d\n", code);
            else
            {
                fprintf(stderr, "[bench] no event loop to quit through (%s): exiting here, code %d\n", SDL_GetError(), code);
                vfs::WaitForStagedFiles(std::chrono::seconds(2));
                renderer::Shutdown();
                report::FlushLog();
                std::_Exit(code);
            }
            std::this_thread::sleep_for(std::chrono::seconds(15));
            fprintf(stderr, "[bench] the game hasn't quit after 15 s: exiting, code %d\n", code);
            report::FlushLog();
            std::_Exit(code);
#endif
        }

        void ReportThread()
        {
            SetHostThreadName("nfsmw-bench");
            const Config& config = GetConfig();
            bool timedOut = false;
            {
                std::unique_lock lock(s_reportMutex);
                if (config.timeoutS > 0)
                    timedOut = !s_reportReady.wait_until(lock, s_deadline, [] { return s_reportRequested; });
                else
                    s_reportReady.wait(lock, [] { return s_reportRequested; });
            }
            if (timedOut)
            {
                // Stop the writers; if the drain finished in the meantime, the
                // report is a whole one after all.
                State state = s_state.load(std::memory_order_acquire);
                while (state != State::Done && !s_state.compare_exchange_weak(state, State::Done, std::memory_order_acq_rel))
                {
                }
                if (state != State::Done)
                {
                    s_timedOut.store(true);
                    fprintf(stderr, "[bench] timed out after %.0f s (%s): reporting what was measured\n", config.timeoutS,
                        state == State::Armed ? "the window never began" : state == State::Running ? "in the window" : "draining");
                }
            }
            WriteReport();
        }
    }

    bool Enabled()
    {
        static const bool enabled = [] {
            if (!timedemo::Replaying())
                return false;
            if (const char* v = std::getenv("NFSMW_BENCH"); v && v[0] == '0')
            {
                fprintf(stderr, "[bench] recorder off (NFSMW_BENCH=0)\n");
                return false;
            }
            return true;
        }();
        return enabled;
    }

    bool Measuring()
    {
        if (!Enabled())
            return false;
        State state = s_state.load(std::memory_order_acquire);
        return state == State::Running || state == State::Draining;
    }

    void OnMainFrame()
    {
        if (!Enabled())
            return;
        State state = s_state.load(std::memory_order_acquire);
        if (state == State::Draining || state == State::Done)
            return;
        int64_t now = guesttime::NowNs();
        if (state == State::Off)
        {
            t_mainThread = true;
            Arm();
            state = State::Armed;
        }
        const Config& config = GetConfig();
        uint32_t loop = timedemo::RealLoop();
        int64_t step = timedemo::WorldStep();
        uint32_t flow = timedemo::FlowState();
        s_loopNow.store(loop, std::memory_order_relaxed);
        if (!s_shots.empty())
            ShotsOnMainFrame(loop, step, flow);
        // The replay flag drops at the log's end (or a divergence, which
        // Diverged reports first); it is 0 too before boot sets it.
        bool replaying = ReadGuest32(kReplayOn) != 0;
        s_replaySeen = s_replaySeen || replaying;
        bool logEnded = s_replaySeen && !replaying && !s_diverged.load(std::memory_order_relaxed);
        // The race's first frame, at the same point timedemo::OnMainFrame
        // (just before this, which logs it) and the capture take it: a replay
        // that loaded it at another loop is out of step, whatever the
        // checksums say.
        if (flow == kRaceFlow && s_raceLoop.load(std::memory_order_relaxed) < 0)
        {
            s_raceLoop.store(loop, std::memory_order_relaxed);
            s_raceLoopMismatch.store(s_meta.raceLoop >= 0 && int64_t(loop) != s_meta.raceLoop);
        }

        End end = End::None;
        if (state == State::Armed)
        {
            // Before the window: a divergence or the log's end (a capture
            // shorter than the race's loading) leaves an empty report.
            if (s_diverged.load(std::memory_order_relaxed))
                end = End::Diverged;
            else if (logEnded)
                end = End::LogEnded;
            else if (flow == kRaceFlow && step >= config.from)
            {
                s_t0.store(now, std::memory_order_relaxed);
                s_prevNs = now;
                s_waitNs = 0;
                s_startStep.store(step, std::memory_order_relaxed);
                uint64_t swap = gpu::FrameCount();
                s_firstSwap.store(swap, std::memory_order_relaxed);
                if (Advance(State::Armed, State::Running))
                    fprintf(stderr, "[bench] window began at loop %u, step %lld, swap %llu\n", loop, (long long)step,
                        (unsigned long long)swap);
            }
        }
        else
        {
            // Running: the iteration that just ended.
            int64_t t0 = s_t0.load(std::memory_order_relaxed);
            MainRow row{ (now - t0) / 1000, loop, int32_t(step), Clamp32((now - s_prevNs) / 1000), Clamp32(s_waitNs / 1000),
                flow, ReadGuest32(kTicks) };
            s_prevNs = now;
            s_waitNs = 0;
            bool stored = s_main.Append(row);
            if (s_diverged.load(std::memory_order_relaxed))
                end = End::Diverged;
            else if (logEnded)
                end = End::LogEnded;
            else if (flow != kRaceFlow)
                end = End::Flow;
            else if (config.steps >= 0 && step >= config.from + config.steps)
                end = End::Steps;
            else if (config.steps < 0 && s_limitLoop >= 0 && int64_t(loop) >= s_limitLoop)
                end = End::Limit;  // the default window, kept clear of the log's replayable end
            else if (config.steps >= 0 && s_untilLoop >= 0 && int64_t(loop) >= s_untilLoop)
            {
                // Past here the log doesn't hold what the capture saw: an
                // NFSMW_BENCH_STEPS window this long has run out of log.
                s_logEnded.store(true);
                end = End::Limit;
            }
            else if (!stored || s_main.Full() || s_swaps.Full())
                end = End::Capacity;
        }
        if (end == End::LogEnded)
            s_logEnded.store(true);
        if (end != End::None)
            EndWindow(state, end, loop, step);
    }

    void OnShotSwap(uint64_t swap, bool gpuFront)
    {
        std::vector<size_t> due;
        {
            std::lock_guard lock(s_shotMutex);
            while (!s_shotQueue.empty() && s_shots[s_shotQueue.front()].swap <= swap)
            {
                size_t i = s_shotQueue.front();
                s_shotQueue.pop_front();
                Shot& shot = s_shots[i];
                shot.copied = swap;
                if (swap != shot.swap)
                {
                    // Named after the command processor had passed it (a
                    // race the model rules out): another frame, not written.
                    shot.state = Shot::State::Failed;
                    shot.why = std::format("its swap {} was named after the command processor had executed it (copied {})",
                        shot.swap, swap);
                    fprintf(stderr, "[bench] shot %lld: %s\n", (long long)shot.step, shot.why.c_str());
                    continue;
                }
                shot.state = Shot::State::Copying;
                due.push_back(i);
            }
            g_shotSwap.store(s_shotQueue.empty() ? 0 : s_shots[s_shotQueue.front()].swap, std::memory_order_relaxed);
        }
        s_shotChanged.notify_all();
        if (due.empty())
            return;
        if (!gpuFront)
        {
            WriteShots(due, nullptr, 0, 0);
            return;
        }
        renderer::ReadBackFrontImage([due](const uint8_t* rgba, uint32_t width, uint32_t height) { WriteShots(due, rgba, width, height); });
    }

    void OnSwap()
    {
        if (!Enabled())
            return;
        OnSwap(gpu::FrameCount(), guesttime::NowNs(), renderer::GetStats().draws);
    }

    void OnSwap(uint64_t swap, int64_t swapNs, uint64_t draws)
    {
        if (!Enabled())
            return;
        State state = s_state.load(std::memory_order_acquire);
        if (state == State::Off || state == State::Done)
            return;
        // Kept from the first swap after arming, so the window's first swap
        // has a swap before it to take its deltas from.
        CpTotals now = TakeCpTotals(swapNs, draws);
        if (state == State::Running || state == State::Draining)
        {
            uint64_t first = s_firstSwap.load(std::memory_order_relaxed);
            uint64_t last = state == State::Draining ? s_lastSwap.load(std::memory_order_relaxed) : UINT64_MAX;
            if (s_cpLast.valid && swap > first && swap <= last)
            {
                const CpTotals& before = s_cpLast;
                SwapRow row{};
                row.swap = swap;
                row.tUs = (swapNs - s_t0.load(std::memory_order_relaxed)) / 1000;
                row.frameUs = Clamp32((now.ns - before.ns) / 1000);
                row.draws = Clamp32(int64_t(now.draws - before.draws));
                row.busyUs = Clamp32(int64_t((now.busyNs - before.busyNs) / 1000));
                for (uint32_t k = 0; k < renderer::CP_WAIT_KINDS; k++)
                    row.waitUs[k] = Clamp32(int64_t((now.waitNs[k] - before.waitNs[k]) / 1000));
                row.gpuBusyUs = Clamp32(int64_t(now.gpuBusyUs - before.gpuBusyUs));
                row.gpuExecUs = Clamp32(int64_t(now.gpuExecUs - before.gpuExecUs));
                row.loop = s_loopNow.load(std::memory_order_relaxed);
                row.late = Clamp16(now.late - before.late);
                row.pipelines = Clamp16(now.pipelines - before.pipelines);
                row.pipelineSkips = Clamp16(now.pipelineSkips - before.pipelineSkips);
                row.dropped = Clamp16(now.dropped - before.dropped);
                row.repeated = Clamp16(now.repeated - before.repeated);
                s_swaps.Append(row);  // full: the main thread ends the window (End::Capacity)
            }
            if (state == State::Draining && swap >= last + kDrainSwaps)
                RequestReport();
        }
        s_cpLast = now;
    }

    void NoteDeviceMemory(uint64_t bytes)
    {
        if (!Enabled() || bytes == 0)
            return;
        State state = s_state.load(std::memory_order_relaxed);
        if (state != State::Running && state != State::Draining)
            return;
        if (bytes > s_devicePeak.load(std::memory_order_relaxed))
            s_devicePeak.store(bytes, std::memory_order_relaxed);  // the command processor is the only writer
    }

    void Diverged(const char* kind, uint32_t loop, int64_t step)
    {
        if (!Enabled())
            return;
        {
            std::lock_guard lock(s_divergenceMutex);
            if (s_diverged.load())
                return;  // the first one is where it went out of step
            s_divergence = { kind ? kind : "?", loop, step };
            s_diverged.store(true);
        }
    }

    void Interrupted()
    {
        if (!Enabled())
            return;
        if (!s_interrupted.exchange(true))
            fprintf(stderr, "[bench] the runtime's UI interrupted the replay: the run counts as interrupted\n");
    }

    int ExitCode()
    {
        if (!Enabled())
            return 0;
        Status status = s_status.load(std::memory_order_acquire);
        if (status != Status::None)
            return StatusCode(status);
        // Quit before the report: what is known so far.
        if (s_diverged.load())
            return StatusCode(Status::Diverged);
        return StatusCode(Status::Interrupted);
    }
}

// The guest's Sleep(ms) (SleepEx(ms, TRUE); 27 callers): on the main thread
// during the window, the time the frame spent there. The replay's loading
// waits (QUEUEDFILE polls with Sleep(8)) land here, as do the game's own
// frame pacing sleeps, if any.
extern "C" PPC_FUNC(__imp__sub_82812D38);
PPC_FUNC(sub_82812D38)
{
    if (bench::s_state.load(std::memory_order_relaxed) != bench::State::Running || !bench::t_mainThread)
    {
        __imp__sub_82812D38(ctx, base);
        return;
    }
    int64_t start = guesttime::NowNs();
    __imp__sub_82812D38(ctx, base);
    bench::s_waitNs += guesttime::NowNs() - start;
}
