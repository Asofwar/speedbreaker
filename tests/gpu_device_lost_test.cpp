// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// A lost GPU device (VK_ERROR_DEVICE_LOST) is only ever a result, so every
// Vulkan call that waits for the GPU, submits to it or presents from it has
// to look at what it returned. v0.1.4 didn't: the renderer's completion
// thread took a lost device's fence status for "not yet" and spun on it at
// 100% CPU while the game hung without a word; the presenter dropped its
// fence waits' and submit's results, and drew to the image index an acquire
// that returned VK_ERROR_SURFACE_LOST_KHR or VK_ERROR_DEVICE_LOST never
// wrote. The arguments are runtime/gpu/renderer.cpp and
// runtime/video/presenter.cpp (ctest passes them). In each, every such call
// must be part of an expression (not a statement of its own, its result
// dropped), and must be followed closely by what handles a lost device:
// video::DeviceLost, a comparison with VK_ERROR_DEVICE_LOST,
// ClassifySurfaceResult (the presenter's acquire and present), or the
// renderer's Check, which is checked to hand it to video::DeviceLost.
// v0.1.4's own lines, planted, must each be caught. From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG tests/gpu_device_lost_test.cpp -o build/gpu_device_lost_test
//   build/gpu_device_lost_test runtime/gpu/renderer.cpp runtime/video/presenter.cpp
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The calls that wait for, submit to or present from the GPU
// (s_waitForPresent: the presenter's vkWaitForPresentKHR).
static const char* const kCalls[] = { "vkWaitForFences", "vkGetFenceStatus", "vkQueueSubmit", "vkQueueWaitIdle", "vkDeviceWaitIdle",
    "vkQueuePresentKHR", "vkAcquireNextImageKHR", "vkWaitSemaphores", "s_waitForPresent" };

// How many lines after a call its handling may come (the renderer's submit
// is checked once the queue lock's scope has closed, eight lines on).
constexpr int kHandledWithin = 10;

static bool IdentifierChar(char c)
{
    return isalnum(uint8_t(c)) || c == '_';
}

// The source without comments, and with string and character literals (raw
// strings too: the renderer's GLSL) blanked, so that only code is searched.
// Newlines are kept, so line numbers stay.
static std::string CodeOnly(const std::string& s)
{
    std::string out = s;
    size_t i = 0;
    auto blank = [&](size_t from, size_t to) {
        for (size_t k = from; k < to && k < out.size(); k++)
            if (out[k] != '\n')
                out[k] = ' ';
    };
    while (i < s.size())
    {
        if (s.compare(i, 2, "//") == 0)
        {
            size_t end = s.find('\n', i);
            end = end == std::string::npos ? s.size() : end;
            blank(i, end);
            i = end;
        }
        else if (s.compare(i, 2, "/*") == 0)
        {
            size_t end = s.find("*/", i + 2);
            end = end == std::string::npos ? s.size() : end + 2;
            blank(i, end);
            i = end;
        }
        else if (s[i] == 'R' && i + 1 < s.size() && s[i + 1] == '"' && (i == 0 || !IdentifierChar(s[i - 1])))
        {
            // R"delim( ... )delim"
            size_t open = s.find('(', i + 2);
            if (open == std::string::npos)
                break;
            const std::string close = ")" + s.substr(i + 2, open - i - 2) + "\"";
            size_t end = s.find(close, open);
            end = end == std::string::npos ? s.size() : end + close.size();
            blank(i, end);
            i = end;
        }
        else if (s[i] == '"' || (s[i] == '\'' && !(i > 0 && isxdigit(uint8_t(s[i - 1])))))  // (not a digit separator: 1'000'000)
        {
            const char quote = s[i];
            size_t k = i + 1;
            while (k < s.size() && s[k] != quote && s[k] != '\n')
                k += s[k] == '\\' ? 2 : 1;
            blank(i, k + 1);
            i = k + 1;
        }
        else
            i++;
    }
    return out;
}

struct Finding
{
    int line;
    std::string call, why;
};

