// ChildProcess against the scripted fake helper (tests/ai/fake_infer.cpp). Part of the core build.

#include "AiTestUtil.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/InferProtocol.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/platform/HelperProcess.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h> // GetPriorityClass
#endif

using namespace wabcut;
using namespace wabcut::ai;
using namespace std::chrono_literals;

TEST_CASE("ChildProcess: line I/O, timeouts, exit codes and kill", "[ai][process]") {
    ChildProcessOptions options;
    options.executable = WABCUT_FAKE_INFER;
    auto child = ChildProcess::spawn(options);
    REQUIRE(child);
    std::string line;
    CHECK((*child)->readLine(line, 50ms) == ChildProcess::ReadStatus::Timeout);
    REQUIRE((*child)->writeLine(infer::encode(infer::Request{infer::Hello{}})));
    REQUIRE((*child)->readLine(line, 10s) == ChildProcess::ReadStatus::Line);
    auto hello = infer::decodeResponse(line);
    REQUIRE(hello);
    CHECK(std::get<infer::HelloReply>(*hello).engine == "fake");
    (*child)->closeInput(); // end of input: the helper exits
    CHECK((*child)->wait(10s) == 0);
    CHECK((*child)->readLine(line, 1s) == ChildProcess::ReadStatus::Closed);
    CHECK_FALSE((*child)->writeLine("{}"));

    options.arguments = {"--mode", "exit-at-start"};
    auto failing = ChildProcess::spawn(options);
    REQUIRE(failing);
    CHECK((*failing)->wait(10s) == 7);
    CHECK(ChildProcess::describeExit(7) == "exit code 7");

    options.arguments = {"--mode", "hang-on-transcribe"};
    auto hung = ChildProcess::spawn(options);
    REQUIRE(hung);
    CHECK_FALSE((*hung)->exitCode());
    (*hung)->kill();
    CHECK((*hung)->exitCode());

    ChildProcessOptions missing;
    missing.executable = test::freshDir("nohelper") / "no-such-helper.exe";
    const auto none = ChildProcess::spawn(missing);
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);
}

#ifdef _WIN32
// Review wave 5, S5-P2-5. A helper got the editor's whole environment, so a provider key set as an environment variable
// reached the script host and every plug-in's process, and the editor's current directory (a project folder opened from
// Explorer, where a planted DLL is found). Fails today: the helper sees both keys and runs in the test's directory.
TEST_CASE("ChildProcess: a helper gets a minimal environment without keys and runs in its own folder", "[ai][process]") {
    const auto dir = test::freshDir("child-environment");
    REQUIRE(_putenv_s("OPENAI_API_KEY", "sk-parent-secret") == 0);
    REQUIRE(_putenv_s("WABCUT_OPENAI_API_KEY", "sk-parent-override") == 0);
    REQUIRE(_putenv_s("WABCUT_CHILD_SETTING", "kept") == 0);
    const std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::current_path(dir); // the editor's current directory, which the helper must not get

    ChildProcessOptions options;
    options.executable = WABCUT_FAKE_INFER;
    options.arguments = {"--mode", "print-environment"};
    auto child = ChildProcess::spawn(options);
    REQUIRE(child);
    std::map<std::string, std::string> variables;
    std::string cwd;
    std::string line;
    while ((*child)->readLine(line, 10s) == ChildProcess::ReadStatus::Line) {
        if (line.starts_with("env ")) {
            const std::size_t equals = line.find('=', 5);
            std::string name = line.substr(4, equals == std::string::npos ? std::string::npos : equals - 4);
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            variables[name] = equals == std::string::npos ? std::string() : line.substr(equals + 1);
        } else if (line.starts_with("cwd ")) {
            cwd = line.substr(4);
        }
    }
    CHECK((*child)->wait(10s) == 0);
    std::filesystem::current_path(before);
    _putenv_s("OPENAI_API_KEY", "");
    _putenv_s("WABCUT_OPENAI_API_KEY", "");
    _putenv_s("WABCUT_CHILD_SETTING", "");

    CHECK_FALSE(variables.contains("OPENAI_API_KEY"));
    CHECK_FALSE(variables.contains("WABCUT_OPENAI_API_KEY"));
    CHECK(variables["WABCUT_CHILD_SETTING"] == "kept"); // Wabcut's own settings still reach it
    CHECK(variables.contains("SYSTEMROOT"));
    CHECK(variables.contains("TEMP"));
    REQUIRE(variables.contains("PATH"));
    const std::filesystem::path helperDir = std::filesystem::absolute(std::filesystem::path(WABCUT_FAKE_INFER)).parent_path();
    CHECK(variables["PATH"].starts_with(pathToUtf8(helperDir))); // its own folder first
    REQUIRE_FALSE(cwd.empty());
    CHECK(std::filesystem::equivalent(pathFromUtf8(cwd), helperDir));

    // A caller's own working directory is kept (LocalRuntime runs its server in the runtime's folder).
    options.workingDirectory = dir;
    auto placed = ChildProcess::spawn(options);
    REQUIRE(placed);
    std::string placedCwd;
    while ((*placed)->readLine(line, 10s) == ChildProcess::ReadStatus::Line) {
        if (line.starts_with("cwd ")) {
            placedCwd = line.substr(4);
        }
    }
    CHECK((*placed)->wait(10s) == 0);
    REQUIRE_FALSE(placedCwd.empty());
    CHECK(std::filesystem::equivalent(pathFromUtf8(placedCwd), dir));
}

