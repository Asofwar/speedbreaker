// Tests for user/paths.h's base folders: $XDG_DATA_HOME and $XDG_CACHE_HOME
// count only when they hold an absolute path (the XDG Base Directory spec:
// an empty or relative one is invalid and ignored). XDG_DATA_HOME= once put
// saves and settings in whatever the current folder was at launch. From the
// repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/saves_xdg_paths_test.cpp -o build/saves_xdg_paths_test
//   build/saves_xdg_paths_test
#include <user/paths.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;

static void Set(const char* name, const char* value)
{
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
}

int main()
{
    setenv("HOME", "/home/p", 1);
    portable::CurrentLayout() = {};  // not portable: the home folders

    // Where each platform puts them without the variables, and whether it
    // reads the variables at all (macOS doesn't; iOS does, for test runs).
#ifdef __APPLE__
    const fs::path dataDefault = "/home/p/Library/Application Support", cacheDefault = "/home/p/Library/Caches";
#if TARGET_OS_IOS
    const bool readsXdg = true;
#else
    const bool readsXdg = false;
#endif
#else
    const fs::path dataDefault = "/home/p/.local/share", cacheDefault = "/home/p/.cache";
    const bool readsXdg = true;
#endif

    // Unset, empty, relative (a tilde isn't expanded either: relative):
    // the defaults, and never a folder relative to wherever the game was
    // started from.
    for (const char* ignored : { (const char*)nullptr, "", "relative/data", "./data", "data", "~/data", " /data" })
    {
        const char* shown = ignored ? ignored : "(unset)";
        Set("XDG_DATA_HOME", ignored);
        Set("XDG_CACHE_HOME", ignored);
        CHECK(XdgBaseDir("XDG_DATA_HOME") == nullptr && XdgBaseDir("XDG_CACHE_HOME") == nullptr, "'%s' ignored", shown);
        CHECK(UserDataBase() == dataDefault, "'%s': data in %s", shown, UserDataBase().c_str());
        CHECK(CacheBase() == cacheDefault, "'%s': caches in %s", shown, CacheBase().c_str());
        CHECK(HomeUserPath().is_absolute() && HomeCachePath().is_absolute(), "'%s': %s, %s", shown,
            HomeUserPath().c_str(), HomeCachePath().c_str());
        CHECK(GetSavePath(true) == dataDefault / DataFolderName() / "save", "'%s': saves in %s", shown,
            GetSavePath(true).c_str());
    }

    // Absolute: used where the platform reads them.
    Set("XDG_DATA_HOME", "/xdg/data");
    Set("XDG_CACHE_HOME", "/xdg/cache");
    CHECK(XdgBaseDir("XDG_DATA_HOME") && XdgBaseDir("XDG_DATA_HOME") == std::string_view("/xdg/data"), "absolute data");
    CHECK(XdgBaseDir("XDG_CACHE_HOME") && XdgBaseDir("XDG_CACHE_HOME") == std::string_view("/xdg/cache"), "absolute cache");
    CHECK(UserDataBase() == (readsXdg ? fs::path("/xdg/data") : dataDefault), "data in %s", UserDataBase().c_str());
    CHECK(CacheBase() == (readsXdg ? fs::path("/xdg/cache") : cacheDefault), "caches in %s", CacheBase().c_str());
    CHECK(GetSavePath(true) == (readsXdg ? fs::path("/xdg/data") : dataDefault) / DataFolderName() / "save",
        "saves in %s", GetSavePath(true).c_str());
    // One valid, one not: each on its own.
    Set("XDG_CACHE_HOME", "cache");
    CHECK(UserDataBase() == (readsXdg ? fs::path("/xdg/data") : dataDefault), "data still in %s", UserDataBase().c_str());
    CHECK(CacheBase() == cacheDefault, "relative caches ignored: %s", CacheBase().c_str());

    Set("XDG_DATA_HOME", nullptr);
    Set("XDG_CACHE_HOME", nullptr);
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
