// A stand-in for Ghostscript (tests of render/VectorImage.h): records its arguments, answers -sDEVICE=bbox with a box on
// standard error and -sDEVICE=pngalpha with the PNG file named by WABCUT_FAKE_GS_PNG on standard output.
//   WABCUT_FAKE_GS_LOG   a file the arguments are appended to (one per line, "--" after each run)
//   WABCUT_FAKE_GS_PNG   the picture to send
//   WABCUT_FAKE_GS_MODE  "fail": a PostScript error and exit code 1

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    bool bbox = false;
    bool png = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        bbox = bbox || a == "-sDEVICE=bbox";
        png = png || a == "-sDEVICE=pngalpha";
    }
    if (const char* log = std::getenv("WABCUT_FAKE_GS_LOG")) {
        std::ofstream out(log, std::ios::app);
        for (int i = 1; i < argc; ++i) {
            out << argv[i] << '\n';
        }
        out << "--\n";
    }
    const char* mode = std::getenv("WABCUT_FAKE_GS_MODE");
    if (mode && std::string(mode) == "fail") {
        std::fputs("Error: /undefined in foo\n", stderr);
        return 1;
    }
    if (bbox) {
        std::fputs("%%BoundingBox: 0 0 200 100\n%%HiResBoundingBox: 0.000000 0.000000 200.000000 100.000000\n", stderr);
        return 0;
    }
    if (png) {
        const char* file = std::getenv("WABCUT_FAKE_GS_PNG");
        std::ifstream in(file ? file : "", std::ios::binary);
        if (!in) {
            std::fputs("fake gs: no picture to send\n", stderr);
            return 2;
        }
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::fwrite(bytes.data(), 1, bytes.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
    return 3;
}