// The environment a helper gets, without a process: what passes, what never does, and its PATH.
TEST_CASE("Helper environment: system locale Qt and Wabcut variables pass but credentials and PATH do not", "[ai][process]") {
    const std::vector<std::wstring> parent{L"=C:=C:\\work",           L"Path=C:\\tools;C:\\evil",
                                           L"SystemRoot=C:\\Windows", L"TEMP=C:\\Temp",
                                           L"QT_QPA_PLATFORM=offscreen", L"WABCUT_CACHE_DIR=D:\\cache",
                                           L"OPENAI_API_KEY=sk-1",     L"WABCUT_OPENAI_API_KEY=sk-2",
                                           L"GITHUB_TOKEN=ghp",        L"AWS_SECRET_ACCESS_KEY=x",
                                           L"MY_PASSWORD=p",           L"WABCUT_KEYMAP=default",
                                           L"RANDOM_TOOL_HOME=C:\\x"};
    const std::wstring block = platform::helperEnvironment(parent, L"C:\\App\\helpers", L"C:\\App", L"C:\\Windows\\System32");
    std::vector<std::wstring> entries;
    for (std::size_t at = 0; at < block.size() && block[at] != L'\0';) {
        const std::size_t end = block.find(L'\0', at);
        entries.push_back(block.substr(at, end - at));
        at = end + 1;
    }
    CHECK(block.ends_with(std::wstring(2, L'\0')));
    const std::vector<std::wstring> expected{L"PATH=C:\\App\\helpers;C:\\App;C:\\Windows\\System32;C:\\Windows",
                                             L"QT_QPA_PLATFORM=offscreen", L"SystemRoot=C:\\Windows", L"TEMP=C:\\Temp",
                                             L"WABCUT_CACHE_DIR=D:\\cache", L"WABCUT_KEYMAP=default"};
    CHECK(entries == expected); // sorted by name, case-insensitively
    CHECK_FALSE(platform::helperMayInherit(L"wabcut_anthropic_api_key"));
    CHECK(platform::helperMayInherit(L"LocalAppData"));
}
#endif

#ifdef _WIN32
// 0.2 (review follow-up): AI helpers ran at normal priority with no CPU cap, so a transcription in the background took
// every core playback needed. They now run below normal priority under the helper policy's hard CPU rate cap (Settings >
// AI > "AI helpers' CPU", 75 % by default); an explicit cap wins, and other children (the script host) are untouched.
TEST_CASE("ChildProcess: AI helpers run below normal priority under the helper policy's CPU cap", "[ai][process]") {
    const HelperCpuPolicy saved = helperCpuPolicy();
    CHECK(saved.cpuRatePercent == kDefaultHelperCpuPercent);
    CHECK(saved.lowPriority);

    ChildProcessOptions options;
    options.executable = WABCUT_FAKE_INFER;
    options.aiHelper = true;
    setHelperCpuPolicy(HelperCpuPolicy{true, 40});
    auto helper = ChildProcess::spawn(options);
    REQUIRE(helper);
    CHECK((*helper)->cpuRateLimitPercent() == 40);
    CHECK((*helper)->belowNormalPriority());
    // The policy applies to helpers started afterwards; one already running keeps its cap.
    setHelperCpuPolicy(HelperCpuPolicy{true, 100});
    CHECK((*helper)->cpuRateLimitPercent() == 40);
    auto unlimited = ChildProcess::spawn(options);
    REQUIRE(unlimited);
    CHECK_FALSE((*unlimited)->cpuRateLimitPercent());
    CHECK((*unlimited)->belowNormalPriority());

    // An explicit cap wins over the policy's.
    options.cpuRatePercent = 25;
    auto explicitCap = ChildProcess::spawn(options);
    REQUIRE(explicitCap);
    CHECK((*explicitCap)->cpuRateLimitPercent() == 25);

    // A child that is not an AI helper gets neither.
    setHelperCpuPolicy(HelperCpuPolicy{true, 50});
    ChildProcessOptions plain;
    plain.executable = WABCUT_FAKE_INFER;
    auto other = ChildProcess::spawn(plain);
    REQUIRE(other);
    CHECK_FALSE((*other)->cpuRateLimitPercent());
    // Untouched: it runs at the test's own priority (a CI runner may start the tests below normal already).
    if (GetPriorityClass(GetCurrentProcess()) != BELOW_NORMAL_PRIORITY_CLASS)
        CHECK_FALSE((*other)->belowNormalPriority());

    for (auto* child : {&*helper, &*unlimited, &*explicitCap, &*other}) {
        (*child)->closeInput();
        CHECK((*child)->wait(10s) == 0);
    }
    setHelperCpuPolicy(saved);
}
#endif
