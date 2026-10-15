// Tests for kernel/vfs.h's atomic writes of saves: the temporary files'
// names, writes that store less than asked, whole-file writes that leave
// either the old file or all of the new one (a full disk midway, a folder in
// the way), the rewrites the file objects make of saves (vfs::Staging, as
// imports_file.cpp drives it), and the clean-up of what crashed writes left.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/saves_atomic_test.cpp -o build/saves_atomic_test -pthread
//   build/saves_atomic_test [folder]
// The disk checks run in a new folder made inside [folder] (default: the
// system's temporary folder), deleted afterwards.
#include <kernel/vfs.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;

static std::string Read(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void Write(const fs::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// Temporary files left in `dir` (none after a write, whether it worked or not).
static int StagingFiles(const fs::path& dir)
{
    int n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        n += vfs::IsStagingName(e.path().filename().string());
    return n;
}

static std::string Pattern(size_t size)
{
    std::string text(size, '\0');
    for (size_t i = 0; i < size; i++)
        text[i] = char('A' + (i * 7 + i / 251) % 26);
    return text;
}

static void Names()
{
    CHECK(vfs::StagingPath("/u/save/Career", 1234) == "/u/save/Career.1234.sbtmp", "%s",
        vfs::StagingPath("/u/save/Career", 1234).c_str());
    CHECK(vfs::StagingPath("/u/save/a.xcontent", 7) == "/u/save/a.xcontent.7.sbtmp", "an extension kept");

    long pid = 0;
    CHECK(vfs::IsStagingName("Career.1234.sbtmp", &pid) && pid == 1234, "pid %ld", pid);
    CHECK(vfs::IsStagingName("a.xcontent.4194304.sbtmp", &pid) && pid == 4194304, "pid %ld", pid);
    CHECK(vfs::IsStagingName(vfs::StagingPath("/x/Name With Spaces", 99).filename().string(), &pid) && pid == 99,
        "StagingPath's names are recognised");
    for (const char* other : { "Career", "Career.sbtmp", ".1234.sbtmp", "Career..sbtmp", "Career.12a4.sbtmp",
             "Career.1234.tmp", "Career.1234.sbtmpx", "Career.1234.SBTMP", "Career.12345678901.sbtmp", "sbtmp", "" })
        CHECK(!vfs::IsStagingName(other), "'%s' isn't a temporary file", other);
}

static void WriteAllAndCopy(const fs::path& dir)
{
    fs::path file = dir / "data";
    int fd = open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "open: %s", strerror(errno));
    std::string big = Pattern(300000);
    CHECK(vfs::WriteAll(fd, big.data(), big.size(), 0) == 0, "300 KB");
    CHECK(vfs::WriteAll(fd, "xyz", 3, 10) == 0, "at an offset");
    CHECK(vfs::SyncFile(fd) == 0, "flushed");
    close(fd);
    std::string expected = big;
    expected.replace(10, 3, "xyz");
    CHECK(Read(file) == expected, "contents");

    // Copying a save that is updated in place: the same bytes.
    fs::path copy = dir / "copy";
    int from = open(file.c_str(), O_RDONLY), to = open(copy.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(vfs::CopyContents(from, to) == 0, "copied");
    close(from);
    close(to);
    CHECK(Read(copy) == expected, "a copy of %zu bytes", Read(copy).size());

    // A descriptor that can't be written: the error, not a success.
    int readOnly = open(file.c_str(), O_RDONLY);
    CHECK(vfs::WriteAll(readOnly, "a", 1, 0) == EBADF, "read-only");
    close(readOnly);
}

// Files capped at `limit` bytes (RLIMIT_FSIZE): writes past it store what
// fits, then fail with EFBIG, as a disk filling up midway does with ENOSPC.
// No limit: the one the test started with.
static bool LimitFileSize(rlim_t limit)
{
    static struct rlimit original;
    static bool saved = getrlimit(RLIMIT_FSIZE, &original) == 0;
    if (!saved)
        return false;
    struct rlimit r = original;
    if (limit != RLIM_INFINITY)
        r.rlim_cur = limit;
    return setrlimit(RLIMIT_FSIZE, &r) == 0;
}

static void ShortWrites(const fs::path& dir)
{
    signal(SIGXFSZ, SIG_IGN);  // the error instead of the signal
    fs::path file = dir / "short";
    int fd = open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (!LimitFileSize(4096))
    {
        printf("  (no RLIMIT_FSIZE here: short writes not checked)\n");
        close(fd);
        return;
    }
    std::string big = Pattern(10000);
    int error = vfs::WriteAll(fd, big.data(), big.size(), 0);
    LimitFileSize(RLIM_INFINITY);
    close(fd);
    // A single pwrite stored 4096 bytes and reported that as success.
    CHECK(error == EFBIG, "a write cut short is an error: %s", strerror(error));
    CHECK(fs::file_size(file) == 4096, "what fit: %ju", uintmax_t(fs::file_size(file)));

    // A whole-file write that doesn't fit: the old file is kept as it was.
    fs::path save = dir / "Career";
    Write(save, "the previous save");
    LimitFileSize(4096);
    std::string failed = vfs::WriteAtomically(save, big.data(), big.size());
    LimitFileSize(RLIM_INFINITY);
    CHECK(!failed.empty(), "reported");
    CHECK(Read(save) == "the previous save", "the old save kept: '%s'", Read(save).c_str());
    CHECK(StagingFiles(dir) == 0, "its temporary file removed");
    signal(SIGXFSZ, SIG_DFL);
}

static void WholeFiles(const fs::path& dir)
{
    // A new file, in a folder that doesn't exist yet (a first save).
    fs::path meta = dir / "save" / "Career.xcontent";
    std::string first = Pattern(306);
    CHECK(vfs::WriteAtomically(meta, first.data(), first.size()).empty(), "new");
    CHECK(Read(meta) == first, "new contents");
    CHECK(StagingFiles(meta.parent_path()) == 0, "no temporary file left");

    // Replacing it: all of the new one.
    std::string second = Pattern(5000).substr(17);
    CHECK(vfs::WriteAtomically(meta, second.data(), second.size()).empty(), "replaced");
    CHECK(Read(meta) == second, "replaced contents");
    CHECK(vfs::WriteAtomically(meta, "", 0).empty() && fs::file_size(meta) == 0, "empty is a size too");
    CHECK(StagingFiles(meta.parent_path()) == 0, "no temporary file left");

    // A folder where the file goes: refused, the folder untouched.
    fs::path folder = dir / "save" / "Folder";
    fs::create_directories(folder / "inside");
    std::string error = vfs::WriteAtomically(folder, "x", 1);
    CHECK(!error.empty(), "a folder in the way");
    CHECK(fs::is_directory(folder / "inside"), "the folder kept");
    CHECK(StagingFiles(folder.parent_path()) == 0, "its temporary file removed");

    // The rename on its own: the target replaced, the temporary file gone.
    fs::path target = dir / "save" / "Profile";
    Write(target, "old");
    fs::path temp = vfs::StagingPath(target, long(getpid()));
    Write(temp, "new");
    CHECK(vfs::ReplaceDurably(temp, target) == 0 && Read(target) == "new" && !fs::exists(temp), "replaced");
    CHECK(vfs::ReplaceDurably(temp, target) == ENOENT && Read(target) == "new", "nothing to rename: the target kept");

    // A folder that can't be written (not as root, which can anyway).
    if (geteuid() != 0)
    {
        fs::path locked = dir / "locked";
        fs::create_directories(locked);
        Write(locked / "Career", "the previous save");
        chmod(locked.c_str(), 0555);
        CHECK(!vfs::WriteAtomically(locked / "Career", "new", 3).empty(), "read-only folder");
        CHECK(Read(locked / "Career") == "the previous save", "the old save kept");
        chmod(locked.c_str(), 0755);
    }
}

// A handle on `target`'s rewrite, as NtCreateFile opens one: the rewrite in
// progress joined, else a new one. -1 on failure.
static int OpenRewrite(vfs::Staging& staging, std::shared_ptr<vfs::StagedFile>& staged, const fs::path& target,
    const fs::path& root, bool keep, bool truncate)
{
    std::lock_guard lock(staging.Mutex());
    staged = staging.FindLocked(target);
    int error = 0;
    int fd = staging.OpenLocked(staged, target, root, keep, truncate, error);
    if (fd < 0)
        printf("  (open of %s: %s)\n", target.c_str(), strerror(error));
    return fd;
}

// As NtClose does: the handle's descriptor, then its hold on the rewrite.
static void CloseRewrite(vfs::Staging& staging, int fd, std::shared_ptr<vfs::StagedFile>& staged)
{
    close(fd);
    staging.Release(*staged);
    staged.reset();
}

static std::string ReadFd(int fd, size_t size)
{
    std::string text(size, '\0');
    ssize_t n = pread(fd, text.data(), size, 0);
    text.resize(n > 0 ? size_t(n) : 0);
    return text;
}

// The rewrites the file objects make of saves (vfs::Staging, StagedFile):
// the old save stays whole until the last handle closes, every handle sees
// the new contents, a flush of the package puts a copy in place and the
// rewrite goes on, a failed write keeps the old save.
static void Rewrites(const fs::path& dir)
{
    vfs::Staging staging;
    fs::path root = dir / "package";
    fs::create_directories(root);
    fs::path save = root / "Career";
    Write(save, "the previous save");

    // An overwrite (FILE_OVERWRITE_IF on the save): an empty rewrite.
    std::shared_ptr<vfs::StagedFile> writer;
    int fd = OpenRewrite(staging, writer, save, root, false, true);
    CHECK(fd >= 0 && writer, "opened");
    if (fd < 0)
        return;
    std::string next = Pattern(20000);
    CHECK(vfs::WriteAll(fd, next.data(), 10000, 0) == 0, "half written");
    // The process ending now (a quit's _Exit, a crash, a flat battery): the
    // old save is still there, whole, beside a hidden temporary file.
    CHECK(Read(save) == "the previous save", "the old save untouched mid-write: %zu bytes", Read(save).size());
    CHECK(StagingFiles(root) == 1, "one temporary file");

    // A second handle (a reader) opens the rewrite, not the old file.
    std::shared_ptr<vfs::StagedFile> reader;
    int fd2 = OpenRewrite(staging, reader, save, root, true, false);
    CHECK(fd2 >= 0 && reader == writer, "joined the rewrite");
    CHECK(ReadFd(fd2, 30000) == next.substr(0, 10000), "the reader sees what was written");
    CHECK(vfs::WriteAll(fd, next.data() + 10000, 10000, 10000) == 0, "the rest written");

    // The package flushed with both still open: a copy put in place, and the
    // rewrite goes on.
    staging.Commit(root);
    CHECK(Read(save) == next, "the flush put the new save in place: %zu bytes", Read(save).size());
    CHECK(StagingFiles(root) == 1, "the copy renamed, the rewrite still there");
    staging.Commit(dir / "another package");
    CHECK(Read(save) == next, "another package's flush: nothing");
    CHECK(vfs::WriteAll(fd, "TAIL", 4, 20000) == 0, "written after the flush");
    CHECK(Read(save) == next, "not in place before the close");

    // The writer closes, the reader still has it open: still not in place.
    CloseRewrite(staging, fd, writer);
    CHECK(Read(save) == next, "one handle still open");
    CHECK(!staging.Wait(std::chrono::milliseconds(1)), "a save still open");
    CloseRewrite(staging, fd2, reader);
    CHECK(Read(save) == next + "TAIL", "the last close put it in place: %zu bytes", Read(save).size());
    CHECK(StagingFiles(root) == 0, "no temporary file left");
    CHECK(staging.Wait(std::chrono::milliseconds(0)), "nothing open");
    {
        std::lock_guard lock(staging.Mutex());
        CHECK(!staging.FindLocked(save) && staging.FilesLocked().empty(), "the rewrite is over");
    }

    // A new save: on the disk only once its handle closes.
    fs::path fresh = root / "New";
    std::shared_ptr<vfs::StagedFile> created;
    fd = OpenRewrite(staging, created, fresh, root, false, false);
    CHECK(fd >= 0 && vfs::WriteAll(fd, "hello", 5, 0) == 0, "new save written");
    CHECK(!fs::exists(fresh), "not on the disk while open");
    {
        std::lock_guard lock(staging.Mutex());
        CHECK(staging.FindLocked(fresh) == created, "found among the rewrites (opens, listings)");
    }
    // A quit waits for it (vfs::WaitForStagedFiles), here closed meanwhile.
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CloseRewrite(staging, fd, created);
    });
    CHECK(staging.Wait(std::chrono::seconds(5)), "the wait ends with the close");
    closer.join();
    CHECK(Read(fresh) == "hello", "new save in place");

    // A part updated (an open that neither truncates nor creates): the
    // rewrite starts as the old save.
    std::shared_ptr<vfs::StagedFile> updater;
    fd = OpenRewrite(staging, updater, save, root, true, false);
    CHECK(fd >= 0 && vfs::WriteAll(fd, "XY", 2, 0) == 0, "two bytes updated");
    CloseRewrite(staging, fd, updater);
    CHECK(Read(save) == "XY" + (next + "TAIL").substr(2), "the rest kept");

    // A write that failed (the guest was told): the old save is kept, also
    // past a flush of the package.
    std::string before = Read(save);
    std::shared_ptr<vfs::StagedFile> failing;
    fd = OpenRewrite(staging, failing, save, root, false, true);
    CHECK(fd >= 0 && vfs::WriteAll(fd, "cut sh", 6, 0) == 0, "part written");
    staging.MarkFailed(*failing);
    staging.Commit(root);
    CHECK(Read(save) == before, "not put in place by a flush");
    CloseRewrite(staging, fd, failing);
    CHECK(Read(save) == before, "nor by the close");
    CHECK(StagingFiles(root) == 0, "its temporary file removed");

    // An overwrite joining a rewrite in progress empties it for both.
    std::shared_ptr<vfs::StagedFile> first, second;
    fd = OpenRewrite(staging, first, save, root, true, false);
    fd2 = OpenRewrite(staging, second, save, root, false, true);
    CHECK(fd >= 0 && fd2 >= 0 && first == second && ReadFd(fd, 100).empty(), "emptied");
    CHECK(vfs::WriteAll(fd2, "short", 5, 0) == 0, "rewritten");
    CloseRewrite(staging, fd2, second);
    CloseRewrite(staging, fd, first);
    CHECK(Read(save) == "short", "'%s'", Read(save).c_str());

    // A folder that can't hold the temporary file: no handle, the save kept.
    std::shared_ptr<vfs::StagedFile> nowhere;
    CHECK(OpenRewrite(staging, nowhere, root / "missing" / "Career", root, false, false) < 0 && !nowhere,
        "refused");
}

