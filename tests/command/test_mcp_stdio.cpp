// End to end: spawn `wabcut-cli mcp` and talk to it over real stdin/stdout pipes, like an MCP client does.

#include "McpHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>

#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// A child process with its stdin and stdout piped to us and its stderr discarded.
class ChildProcess {
public:
    ChildProcess(const std::filesystem::path& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE inRead = nullptr;
        HANDLE outWrite = nullptr;
        REQUIRE(CreatePipe(&inRead, &m_in, &inherit, 0));
        REQUIRE(CreatePipe(&m_out, &outWrite, &inherit, 0));
        SetHandleInformation(m_in, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_out, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0,
                                 nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inRead;
        startup.hStdOutput = outWrite;
        startup.hStdError = nul;
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        for (const std::string& arg : args) {
            command += L" \"" + std::filesystem::path(arg).wstring() + L"\"";
        }
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            nullptr, &startup, &process);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        CloseHandle(nul);
        REQUIRE(started);
        CloseHandle(process.hThread);
        m_process = process.hProcess;
#else
        int in[2];
        int out[2];
        REQUIRE(pipe(in) == 0);
        REQUIRE(pipe(out) == 0);
        m_pid = fork();
        REQUIRE(m_pid >= 0);
        if (m_pid == 0) {
            dup2(in[0], 0);
            dup2(out[1], 1);
            const int nul = open("/dev/null", O_WRONLY);
            dup2(nul, 2);
            close(in[0]);
            close(in[1]);
            close(out[0]);
            close(out[1]);
            std::vector<std::string> all{exe.string()};
            all.insert(all.end(), args.begin(), args.end());
            std::vector<char*> argv;
            for (std::string& a : all) {
                argv.push_back(a.data());
            }
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
            _exit(127);
        }
        close(in[0]);
        close(out[1]);
        m_in = in[1];
        m_out = out[0];
#endif
    }

    ~ChildProcess() {
        closeInput();
#ifdef _WIN32
        if (m_process) {
            if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(m_process, 1);
            }
            CloseHandle(m_process);
        }
        if (m_out) {
            CloseHandle(m_out);
        }
#else
        if (m_out >= 0) {
            close(m_out);
        }
        if (m_pid > 0 && !m_waited) {
            kill(m_pid, SIGKILL);
            waitpid(m_pid, nullptr, 0);
        }
#endif
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void writeLine(const std::string& line) {
        const std::string data = line + "\n";
#ifdef _WIN32
        DWORD written = 0;
        REQUIRE(WriteFile(m_in, data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
#else
        REQUIRE(write(m_in, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
#endif
    }

    /// The next stdout line, or nullopt once the child closed stdout.
    std::optional<std::string> readLine() {
        for (;;) {
            if (const auto nl = m_buffer.find('\n'); nl != std::string::npos) {
                std::string line = m_buffer.substr(0, nl);
                m_buffer.erase(0, nl + 1);
                return line;
            }
            char chunk[4096];
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(m_out, chunk, sizeof chunk, &n, nullptr) || n == 0) {
                return std::nullopt;
            }
#else
            const ssize_t n = read(m_out, chunk, sizeof chunk);
            if (n <= 0) {
                return std::nullopt;
            }
#endif
            m_buffer.append(chunk, static_cast<std::size_t>(n));
        }
    }

    Json reply(const std::string& request) {
        writeLine(request);
        const auto line = readLine();
        REQUIRE(line);
        INFO(*line);
        CHECK(line->find('\r') == std::string::npos); // binary stdout: no CRLF
        return Json::parse(*line);
    }

    void closeInput() {
#ifdef _WIN32
        if (m_in) {
            CloseHandle(m_in);
            m_in = nullptr;
        }
#else
        if (m_in >= 0) {
            close(m_in);
            m_in = -1;
        }
#endif
    }

    int wait() {
        closeInput();
#ifdef _WIN32
        REQUIRE(WaitForSingleObject(m_process, 60'000) == WAIT_OBJECT_0);
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        return static_cast<int>(code);
#else
        int status = 0;
        waitpid(m_pid, &status, 0);
        m_waited = true;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

private:
    std::string m_buffer;
#ifdef _WIN32
    HANDLE m_in = nullptr;
    HANDLE m_out = nullptr;
    HANDLE m_process = nullptr;
#else
    int m_in = -1;
    int m_out = -1;
    pid_t m_pid = -1;
    bool m_waited = false;
#endif
};

std::string initialize() {
    return plainRequest(1, "initialize",
                        Json{{"protocolVersion", "2025-11-25"},
                             {"capabilities", Json::object()},
                             {"clientInfo", Json{{"name", "stdio-test"}, {"version", "1"}}}});
}

std::filesystem::path projectFile(const char* name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command" / "mcp";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    for (int i = 0; i <= 5; ++i) {
        std::filesystem::remove(i == 0 ? file : io::backupPath(file, i));
    }
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    REQUIRE(io::saveProject(tl.linkMatching().state, file));
    return file;
}

} // namespace

TEST_CASE("wabcut-cli mcp over real stdio: read-only session", "[command][mcp][stdio]") {
    const auto file = projectFile("stdio-read.wabcut");
    ChildProcess child(WABCUT_CLI_PATH, {"mcp", "--project", file.string()});

    const Json init = child.reply(initialize());
    REQUIRE(init.contains("result"));
    CHECK(init["result"]["protocolVersion"] == "2025-11-25");
    child.writeLine(notification("notifications/initialized"));

    const Json tools = child.reply(modernRequest(2, "tools/list"));
    CHECK(tools["result"]["tools"].size() == readCommandCount(wabcut::ai::edit::editingCommands())); // + transcript.*
    const Json summary = child.reply(callLine(3, "sequence_summary"));
    CHECK(summary["result"]["structuredContent"]["data"]["tracks"].size() == 2);
    const Json refused = child.reply(callLine(4, "timeline_rippleDelete", Json{{"clips", Json::array({1})}}));
    CHECK(refused["result"]["isError"] == true);
    CHECK(child.reply("{oops")["error"]["code"] == -32700);

    CHECK(child.wait() == 0);
    CHECK_FALSE(child.readLine()); // nothing after the last reply
}

TEST_CASE("wabcut-cli mcp over real stdio: edit and save on exit", "[command][mcp][stdio]") {
    const auto file = projectFile("stdio-edit.wabcut");
    ChildProcess child(WABCUT_CLI_PATH, {"mcp", "--project", file.string(), "--allow-edit", "--save-on-exit"});
    REQUIRE(child.reply(initialize()).contains("result"));
    child.writeLine(notification("notifications/initialized"));
    const Json edit = child.reply(
        callLine(2, "timeline_rippleDelete", Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", 0}}})}}));
    INFO(edit.dump());
    CHECK(edit["result"]["structuredContent"]["committed"] == true);
    CHECK(child.wait() == 0);

    const auto saved = io::loadProject(file);
    REQUIRE(saved);
    const Clip& first = saved->sequences[0]->videoTracks[0]->clips[0];
    CHECK(first.name() == "B");
    CHECK(first.start() == f(2));
}

TEST_CASE("wabcut-cli mcp exits with 2 when the project cannot be loaded", "[command][mcp][stdio]") {
    ChildProcess child(WABCUT_CLI_PATH, {"mcp", "--project", (std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) /
                                                              "command" / "mcp" / "missing.wabcut")
                                                                 .string()});
    CHECK(child.wait() == 2);
    CHECK_FALSE(child.readLine()); // stdout stays clean
}
