// wabcut_crash_helper: installs Wabcut's crash handler the way main() does, logs lines with private paths, keys and an
// e-mail address, records diagnostics, then ends the way it is told (tests/app/test_crash_safety.cpp):
//
//   wabcut_crash_helper <folder> segv|abort|throw|stack|exit|clean
//
//   segv    an access violation          abort   abort()               throw   an exception nobody catches
//   stack   a stack overflow             exit    returns without a clean exit (killed / power loss stand-in)
//   clean   a clean exit

#include "CrashHandler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <wabcut/platform/DllSearch.h>

namespace {

volatile int g_sink = 0;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4717) // recursive on all paths: the stack overflow is the point
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
#endif
int recurse(int depth) {
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    frame[sizeof frame - 1] = static_cast<char>(depth);
    return recurse(depth + 1) + frame[0] + frame[sizeof frame - 1];
}
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace

int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    if (argc < 3) {
        std::fprintf(stderr, "usage: wabcut_crash_helper <folder> segv|abort|throw|stack|exit|clean\n");
        return 2;
    }
    const std::string mode = argv[2];
    wabcut::crash::CrashOptions options;
    options.folder = argv[1];
    options.version = "9.9.9 (test)";
    options.logLines = 50;
    if (!wabcut::crash::install(options)) {
        std::fprintf(stderr, "crash handler not installed\n");
        return 3;
    }
    wabcut::crash::setDiagnostic("gpu", "Test GPU 3000");
    wabcut::crash::setDiagnostic("driver", "1.2.3.4");
    wabcut::crash::setDiagnostic("graphicsApi", "D3D11");
    wabcut::crash::setDiagnostic("decode", "d3d11va");
    wabcut::crash::log("opened C:\\Users\\alice\\Videos\\Wedding Day.mp4 for the timeline");
    wabcut::crash::log("relinked /home/alice/footage/interview.mov");
    wabcut::crash::log("provider key sk-test-0123456789abcdef0123456789 accepted");
    wabcut::crash::log("Authorization: Bearer abc.def.ghi");
    wabcut::crash::log("signed in as alice@example.com");
    if (const char* secret = std::getenv("WABCUT_TEST_API_KEY")) {
        wabcut::crash::log(std::string("the environment key is ") + secret + " today");
    }
    wabcut::crash::log("plain line 42");

    if (mode == "segv") {
        volatile int* p = nullptr;
        *p = 1;
    } else if (mode == "abort") {
        std::abort();
    } else if (mode == "throw") {
        throw std::runtime_error("nobody catches this");
    } else if (mode == "stack") {
        g_sink = recurse(0);
    } else if (mode == "exit") {
        return 0; // no clean exit: the session file stays
    } else if (mode == "clean") {
        wabcut::crash::markCleanExit();
        return 0;
    }
    return 4;
}