// A crash between the temporary file and the rename leaves the old save and
// a temporary file, which the guest never sees and a later write removes.
static void Leftovers(const fs::path& dir)
{
    fs::path save = dir / "leftovers";
    fs::create_directories(save);
    long me = long(getpid());
    long other = me == 99991 ? 99992 : 99991;
    auto old = fs::file_time_type::clock::now() - std::chrono::minutes(5);
    fs::path crashed = vfs::StagingPath(save / "Career", other);
    fs::path recent = vfs::StagingPath(save / "Options", other + 2);
    fs::path mine = vfs::StagingPath(save / "Open", me);
    for (const fs::path& p : { crashed, recent, mine, save / "Career", save / "notes.tmp" })
        Write(p, "x");
    fs::last_write_time(crashed, old);
    fs::last_write_time(mine, old);
    fs::last_write_time(save / "notes.tmp", old);

    vfs::RemoveStaleStaging(save);
    CHECK(!fs::exists(crashed), "another process's, minutes old: removed");
    CHECK(fs::exists(recent), "another process's, seconds old (maybe saving now): kept");
    CHECK(fs::exists(mine), "this process's own (a file it has open): kept");
    CHECK(fs::exists(save / "Career") && fs::exists(save / "notes.tmp"), "other files kept");

    // A whole-file write in the folder removes them as well.
    fs::last_write_time(recent, old);
    CHECK(vfs::WriteAtomically(save / "Career", "new", 3).empty() && Read(save / "Career") == "new", "written");
    CHECK(!fs::exists(recent), "removed by the next write there");
}

int main(int argc, char** argv)
{
    Names();

    fs::path base = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path();
    std::error_code ec;
    fs::create_directories(base, ec);
    std::string pattern = (base / "saves_atomic.XXXXXX").string();
    if (!mkdtemp(pattern.data()))
    {
        printf("FAILED: can't make a folder in %s: %s\n", base.c_str(), strerror(errno));
        return 1;
    }
    fs::path dir = pattern;
    WriteAllAndCopy(dir);
    ShortWrites(dir);
    WholeFiles(dir);
    Rewrites(dir);
    Leftovers(dir);
    fs::remove_all(dir, ec);

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
