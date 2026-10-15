// Small lifecycle helper; no LLM and no external network. Owns the requested loopback port until terminated.
#include <wabcut/platform/DllSearch.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
int main(int argc, char** argv) {
    wabcut::platform::hardenDllSearch(); // first: no DLL from the current directory or PATH (S5-P2-5)
    int port = 0; std::string mode;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string name = argv[i];
        if (name == "--port") port = std::atoi(argv[i + 1]);
        if (name == "--model") mode = argv[i + 1];
        if (name == "--jinja") --i;
    }
    if (mode.find("crash") != std::string::npos) return 3;
    // AI-15: "record-args" weights write the command line (without the key) next to them, for the launch-argument test.
    if (mode.find("record-args") != std::string::npos) {
        std::ofstream out(mode + ".args", std::ios::binary);
        for (int i = 1; i < argc; ++i) out << (i > 1 && std::string(argv[i - 1]) == "--api-key" ? "<key>" : argv[i]) << '\n';
    }
#ifdef _WIN32
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 4;
    auto fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return 5;
    const BOOL exclusive = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    const int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return 5;
#endif
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<unsigned short>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 1) != 0) return 6;
    for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
