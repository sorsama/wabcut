#include "FakeTransport.h"
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/generation/GenerationCommands.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/command/Registry.h>
#include <cstdlib>
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <catch2/catch_test_macros.hpp>
#include <thread>
using namespace wabcut::ai;
using namespace wabcut::ai::test;
TEST_CASE("Managed local helper lifecycle stays authenticated and unloads", "[ai][llm][process]") {
    FakeTransport fake; llm::LocalRuntime runtime(fake);
    const auto model = *findModel("qwen3-4b-instruct-2507");
    llm::LaunchPlan plan; plan.runtimeId = "test-cpu";
    auto weights = uniqueTempPath("fake-weights", ".gguf"); std::ofstream(weights) << "fixture";
    // Generous: the first probe waits for the (low-priority) child to listen (P2-2), which can take seconds on a loaded
    // machine; a start that succeeds returns as soon as the child answers.
    llm::LocalRuntime::Options options; options.startupTimeout = std::chrono::seconds(10);
    options.idleTimeout = std::chrono::milliseconds(0);
    SECTION("ready, idle, pressure, busy") {
        fake.pushJson(200, Json{{"data", Json::array({{{"id", model.id}}})}}.dump());
        const auto started = runtime.start(WABCUT_FAKE_LLAMA, weights, model, plan, {}, options);
        INFO((started ? std::string() : started.error().message));
        REQUIRE(started);
        CHECK(runtime.status().state == llm::RuntimeStatus::State::Ready);
        CHECK(runtime.status().baseUrl.starts_with("http://127.0.0.1:"));
        REQUIRE(fake.requests().size() == 1);
        CHECK(findHeader(fake.requests()[0].headers, "Authorization")->starts_with("Bearer "));
        CHECK(runtime.key().reveal().size() == 64);
        runtime.setBusy(true); runtime.tick(); CHECK(runtime.status().state == llm::RuntimeStatus::State::Ready);
        runtime.setBusy(false); runtime.tick(); CHECK(runtime.status().state == llm::RuntimeStatus::State::Stopped);
        CHECK(runtime.key().empty());
        runtime.setPressure(true);
        CHECK_FALSE(runtime.start(WABCUT_FAKE_LLAMA, weights, model, plan, {}, options));
        CHECK(runtime.status().state == llm::RuntimeStatus::State::Suspended);
    }
    SECTION("startup timeout rejects another process model identity") {
        options.startupTimeout = std::chrono::milliseconds(1000);
        fake.pushJson(200, R"({"data":[{"id":"wrong-model"}]})");
        CHECK_FALSE(runtime.start(WABCUT_FAKE_LLAMA, weights, model, plan, {}, options));
        CHECK(runtime.status().state == llm::RuntimeStatus::State::Failed);
        CHECK(runtime.key().empty());
    }
    SECTION("crash") {
        auto crash = uniqueTempPath("crash-weights", ".gguf"); std::ofstream(crash) << "fixture";
        auto r = runtime.start(WABCUT_FAKE_LLAMA, crash, model, plan, {}, options);
        REQUIRE_FALSE(r); INFO(r.error().message); CHECK(r.error().message.find("exited") != std::string::npos);
        std::filesystem::remove(crash);
    }
    SECTION("cancelled") {
        std::stop_source stop; stop.request_stop();
        CHECK_FALSE(runtime.start(WABCUT_FAKE_LLAMA, weights, model, plan, stop.get_token(), options));
        CHECK(runtime.key().empty());
    }
    SECTION("port conflict") {
        ScriptedResponse ready; ready.body = Json{{"data", Json::array({{{"id", model.id}}})}}.dump();
        ready.before = [] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }; fake.push(ready);
        const auto started = runtime.start(WABCUT_FAKE_LLAMA, weights, model, plan, {}, options);
        INFO((started ? std::string() : started.error().message));
        REQUIRE(started);
        const auto url = parseHttpUrl(runtime.status().baseUrl); REQUIRE(url);
        const auto base = runtime.status().baseUrl;
        const int port = std::stoi(base.substr(base.find_last_of(':') + 1));
        FakeTransport other; llm::LocalRuntime conflict(other);
        options.choosePort = [port] { return port; };
        auto r = conflict.start(WABCUT_FAKE_LLAMA, weights, model, plan, {}, options);
        REQUIRE_FALSE(r);
        // The second child either exits on the taken port or is caught not owning it (P2-2); either way no key leaves.
        CHECK((r.error().message.find("exited") != std::string::npos || r.error().message.find("another program") != std::string::npos));
        CHECK(other.requests().empty());
    }
    runtime.stop(); std::filesystem::remove(weights);
}

