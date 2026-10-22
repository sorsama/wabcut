// A stand-in for a camera RAW helper (media/RawHelper.h) in the media tests: it "decodes" a .fakeraw file - one line
// "<width> <height> <rate num> <rate den> <frames>" - into rgba64le frames whose red is frame index x 20 (8-bit terms),
// following the helper protocol: a JSON header line, then one frame per index read from stdin, until "quit".
// A file whose first word is "error" makes it refuse with a message.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        std::cout << "{\"error\": \"no file\"}\n" << std::flush;
        return 1;
    }
    std::ifstream in(argv[1]);
    std::string first;
    in >> first;
    if (!in || first == "error") {
        std::cout << "{\"error\": \"this file is not supported by the fake helper\"}\n" << std::flush;
        return 1;
    }
    const int width = std::stoi(first);
    int height = 0, num = 0, den = 0, frames = 0;
    in >> height >> num >> den >> frames;
    std::cout << "{\"width\": " << width << ", \"height\": " << height << ", \"rateNum\": " << num << ", \"rateDen\": "
              << den << ", \"frames\": " << frames << ", \"pixelFormat\": \"rgba64le\", \"colorTransfer\": \"srgb\"}\n"
              << std::flush;
    std::vector<unsigned char> frame(static_cast<std::size_t>(width) * height * 8);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") {
            break;
        }
        const int index = std::stoi(line);
        const unsigned value = static_cast<unsigned>((index * 20) % 256) << 8;
        for (std::size_t p = 0; p < frame.size(); p += 8) {
            frame[p + 0] = static_cast<unsigned char>(value & 0xFF); // R, little endian
            frame[p + 1] = static_cast<unsigned char>(value >> 8);
            frame[p + 2] = 0;
            frame[p + 3] = 0; // G
            frame[p + 4] = 0;
            frame[p + 5] = 0; // B
            frame[p + 6] = 0xFF;
            frame[p + 7] = 0xFF; // A
        }
        std::cout.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
        std::cout.flush();
    }
    return 0;
}