// Every call in `source` that drops its result or isn't followed by what
// handles a lost device (`markers`).
static std::vector<Finding> Scan(const std::string& source, const std::vector<std::string>& markers, int* calls = nullptr)
{
    const std::string code = CodeOnly(source);
    std::vector<Finding> found;
    int seen = 0;
    for (const char* name : kCalls)
    {
        const std::string call = std::string(name) + "(";
        for (size_t at = code.find(call); at != std::string::npos; at = code.find(call, at + 1))
        {
            if (at > 0 && IdentifierChar(code[at - 1]))
                continue;  // part of a longer name
            seen++;
            const int line = 1 + int(std::count(code.begin(), code.begin() + long(at), '\n'));
            // What comes before it: nothing of an expression (a statement's
            // start, a control statement's header, `else`, a cast to void)
            // means the result is dropped.
            size_t p = at;
            while (p > 0 && isspace(uint8_t(code[p - 1])))
                p--;
            const char before = p > 0 ? code[p - 1] : ';';
            const bool afterElse = p >= 4 && code.compare(p - 4, 4, "else") == 0 && (p == 4 || !IdentifierChar(code[p - 5]));
            if (before == ';' || before == '{' || before == '}' || before == ')' || afterElse)
            {
                found.push_back({ line, name, "its result is dropped" });
                continue;
            }
            // What handles a lost device, in this line (from its start: the
            // renderer's Check(vkQueueSubmit(...))) or the next few.
            const size_t start = code.rfind('\n', at) == std::string::npos ? 0 : code.rfind('\n', at) + 1;
            size_t end = at;
            for (int n = 0; n <= kHandledWithin && end != std::string::npos; n++)
                end = code.find('\n', end + 1);
            const std::string window = code.substr(start, end == std::string::npos ? std::string::npos : end - start);
            bool handled = false;
            for (const std::string& m : markers)
                for (size_t k = window.find(m); k != std::string::npos && !handled; k = window.find(m, k + 1))
                    handled = k == 0 || !IdentifierChar(window[k - 1]);  // the name itself, not the end of a longer one
            if (!handled)
                found.push_back({ line, name, "nothing after it handles VK_ERROR_DEVICE_LOST" });
        }
    }
    if (calls)
        *calls = seen;
    return found;
}

static const std::vector<std::string> kPresenterMarkers = { "VK_ERROR_DEVICE_LOST", "DeviceLost(", "ClassifySurfaceResult(" };
static const std::vector<std::string> kRendererMarkers = { "VK_ERROR_DEVICE_LOST", "DeviceLost(", "Check(" };