// Review wave 4, P2-1: starting the assistant is not a read, and the runtime is extracted once per archive set instead
// of into a new random directory on every start.
TEST_CASE("llm.local.start/stop need more than Read; the runtime is extracted once and reused while intact", "[ai][llm][process]") {
    wabcut::command::CommandRegistry registry;
    generation::registerGenerationCommands(registry);
    CHECK(registry.find("llm.local.start")->capability == wabcut::command::Capability::MediaAnalysis);
    CHECK(registry.find("llm.local.stop")->capability == wabcut::command::Capability::MediaAnalysis);
    CHECK(registry.find("llm.local.status")->capability == wabcut::command::Capability::Read);
    CHECK(registry.find("llm.local.models")->capability == wabcut::command::Capability::Read);
#ifndef _WIN32
    SKIP("the runtime archives are extracted with Windows' tar.exe");
#else
    const char* systemRoot = std::getenv("SystemRoot");
    REQUIRE(systemRoot != nullptr);
    const std::filesystem::path tar = std::filesystem::path(systemRoot) / "System32" / "tar.exe";
    if (!std::filesystem::exists(tar)) {
        SKIP("no System32\tar.exe");
    }
    const auto root = uniqueTempPath("runtime-store");
    std::filesystem::create_directories(root / "content" / "bin");
    std::ofstream(root / "content" / "bin" / "llama-server.exe", std::ios::binary) << "fake server";
    std::ofstream(root / "content" / "bin" / "ggml.dll", std::ios::binary) << "library";
    const auto archive = root / "llama.tar";
    {
        ChildProcessOptions pack;
        pack.executable = tar;
        pack.arguments = {"-cf", archive.string(), "-C", (root / "content").string(), "bin"};
        auto process = ChildProcess::spawn(pack);
        REQUIRE(process);
        REQUIRE((*process)->wait(std::chrono::seconds(30)) == 0);
    }
    Sha256 hash;
    hash.update(readFile(archive.string()));
    const std::vector<llm::RuntimeArchive> archives{{archive, hash.finishHex()}};
    const auto store = root / "store";
    std::filesystem::create_directories(store / "runtime-0123456789abcdef");      // a stale random extraction
    std::ofstream(store / "runtime-0123456789abcdef" / "old.dll") << "old";

    auto first = llm::extractRuntime(store, archives, tar, {});
    REQUIRE(first);
    CHECK(first->filename() == "llama-server.exe");
    CHECK(first->parent_path().parent_path().filename() == "runtime-" + archives[0].sha256.substr(0, 16));
    CHECK_FALSE(std::filesystem::exists(store / "runtime-0123456789abcdef"));
    // A second start reuses it without the extractor (a missing extractor would fail otherwise).
    const auto noExtractor = root / "missing-tar.exe";
    auto second = llm::extractRuntime(store, archives, noExtractor, {});
    REQUIRE(second);
    CHECK(*second == *first);
    std::size_t runtimes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(store)) {
        runtimes += entry.path().filename().string().starts_with("runtime-") ? 1 : 0;
    }
    CHECK(runtimes == 1);
    // A changed file is noticed: the copy is not trusted until extracted again.
    std::ofstream(first->parent_path() / "ggml.dll", std::ios::binary) << "tampered";
    CHECK_FALSE(llm::extractRuntime(store, archives, noExtractor, {}));
    auto third = llm::extractRuntime(store, archives, tar, {});
    REQUIRE(third);
    CHECK(readFile((third->parent_path() / "ggml.dll").string()) == "library");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
#endif
}

// Review wave 4, P2-2: another process that grabs the chosen port before llama-server binds it never receives the
// API key: the readiness probe (which carries the key) is sent only once the listener is our own child.
TEST_CASE("The local runtime sends its key only to a listener that is its own child", "[ai][llm][process]") {
#ifndef _WIN32
    SKIP("the listener check uses the Windows TCP table");
#else
    WSADATA data{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const SOCKET impostor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(impostor != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(impostor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(listen(impostor, 1) == 0);
    int size = sizeof(address);
    REQUIRE(getsockname(impostor, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const int port = ntohs(address.sin_port);

    FakeTransport fake;
    fake.pushJson(200, Json{{"data", Json::array({{{"id", "qwen3-4b-instruct-2507"}}})}}.dump()); // the impostor's answer
    llm::LocalRuntime runtime(fake);
    const auto model = *findModel("qwen3-4b-instruct-2507");
    llm::LaunchPlan plan;
    plan.runtimeId = "test-cpu";
    auto weights = uniqueTempPath("fake-weights", ".gguf");
    std::ofstream(weights) << "fixture";
    llm::LocalRuntime::Options options;
    options.startupTimeout = std::chrono::milliseconds(1500);
    options.choosePort = [port] { return port; };
    auto started = runtime.start(WABCUT_FAKE_LLAMA, weights, model, plan, {}, options);
    REQUIRE_FALSE(started);
    CHECK(fake.requests().empty()); // the key never left
    CHECK(runtime.key().empty());
    closesocket(impostor);
    WSACleanup();
    std::filesystem::remove(weights);
#endif
}
