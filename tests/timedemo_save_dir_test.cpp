// Tests for NFSMW_SAVE_DIR (runtime/user/paths.h): a timedemo's replay
// (game/timedemo.h) reads the game's saves and profile settings from a
// fresh copy of its capture's, so its autosaves never touch the player's.
// Only an absolute path counts, as with the XDG variables; unset, the saves
// stay where they were. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/timedemo_save_dir_test.cpp -o build/timedemo_save_dir_test
//   build/timedemo_save_dir_test runtime
// With the runtime folder it also checks that profile_settings.bin follows
// the saves (kernel/imports_user.cpp) and the content packages use
// GetSavePath (kernel/xam.cpp).
#include <user/paths.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;

namespace
{
    void Set(const char* name, const char* value)
    {
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    }

    std::string Read(const std::string& path)
    {
        std::ifstream f(path);
        std::stringstream s;
        s << f.rdbuf();
        return s.str();
    }

    void Paths()
    {
        setenv("HOME", "/home/p", 1);
        portable::CurrentLayout() = {};  // not portable: the home folders
        Set("XDG_DATA_HOME", nullptr);
        const fs::path user = GetUserPath();

        // Unset, empty, relative: the user folder, as before.
        for (const char* ignored : { (const char*)nullptr, "", "save-run", "./save-run", "~/save-run", " /save-run" })
        {
            const char* shown = ignored ? ignored : "(unset)";
            Set("NFSMW_SAVE_DIR", ignored);
            CHECK(SaveDirOverride() == nullptr, "'%s' ignored", shown);
            CHECK(GetSaveRootPath() == user, "'%s': %s", shown, GetSaveRootPath().c_str());
            CHECK(GetSavePath(true) == user / "save", "'%s': saves in %s", shown, GetSavePath(true).c_str());
        }

        // Absolute: saves and the profile's folder there; the rest of the
        // user folder (settings, logs, the installed game) stays.
        Set("NFSMW_SAVE_DIR", "/home/deck/sb-bench/save-run");
        CHECK(SaveDirOverride() && std::string(SaveDirOverride()) == "/home/deck/sb-bench/save-run", "absolute");
        CHECK(GetSaveRootPath() == "/home/deck/sb-bench/save-run", "root %s", GetSaveRootPath().c_str());
        CHECK(GetSavePath(true) == "/home/deck/sb-bench/save-run/save", "saves in %s", GetSavePath(true).c_str());
        CHECK(GetUserPath() == user, "the user folder unchanged: %s", GetUserPath().c_str());

        // Portable mode: the override wins there too.
        portable::CurrentLayout().root = "/portable";
        portable::CurrentLayout().data = "/portable/data";
        portable::CurrentLayout().cache = "/portable/cache";
        CHECK(portable::Current().Active(), "portable mode on");
        CHECK(GetSavePath(true) == "/home/deck/sb-bench/save-run/save", "portable: saves in %s", GetSavePath(true).c_str());
        Set("NFSMW_SAVE_DIR", nullptr);
        CHECK(GetSavePath(true) == fs::path("/portable/data") / "save", "portable, unset: %s", GetSavePath(true).c_str());
        portable::CurrentLayout() = {};
        Set("NFSMW_SAVE_DIR", nullptr);
    }

    void Wiring(const std::string& rt)
    {
        std::string user = Read(rt + "/kernel/imports_user.cpp");
        CHECK(user.find("return GetSaveRootPath() / \"profile_settings.bin\";") != std::string::npos,
            "profile_settings.bin beside the saves");
        CHECK(user.find("GetUserPath() / \"profile_settings.bin\"") == std::string::npos, "not in the user folder regardless");
        std::string xam = Read(rt + "/kernel/xam.cpp");
        CHECK(xam.find("GetSavePath(true) / name") != std::string::npos, "save packages under GetSavePath");
        CHECK(xam.find("GetUserPath() / \"save\"") == std::string::npos, "no save path of its own");
    }
}

int main(int argc, char** argv)
{
    Paths();
    if (argc > 1)
        Wiring(argv[1]);
    else
        printf("  (no runtime folder given: the wiring not checked)\n");
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
