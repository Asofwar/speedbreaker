#pragma once
// Timedemo: a race recorded once and replayed as a benchmark, through the
// record/replay system EA left in the retail game ("Joylog", off by a flag
// the game never sets). Capture writes the game's own log (inputs, the frame
// time quantum, the random seed, file loading status) to a .jlg file; replay
// feeds it back, so every run drives the same race frame for frame, at any
// host speed, and the game's per-frame checksum reports a divergence.
//
//   NFSMW_TIMEDEMO=capture|replay   the mode (off without it: no change at all)
//   NFSMW_TIMEDEMO_FILE=<path.jlg>  the log (default: <user>/timedemo/race.jlg)
//   NFSMW_TIMEDEMO_FIXED=1|0        capture: exactly 1/60 s a frame (default 1)
//   NFSMW_TIMEDEMO_UNCAPPED=1       replay: swaps released without a vblank
//                                   (with the recorder on, in its window only)
//   NFSMW_TIMEDEMO_CAPTURE_SECONDS  capture: stop and exit this long after the
//                                   race has loaded (0: at the quit)
//   NFSMW_SAVE_DIR=<folder>         saves from there (user/paths.h): a replay
//                                   runs on a fresh copy of <file>.save/
//
// A capture also writes <file>.json (its loops, settings and NFSMW_*
// variables) and <file>.save/ (the save it started from).
//
// Guest addresses are those of the supported default.xex (USA) only.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace timedemo
{
    enum class Mode : uint8_t { Off, Capture, Replay };

    Mode GetMode();
    inline bool Active() { return GetMode() != Mode::Off; }
    inline bool Capturing() { return GetMode() == Mode::Capture; }
    inline bool Replaying() { return GetMode() == Mode::Replay; }
    // Capture: every frame gets exactly 1/60 s (NFSMW_TIMEDEMO_FIXED).
    bool FixedStep();
    // Replay: swaps released without waiting for a vblank (NFSMW_TIMEDEMO_UNCAPPED).
    bool Uncapped();
    // The log file (NFSMW_TIMEDEMO_FILE or the default).
    const std::filesystem::path& File();

    // The game's counters, read from guest memory (0 / -1 when not there yet).
    uint32_t RealLoop();   // RealLoopCounter [0x82A2CD20]: +1 a frame
    int64_t WorldStep();   // WorldLoopCounter [[0x82A2CB6C]+40]: +1 a world step, 0 at a race's load; -1 without a world
    uint32_t FlowState();  // GameFlowState [0x82A39AD8]: 6 once a race has loaded

    // kernel/vfs.cpp: the host file for the game's own Joylog paths
    // (d:\CaptureJoylog.jlg, d:\ReplayJoylog.jlg) while a mode is on, with
    // whether it is read-only; nullopt for every other path or with no mode.
    std::optional<std::filesystem::path> RedirectJoylog(std::string_view guestPath, bool& readOnly);

    // Capture: flush the log's tail and write <file>.json (main.cpp, at exit).
    void FinishCapture();

    // The runtime's system UI notifications (XN_SYS_UI): capture notes where
    // the log stops being replayable; replay keeps them from the guest (true:
    // don't send) and marks the run interrupted.
    bool OnSystemUi(bool shown);

    // main.cpp, once, after default.xex is loaded and before the guest
    // starts: logs the mode and its file. A capture makes the file's folder
    // and snapshots the save (<file>.save/: save/ and profile_settings.bin)
    // before the game has read it, the state a replay must start from. A
    // replay without its file, or an NFSMW_TIMEDEMO that is neither mode,
    // stops here (exit 2). `xexSha256` goes in the capture's .json.
    void Start(std::string_view xexSha256);

    // game/frame_time.cpp's GameFrameTime hook, once a frame on the guest
    // main thread while a mode is on (before the frame runs): notes the
    // race's load, asks the game to write its capture buffer out every 600
    // frames and at FinishCapture, ends a capture after
    // NFSMW_TIMEDEMO_CAPTURE_SECONDS, and notes a replay's log running out.
    void OnMainFrame();

    // kernel/imports_unleashed.cpp: the system UI the runtime reports for the
    // game's own XamShowDeviceSelectorUI call, which shows nothing. True while
    // a mode is on, in capture and replay alike: don't send it.
    bool HideGuestSystemUi();

    // This run's race: the RealLoop of the first frame that began with
    // GameFlowState 6 (nullopt before; the same in a capture and its replay).
    std::optional<uint32_t> RaceLoop();
    // Replay: the log ran out with no divergence (the game plays live after).
    bool LogEnded();
    // Replay: the game's checks found the run out of step with the log.
    bool Diverged();

    // Replay: the loops the capture wrote in <file>.json (nullopt: no file,
    // or none written). The first loop not in the log is `end`; `validUntil`
    // the first the runtime's menu paused in the capture, else `end`.
    struct CapturedLoops
    {
        std::optional<uint32_t> race, end, validUntil;
    };
    const CapturedLoops& Captured();

    // GameFrameTime's r3 in a fixed-step capture: 1000/60 ms in 16.16.
    inline constexpr uint32_t kFixedFrame = 1092267;
    // The swap callback's argument (sub_825985A0) in an uncapped replay:
    // presentation interval 0, immediate threshold 100%.
    inline constexpr uint32_t kUncappedSwap = 0x00000064;

    // The parts that need neither the game nor the host (header only, for
    // tests/timedemo_logic_test.cpp).
    namespace detail
    {
        inline bool IEquals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
            {
                char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] - 'A' + 'a') : a[i];
                char y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] - 'A' + 'a') : b[i];
                if (x != y)
                    return false;
            }
            return true;
        }

        // The switches, as read from the environment once.
        struct Config
        {
            Mode mode = Mode::Off;
            std::string invalid;         // NFSMW_TIMEDEMO's value when it is neither mode
            std::filesystem::path file;  // NFSMW_TIMEDEMO_FILE as given (empty: the default)
            bool fixed = true;           // NFSMW_TIMEDEMO_FIXED (capture only)
            bool uncapped = false;       // NFSMW_TIMEDEMO_UNCAPPED (replay only)
            double captureSeconds = 0;   // NFSMW_TIMEDEMO_CAPTURE_SECONDS (capture only; 0: until the quit)
        };

        // A 0/1 switch: 1, true, yes, on or 0, false, no, off; anything else
        // (unset included) leaves the default.
        inline bool Switch(const char* value, bool fallback)
        {
            if (!value)
                return fallback;
            for (std::string_view on : { "1", "true", "yes", "on" })
                if (IEquals(value, on))
                    return true;
            for (std::string_view off : { "0", "false", "no", "off" })
                if (IEquals(value, off))
                    return false;
            return fallback;
        }

        // `get` is getenv, or a test's table. An empty NFSMW_TIMEDEMO, 0 or
        // off is no mode, as unset is; a typo is kept in `invalid`, so the
        // run stops instead of quietly playing the game (a benchmark script
        // would wait for its timeout).
        template <typename Get>
        Config ParseConfig(Get&& get)
        {
            Config c;
            if (const char* mode = get("NFSMW_TIMEDEMO"); mode && *mode)
            {
                if (IEquals(mode, "capture"))
                    c.mode = Mode::Capture;
                else if (IEquals(mode, "replay"))
                    c.mode = Mode::Replay;
                else if (!IEquals(mode, "0") && !IEquals(mode, "off"))
                    c.invalid = mode;
            }
            if (const char* file = get("NFSMW_TIMEDEMO_FILE"); file && *file)
                c.file = file;
            c.fixed = Switch(get("NFSMW_TIMEDEMO_FIXED"), true);
            c.uncapped = Switch(get("NFSMW_TIMEDEMO_UNCAPPED"), false);
            if (const char* s = get("NFSMW_TIMEDEMO_CAPTURE_SECONDS"); s && *s)
            {
                char* end = nullptr;
                double seconds = std::strtod(s, &end);
                if (end != s && std::isfinite(seconds) && seconds > 0)
                    c.captureSeconds = seconds;
            }
            return c;
        }

        // Each switch counts in its own mode only: a fixed step would be
        // ignored by a replay anyway (its frame times come from the log), and
        // a capture runs at the vblank as the player sees it.
        inline bool FixedStep(const Config& c) { return c.mode == Mode::Capture && c.fixed; }
        inline bool Uncapped(const Config& c) { return c.mode == Mode::Replay && c.uncapped; }
        // A capture's length in world steps after the race loaded (0: until the quit).
        inline int64_t CaptureSteps(const Config& c)
        {
            return c.mode == Mode::Capture && c.captureSeconds > 0 ? std::llround(c.captureSeconds * 60.0) : 0;
        }

        // The game's two Joylog names, by the path's last component, in any
        // case (it opens them as d:\<name>).
        enum class JoylogName : uint8_t { None, Capture, Replay };
        inline JoylogName MatchJoylog(std::string_view guestPath)
        {
            size_t cut = guestPath.find_last_of("\\/:");
            std::string_view name = cut == std::string_view::npos ? guestPath : guestPath.substr(cut + 1);
            if (IEquals(name, "CaptureJoylog.jlg"))
                return JoylogName::Capture;
            if (IEquals(name, "ReplayJoylog.jlg"))
                return JoylogName::Replay;
            return JoylogName::None;
        }

        // Where a Joylog name goes in a mode: the timedemo's file, or a path
        // that doesn't exist and can't be created (read-only), or as before.
        // The game replays whenever ReplayJoylog.jlg exists, so a capture
        // hides it (a stray file must not turn the capture into a replay),
        // and a replay hides CaptureJoylog.jlg, whose open would truncate
        // the log it is replaying.
        enum class JoylogRoute : uint8_t { Unchanged, LogFile, Absent };
        struct JoylogTarget
        {
            JoylogRoute route;
            bool readOnly;
        };
        inline JoylogTarget RouteJoylog(Mode mode, JoylogName name)
        {
            if (mode == Mode::Off || name == JoylogName::None)
                return { JoylogRoute::Unchanged, false };
            if (mode == Mode::Capture)
                return name == JoylogName::Capture ? JoylogTarget{ JoylogRoute::LogFile, false } : JoylogTarget{ JoylogRoute::Absent, true };
            return name == JoylogName::Replay ? JoylogTarget{ JoylogRoute::LogFile, true } : JoylogTarget{ JoylogRoute::Absent, true };
        }

        // Which Joylog check called DbgBreakPoint (sub_8262CE48, also the
        // game's assert break, 259 callers), by its return address; null for
        // any other caller.
        inline const char* BreakKind(uint32_t lr)
        {
            switch (lr)
            {
            case 0x823AF9CC: return "checksum";  // sub_823AF830: the frame's world, menu, seed and loop sums
            case 0x823B6F4C: return "channel";   // the log's next record is another channel's
            case 0x823B7884: return "verify";    // sub_823B7800: a 32-bit value checked against the log
            case 0x823B7D48: return "exe";       // sub_823B7BF0: the log is from another build of the game
            default: return nullptr;
            }
        }

        // <file>.json, written by a capture.
        struct Meta
        {
            std::string created;   // UTC, ISO 8601
            std::string runtime;   // report::VersionString()
            std::string buildId;   // report::BuildId()
            std::string exeCheck;  // default.xex's SHA-256
            std::string file;      // the log's file name
            uint64_t size = 0;     // its length at the last flush
            bool complete = false; // the last flush came at the quit (else an earlier one's end)
            bool fixedStep = false;
            double captureSeconds = 0;
            std::optional<uint32_t> raceLoop, endLoop, validUntilLoop, systemUiBeforeRace;
            std::vector<std::pair<std::string, std::string>> settings, env;
        };

        inline std::string JsonString(std::string_view s)
        {
            std::string out = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += c;
                }
                else if (c == '\n')
                    out += "\\n";
                else if (c == '\t')
                    out += "\\t";
                else if (static_cast<unsigned char>(c) < 0x20)
                {
                    char u[8];
                    std::snprintf(u, sizeof(u), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                    out += u;
                }
                else
                    out += c;
            }
            return out + "\"";
        }

        inline std::string MetaJson(const Meta& m)
        {
            auto loop = [](const std::optional<uint32_t>& v) { return v ? std::to_string(*v) : std::string("null"); };
            auto object = [](const std::vector<std::pair<std::string, std::string>>& items) {
                std::string out = "{";
                for (size_t i = 0; i < items.size(); i++)
                    out += (i ? ",\n    " : "\n    ") + JsonString(items[i].first) + ": " + JsonString(items[i].second);
                return out + (items.empty() ? "}" : "\n  }");
            };
            char seconds[32];
            std::snprintf(seconds, sizeof(seconds), "%g", m.captureSeconds);
            std::string out = "{\n";
            out += "  \"schema\": 1,\n";
            out += "  \"created\": " + JsonString(m.created) + ",\n";
            out += "  \"runtime\": " + JsonString(m.runtime) + ",\n";
            out += "  \"build_id\": " + JsonString(m.buildId) + ",\n";
            out += "  \"exe_check\": " + JsonString(m.exeCheck) + ",\n";
            out += "  \"file\": " + JsonString(m.file) + ",\n";
            out += "  \"size\": " + std::to_string(m.size) + ",\n";
            out += std::string("  \"complete\": ") + (m.complete ? "true" : "false") + ",\n";
            out += std::string("  \"fixed_step\": ") + (m.fixedStep ? "true" : "false") + ",\n";
            out += std::string("  \"capture_seconds\": ") + seconds + ",\n";
            out += "  \"race_loop\": " + loop(m.raceLoop) + ",\n";
            out += "  \"end_loop\": " + loop(m.endLoop) + ",\n";
            out += "  \"valid_until_loop\": " + loop(m.validUntilLoop) + ",\n";
            out += "  \"system_ui_before_race_loop\": " + loop(m.systemUiBeforeRace) + ",\n";
            out += "  \"settings\": " + object(m.settings) + ",\n";
            out += "  \"env\": " + object(m.env) + "\n";
            return out + "}\n";
        }

        // A top-level integer field of a .json MetaJson wrote (nullopt: no
        // such field, null, or not an integer). Not a JSON parser: the keys
        // it is asked for are unique in that file.
        inline std::optional<int64_t> JsonInteger(std::string_view json, std::string_view key)
        {
            std::string quoted = "\"" + std::string(key) + "\"";
            size_t at = json.find(quoted);
            if (at == std::string_view::npos)
                return std::nullopt;
            size_t i = at + quoted.size();
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
                i++;
            if (i >= json.size() || json[i] != ':')
                return std::nullopt;
            i++;
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
                i++;
            bool negative = i < json.size() && json[i] == '-';
            if (negative)
                i++;
            if (i >= json.size() || json[i] < '0' || json[i] > '9')
                return std::nullopt;
            int64_t value = 0;
            for (; i < json.size() && json[i] >= '0' && json[i] <= '9'; i++)
            {
                if (value > (INT64_MAX - 9) / 10)
                    return std::nullopt;
                value = value * 10 + (json[i] - '0');
            }
            if (i < json.size() && (json[i] == '.' || json[i] == 'e' || json[i] == 'E'))
                return std::nullopt;
            return negative ? -value : value;
        }
    }
}