static std::string Read(const char* path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// v0.1.4's lines (and the forms that replaced them): each old one is caught,
// each new one passes.
static void Planted()
{
    struct Case
    {
        const char* what;
        const char* code;
        bool caught;
    };
    const Case cases[] = {
        { "the presenter's fence wait, its result dropped",
            "        vkWaitForFences(s_device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);\n        if (f.timed)\n", true },
        { "the presenter's submit, its result dropped", "        vkQueueSubmit(s_queue, 1, &si, f.inFlight);\n        frontLock.unlock();\n", true },
        { "a wait after an if, dropped",
            "        if (gameCapture.buffer)\n            vkWaitForFences(s_device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);\n", true },
        { "a wait cast to void", "        (void)vkQueueWaitIdle(s_queue);\n", true },
        { "the completion thread's fence status: 'not yet' for a lost device",
            "                    bool finished = vkGetFenceStatus(s_dev, sub.fence) == VK_SUCCESS;\n                    bool watching = false;\n"
            "                    {\n                    }\n                    if (finished)\n                        break;\n",
            true },
        { "the completion thread's wait: anything but success, round again",
            "                        VkResult r = vkWaitForFences(s_dev, 1, &sub.fence, VK_TRUE, 1'000'000);\n"
            "                        if (r == VK_SUCCESS)\n                            break;\n                    }\n                }\n",
            true },
        { "the acquire, two results of many handled",
            "        VkResult acquire = vkAcquireNextImageKHR(s_device, s_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);\n"
            "        if (acquire == VK_SUBOPTIMAL_KHR)\n            s_swapPolicy.Reported(SurfaceResult::Suboptimal);\n"
            "        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)\n        {\n            return true;\n        }\n"
            "        vkResetFences(s_device, 1, &f.inFlight);\n",
            true },
        { "a fence wait that says a lost device",
            "        if (vkWaitForFences(s_device, 1, &f.inFlight, VK_TRUE, UINT64_MAX) == VK_ERROR_DEVICE_LOST)\n            DeviceLost(\"x\");\n",
            false },
        { "the acquire, classified",
            "        VkResult acquire = vkAcquireNextImageKHR(s_device, s_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);\n"
            "        const SurfaceOutcome acquired = ClassifySurfaceResult(acquire);\n",
            false },
        { "a call named only in a comment or a string",
            "        // vkWaitForFences(s_device, ...);\n        fprintf(stderr, \"vkQueueSubmit(%d)\\n\", 1);\n", false },
    };
    for (const Case& c : cases)
    {
        const bool caught = !Scan(c.code, kPresenterMarkers).empty();
        printf("planted: %-66s %s\n", c.what, caught ? "caught" : "passes");
        CHECK(caught == c.caught, "%s: %s", c.what, c.caught ? "not caught" : "caught, but it handles the result");
    }
}

static void CheckFile(const char* path, const std::vector<std::string>& markers, int fewest)
{
    const std::string source = Read(path);
    CHECK(!source.empty(), "can't read %s", path);
    if (source.empty())
        return;
    int calls = 0;
    const std::vector<Finding> found = Scan(source, markers, &calls);
    printf("%s: %d calls that wait, submit or present; %zu unhandled\n", path, calls, found.size());
    // Fewer than it has: the names moved, and the scan would pass on nothing.
    CHECK(calls >= fewest, "%s: %d calls found, at least %d expected", path, calls, fewest);
    for (const Finding& f : found)
        CHECK(false, "%s:%d: %s: %s", path, f.line, f.call.c_str(), f.why.c_str());
}

// The renderer's Check hands a lost device to video::DeviceLost (the
// renderer's sites count Check as handling it), before its abort().
static void RendererCheck(const char* path)
{
    const std::string code = CodeOnly(Read(path));
    const size_t at = code.find("void Check(VkResult r, const char* what)");
    CHECK(at != std::string::npos, "%s: Check not found", path);
    if (at == std::string::npos)
        return;
    const std::string body = code.substr(at, code.find("\n        }\n", at) - at);
    const size_t lost = body.find("if (r == VK_ERROR_DEVICE_LOST)");
    const size_t handed = body.find("video::DeviceLost(what);");
    const size_t aborted = body.find("abort();");
    CHECK(lost != std::string::npos && handed != std::string::npos && aborted != std::string::npos && lost < handed && handed < aborted,
        "%s: Check hands VK_ERROR_DEVICE_LOST to video::DeviceLost before it aborts: %s", path, body.c_str());
}

// The presenter draws to the acquired image only when there is one, and its
// frame loop starts by looking whether another thread lost the device (a
// game that stopped sending frames brings the main thread to no Vulkan call).
static void PresenterFrame(const char* path)
{
    const std::string code = CodeOnly(Read(path));
    const size_t acquire = code.find("vkAcquireNextImageKHR(");
    const size_t classified = code.find("ClassifySurfaceResult(acquire)", acquire);
    const size_t skipped = code.find("if (!acquired.image)", acquire);
    const size_t used = code.find("s_swapImages[imageIndex]", acquire);
    CHECK(acquire != std::string::npos && classified != std::string::npos && skipped != std::string::npos && used != std::string::npos &&
        classified < skipped && skipped < used, "%s: the acquire is classified, and a frame without an image returns before the index is used",
        path);
    const size_t runFrame = code.find("bool RunFrame()");
    const size_t look = code.find("if (s_deviceLost.load(std::memory_order_acquire) != 0)", runFrame);
    const size_t events = code.find("SDL_PollEvent(", runFrame);
    CHECK(runFrame != std::string::npos && look != std::string::npos && look < events,
        "%s: RunFrame looks for a device another thread lost before anything else", path);
}

int main(int argc, char** argv)
{
    Planted();
    if (argc > 2)
    {
        CheckFile(argv[1], kRendererMarkers, 3);
        RendererCheck(argv[1]);
        CheckFile(argv[2], kPresenterMarkers, 9);
        PresenterFrame(argv[2]);
    }
    else
        printf("(renderer.cpp and presenter.cpp not given: their checks skipped)\n");
    if (g_failures)
    {
        printf("gpu_device_lost_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_device_lost_test: all passed\n");
    return 0;
}
