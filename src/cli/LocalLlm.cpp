#include "LocalLlm.h"
#include <wabcut/ai/generation/GenerationCommands.h>
#include <wabcut/ai/llm/LocalRuntime.h>
#include <wabcut/ai/llm/Tiers.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai_net/Network.h>
#include <condition_variable>
#include <thread>
namespace {
using namespace wabcut;
using namespace wabcut::ai;
class Services final : public generation::GenerationServices {
public:
    Services() : runtime(http), timer([this](std::stop_token stop) {
        std::mutex mutex; std::condition_variable_any cv; std::unique_lock lock(mutex);
        while (!stop.stop_requested()) { cv.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; }); runtime.tick(); }
    }) {}
    ~Services() override { timer.request_stop(); timer.join(); runtime.stop(); }
    command::CommandOutput generate(std::string_view, command::ExecutionContext&) override {
        command::fail(command::ErrorCode::InvalidRequest,
            "This CLI/MCP host cannot approve cloud uploads or spend; use the Generate panel to approve and preview this request");
    }
    Json local(std::string_view operation, const Json& params) override {
        if (operation == "stop") runtime.stop();
        if (operation == "start") {
            std::string id = params.value("model", std::string());
            if (id.empty()) { // AI-15: the tier this computer runs best now
                id = std::string(llm::defaultModel(llm::probeSystem()));
            }
            const auto* model = findModel(id);
            if (!model || model->kind != ModelKind::Llm) command::fail(command::ErrorCode::InvalidParams, "unknown allow-listed local model");
            auto plan = llm::planLaunch(*model, llm::probeSystem());
            if (!plan) command::fail(command::ErrorCode::InvalidRequest, plan.error().message);
            ModelStore store(ModelStore::defaultRoot());
            auto weights = store.ensure(*model, nullptr);
            if (!weights) command::fail(command::ErrorCode::InvalidRequest, "Download the model and runtime in AI preferences first");
            if (auto verified = store.verify(*model); !verified) command::fail(command::ErrorCode::InvalidRequest, verified.error().message);
            const auto root = systemEnv("SystemRoot");
            if (!root) command::fail(command::ErrorCode::InvalidRequest, "managed runtime currently needs Windows x64");
            auto executable = llm::installRuntime(store, *plan, nullptr, std::filesystem::path(*root) / "System32" / "tar.exe", {});
            if (!executable) command::fail(command::ErrorCode::InvalidRequest, executable.error().message);
            runtime.stop();
            if (auto started = runtime.start(*executable, *weights, *model, *plan, {}, {}); !started)
                command::fail(command::ErrorCode::InvalidRequest, started.error().message);
        }
        const auto s = runtime.status();
        const char* state = s.state == llm::RuntimeStatus::State::Ready ? "ready" : s.state == llm::RuntimeStatus::State::Starting ? "starting" :
            s.state == llm::RuntimeStatus::State::Failed ? "failed" : s.state == llm::RuntimeStatus::State::Suspended ? "suspended" : "stopped";
        return {{"state", state}, {"model", s.model}, {"backend", s.backend}, {"message", s.message},
            {"lifetime", "this CLI/MCP process; idle unload after two minutes"}};
    }
private:
    net::QtHttpTransport http;
    llm::LocalRuntime runtime;
    std::jthread timer;
};
}
std::shared_ptr<wabcut::command::CommandServices> localLlmServices() { return std::make_shared<Services>(); }
