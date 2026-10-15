// Tests for update/fetch_curl.cpp, Check for Updates' transport on Linux and
// SteamOS: the system's curl, here stand-ins for it (shell scripts, through
// the NFSMW_UPDATE_CURL hook) that answer, fail with curl's exit codes, hang
// past the deadline, close their output without exiting, send more than the
// cap, or die by a signal; and the arguments, environment and descriptors
// the real one gets. Linux only, as the transport is (macOS and iOS use
// NSURLSession). From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/update_fetch_test.cpp runtime/update/fetch_curl.cpp runtime/update/check.cpp -o build/update_fetch_test
//   build/update_fetch_test
#include <update/fetch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace update;
using Clock = std::chrono::steady_clock;

static std::string g_dir;  // the scratch folder: the stand-ins and what they record

// A stand-in curl: a shell script with this body, executable. Its path.
static std::string Curl(const char* name, const std::string& body)
{
    std::string path = g_dir + "/" + name;
    std::ofstream(path) << "#!/bin/sh\n" << body;
    chmod(path.c_str(), 0755);
    return path;
}

static std::string Read(const std::string& path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

// Fetch with that curl, as the update check asks (the default timeout and
// cap unless the request says otherwise); `seconds` is how long it took.
static Fetched Run(const std::string& curl, Request request, double& seconds)
{
    setenv("NFSMW_UPDATE_CURL", curl.c_str(), 1);
    if (request.url.empty())
        request.url = kApiUrl;
    request.userAgent = "SpeedBreaker/0.0.0-test";
    const auto started = Clock::now();
    Fetched fetched = Fetch(request);
    seconds = std::chrono::duration<double>(Clock::now() - started).count();
    return fetched;
}

static Fetched Run(const std::string& curl, Request request = {})
{
    double seconds;
    return Run(curl, std::move(request), seconds);
}

static const char kAnswer[] = "printf 'HTTP/2 200\\r\\ncontent-type: application/json\\r\\n\\r\\n{\"tag_name\":\"v0.1.4\"}'\n";

static void Answers()
{
    Fetched f = Run(Curl("ok", kAnswer));
    CHECK(f.transport == Transport::Ok && f.response.status == 200 && f.response.body == R"({"tag_name":"v0.1.4"})",
        "an answer: transport %d, HTTP %d, '%s'", int(f.transport), f.response.status, f.response.body.c_str());
    CHECK(Header(f.response, "Content-Type") == "application/json", "its headers");

    f = Run(Curl("garbled", "echo hello\n"));
    CHECK(f.transport == Transport::Garbled && f.detail.find("isn't an HTTP answer") != std::string::npos, "not HTTP: %d '%s'",
        int(f.transport), f.detail.c_str());

    // Over the body's cap, though curl itself (--max-filesize) let it through.
    Request small;
    small.maxBytes = 1000;
    f = Run(Curl("big", "printf 'HTTP/1.1 200 OK\\r\\n\\r\\n'; head -c 2000 /dev/zero | tr '\\0' x\n"), small);
    CHECK(f.transport == Transport::TooLarge && f.detail == "2000 bytes", "a body over maxBytes: %d '%s'", int(f.transport), f.detail.c_str());
    // Past the pipe's cap (the body and 64 KB of headers): killed while still sending.
    f = Run(Curl("flood", "printf 'HTTP/1.1 200 OK\\r\\n\\r\\n'; exec cat /dev/zero\n"), small);
    CHECK(f.transport == Transport::TooLarge && f.detail.starts_with("over "), "an endless answer: %d '%s'", int(f.transport), f.detail.c_str());
}

// curl(1)'s exit codes, as the check tells them apart.
static void ExitCodes()
{
    struct E { int code; Transport transport; };
    const E codes[] = {
        { 6, Transport::Offline }, { 7, Transport::Offline }, { 52, Transport::Offline }, { 56, Transport::Offline },
        { 28, Transport::Timeout },
        { 35, Transport::Secure }, { 60, Transport::Secure }, { 77, Transport::Secure },
        { 63, Transport::TooLarge },
        { 1, Transport::Garbled }, { 8, Transport::Garbled },
        { 3, Transport::Failed }, { 22, Transport::Failed }, { 99, Transport::Failed },
    };
    for (const E& e : codes)
    {
        Fetched f = Run(Curl("exit", "exit " + std::to_string(e.code) + "\n"));
        CHECK(f.transport == e.transport && f.detail == "curl exit " + std::to_string(e.code), "exit %d: transport %d, '%s'", e.code,
            int(f.transport), f.detail.c_str());
    }
    Fetched f = Run(Curl("signal", "kill -TERM $$\nsleep 5\n"));
    CHECK(f.transport == Transport::Failed && f.detail == "curl ended by signal 15", "a signal: %d '%s'", int(f.transport), f.detail.c_str());

    // No curl: the player's message.
    for (const char* missing : { "/nonexistent/curl", "curl", "/" })
    {
        setenv("NFSMW_UPDATE_CURL", missing, 1);
        f = Fetch({ .url = kApiUrl, .userAgent = "SpeedBreaker/0.0.0-test" });
        CHECK(f.transport == Transport::Unavailable && f.detail.starts_with("Checking needs curl"), "'%s': %d '%s'", missing,
            int(f.transport), f.detail.c_str());
    }
}

// The deadline: the timeout plus two seconds, whatever curl does.
static void Deadline()
{
    Request quick;
    quick.timeoutSeconds = 0.5;
    double seconds = 0;
    Fetched f = Run(Curl("hang", "exec sleep 30\n"), quick, seconds);
    CHECK(f.transport == Transport::Timeout && f.detail == "curl was still running after the deadline", "a hang: %d '%s'",
        int(f.transport), f.detail.c_str());
    CHECK(seconds >= 2.4 && seconds < 4.5, "it gave up at the deadline (0.5 + 2 s): %.2f s", seconds);

    // Its output closed, the process still there: two seconds to exit, then the kill.
    f = Run(Curl("lingers", std::string(kAnswer) + "exec >&-\nexec sleep 30\n"), quick, seconds);
    CHECK(f.transport == Transport::Timeout && f.detail == "curl didn't exit", "no exit: %d '%s'", int(f.transport), f.detail.c_str());
    CHECK(seconds >= 1.9 && seconds < 4.0, "two seconds for it to exit: %.2f s", seconds);

    // Slow but within the deadline: an answer.
    f = Run(Curl("slow", "sleep 1\n" + std::string(kAnswer)), {}, seconds);
    CHECK(f.transport == Transport::Ok && f.response.status == 200, "a slow answer: %d '%s'", int(f.transport), f.detail.c_str());
}

// What curl gets: the fixed arguments, a small environment, /dev/null for
// stdin and stderr, and none of the game's other descriptors.
static void Process()
{
    const std::string args = g_dir + "/args", env = g_dir + "/env", fds = g_dir + "/fds", in = g_dir + "/stdin";
    const std::string curl = Curl("record", "for a in \"$@\"; do echo \"$a\"; done > '" + args + "'\nenv > '" + env + "'\nls /proc/$$/fd > '" +
                                                fds + "'\nreadlink /proc/$$/fd/0 > '" + in + "'\n" + kAnswer);
    setenv("https_proxy", "http://proxy.test:3128", 1);
    setenv("LD_LIBRARY_PATH", "/sb-test/lib", 1);
    setenv("SB_TEST_PRIVATE", "1", 1);
    // A descriptor of the game's without close-on-exec, at a number of its own.
    const int leak = open("/dev/null", O_RDONLY);
    const bool leaked = leak >= 0 && dup2(leak, 42) == 42;
    Fetched f = Run(curl, { .headers = { { "Accept", "application/vnd.github+json" } } });
    CHECK(f.transport == Transport::Ok, "recorded: %d '%s'", int(f.transport), f.detail.c_str());

    const std::vector<std::string> a = Lines(Read(args));
    auto after = [&a](const char* option) {
        auto it = std::find(a.begin(), a.end(), option);
        return it == a.end() || it + 1 == a.end() ? std::string("<none>") : *(it + 1);
    };
    CHECK(!a.empty() && a[0] == "-q", "-q first (no ~/.curlrc): '%s'", a.empty() ? "" : a[0].c_str());
    CHECK(after("--proto") == "=https", "https only: '%s'", after("--proto").c_str());
    CHECK(after("--max-time") == "10" && after("--connect-timeout") == "10", "the timeout: '%s'", after("--max-time").c_str());
    CHECK(after("--max-filesize") == std::to_string(kMaxResponseBytes), "the cap: '%s'", after("--max-filesize").c_str());
    CHECK(after("--user-agent") == "SpeedBreaker/0.0.0-test", "the User-Agent: '%s'", after("--user-agent").c_str());
    CHECK(after("--header") == "Accept: application/vnd.github+json", "the header: '%s'", after("--header").c_str());
    CHECK(a.size() >= 2 && a[a.size() - 2] == "--url" && a.back() == kApiUrl, "the URL last: '%s'", a.empty() ? "" : a.back().c_str());
    CHECK(std::find(a.begin(), a.end(), "-L") == a.end() && std::find(a.begin(), a.end(), "--location") == a.end(), "no redirects followed");

    const std::string e = Read(env);
    CHECK(e.find("https_proxy=http://proxy.test:3128\n") != std::string::npos, "the proxy passed");
    CHECK(e.find("PATH=") != std::string::npos, "PATH passed");
    CHECK(e.find("LD_LIBRARY_PATH=/sb-test/lib") == std::string::npos, "not the game's library path");
    CHECK(e.find("SB_TEST_PRIVATE") == std::string::npos && e.find("NFSMW_UPDATE_CURL") == std::string::npos, "nothing else");
    CHECK(Read(in) == "/dev/null\n", "stdin is /dev/null: '%s'", Read(in).c_str());
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    const std::vector<std::string> open = Lines(Read(fds));
    CHECK(leaked && std::find(open.begin(), open.end(), "42") == open.end(), "the game's descriptor 42 not inherited");
#endif
    if (leak >= 0)
        close(leak);
    close(42);
    unsetenv("https_proxy");
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("SB_TEST_PRIVATE");

    // The test hook's local server: plain http allowed.
    f = Run(curl, { .url = "http://127.0.0.1:8080/latest", .allowHttp = true });
    CHECK(f.transport == Transport::Ok, "recorded again: %d '%s'", int(f.transport), f.detail.c_str());
    const std::vector<std::string> b = Lines(Read(args));
    auto it = std::find(b.begin(), b.end(), "--proto");
    CHECK(it != b.end() && it + 1 != b.end() && *(it + 1) == "=http,https", "allowHttp: http too");
}

int main()
{
    char scratch[] = "/tmp/update_fetch_test.XXXXXX";
    if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp)
        g_dir = std::string(tmp) + "/update_fetch_test.XXXXXX";
    else
        g_dir = scratch;
    if (!mkdtemp(g_dir.data()))
    {
        printf("no scratch folder\n");
        return 1;
    }
    Answers();
    ExitCodes();
    Deadline();
    Process();
    std::string rm = "rm -rf '" + g_dir + "'";
    if (std::system(rm.c_str()) != 0)
        printf("couldn't remove %s\n", g_dir.c_str());
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
