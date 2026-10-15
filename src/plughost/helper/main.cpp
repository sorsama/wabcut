// wabcut-plughost: runs audio plugins outside the editor (A.6, docs/plugins.md).
//
//   wabcut-plughost --scan <file-or-bundle>   prints the plugins in one binary as JSON and exits (the editor's scanner
//                                             runs one per file, so a plugin that crashes while scanned only takes
//                                             this process down and goes on the blocklist)
//   wabcut-plughost --serve                   hosts instances for the editor: JSON lines on stdin/stdout (Protocol.h)
//
// Serve mode threads: the main thread runs the Windows message loop (plugin editors, and the plugins' main-thread
// calls, which both formats require there); a reader thread turns stdin lines into main-thread work; every instance
// has an audio thread (MMCSS "Pro Audio") that processes the blocks the editor submits through shared memory.

#include "HostedPlugin.h"

#include <wabcut/plughost/Ipc.h>
#include <wabcut/plughost/Protocol.h>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <avrt.h>
#include <ole2.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using nlohmann::json;

namespace wabcut::plughost {

namespace {

constexpr UINT kWorkMessage = WM_APP + 1;
HWND gMessageWindow = nullptr;
std::mutex gWorkMutex;
std::deque<std::function<void()>> gWork;

LRESULT CALLBACK messageProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kWorkMessage) {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard lock(gWorkMutex);
            work.swap(gWork);
        }
        for (auto& w : work) {
            try {
                w();
            } catch (...) {
                std::fprintf(stderr, "[plughost] main-thread work failed with an exception\n");
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/// One control line: never throws (a plugin's name or message that is not UTF-8 is repaired, not fatal).
std::string toLine(const json& message) { return message.dump(-1, ' ', false, json::error_handler_t::replace); }

json paramsJson(HostedInstance& instance, bool withValues) {
    json out = json::array();
    for (const ParamInfo& p : instance.params()) {
        json j{{"id", p.id},           {"name", p.name},       {"module", p.module},       {"unit", p.unit},
               {"min", p.minValue},    {"max", p.maxValue},    {"default", p.defaultValue}, {"automatable", p.automatable},
               {"stepped", p.stepped}, {"hidden", p.hidden}};
        if (withValues) {
            if (const auto v = instance.paramValue(p.id)) {
                j["value"] = *v;
            }
        }
        out.push_back(std::move(j));
    }
    return out;
}

// ---- Scan mode --------------------------------------------------------------------------------------------------

int scan(const std::filesystem::path& path) {
    std::string error;
    std::shared_ptr<PluginModule> module = loadModule(path, error);
    if (!module) {
        writeStdoutLine(toLine(json{{"ok", false}, {"error", error}}));
        return 2;
    }
    json plugins = json::array();
    for (const PluginDescription& d : module->plugins()) {
        json p{{"format", d.format}, {"id", d.id},           {"name", d.name},     {"vendor", d.vendor},
               {"version", d.version}, {"category", d.category}, {"effect", d.effect}};
        if (d.effect) {
            std::string why;
            if (std::unique_ptr<HostedInstance> instance = module->create(d.id, {}, why)) {
                p["params"] = paramsJson(*instance, false);
                p["editor"] = instance->hasEditor();
                if (instance->activate(48000.0, why)) {
                    p["latency"] = instance->latency();
                    instance->deactivate();
                } else {
                    p["problem"] = why;
                }
            } else {
                p["problem"] = why;
            }
        }
        plugins.push_back(std::move(p));
    }
    writeStdoutLine(toLine(json{{"ok", true}, {"plugins", std::move(plugins)}}));
    return 0;
}

// ---- Serve mode -------------------------------------------------------------------------------------------------

/// One instance: the plugin (main thread), its shared memory and its audio thread.
struct Instance {
    std::uint64_t id = 0;
    double sampleRate = 48000.0;
    std::unique_ptr<SharedMemory> memory;
    InstanceShared* shared = nullptr;
    std::unique_ptr<NamedEvent> work;
    std::unique_ptr<NamedEvent> done;
    std::unique_ptr<HostedInstance> plugin;
    std::atomic<bool> quit{false};
    std::thread audio;
    /// Set while the main thread restarts the plugin: its latency reports during that restart are answered by it.
    std::shared_ptr<std::atomic<bool>> restarting = std::make_shared<std::atomic<bool>>(false);

    // The main thread stops the audio thread between two blocks to restart the plugin (a latency change).
    std::mutex pauseMutex;
    std::condition_variable pauseChanged;
    std::atomic<bool> pauseRequested{false};
    bool paused = false;

    /// Audio thread: waits here while the main thread restarts the plugin.
    void holdIfPaused() {
        if (!pauseRequested.load(std::memory_order_acquire)) {
            return;
        }
        std::unique_lock lock(pauseMutex);
        paused = true;
        pauseChanged.notify_all();
        pauseChanged.wait(lock, [&] {
            return !pauseRequested.load(std::memory_order_acquire) || quit.load(std::memory_order_acquire);
        });
        paused = false;
    }

    /// Main thread: stops the audio thread between blocks. False when it does not stop within 2 s (a plugin stuck in its
    /// process call): then nothing may touch the plugin.
    bool pause() {
        std::unique_lock lock(pauseMutex);
        pauseRequested.store(true, std::memory_order_release);
        if (work) {
            work->signal();
        }
        if (!pauseChanged.wait_for(lock, std::chrono::seconds(2), [&] { return paused; })) {
            pauseRequested.store(false, std::memory_order_release);
            pauseChanged.notify_all();
            return false;
        }
        return true;
    }

    void resume() {
        {
            std::lock_guard lock(pauseMutex);
            pauseRequested.store(false, std::memory_order_release);
        }
        pauseChanged.notify_all();
    }

    void run() {
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        while (!quit.load(std::memory_order_acquire)) {
            holdIfPaused();
            work->wait(std::chrono::milliseconds(50));
            holdIfPaused();
            const std::uint64_t submitted = shared->submitted.load(std::memory_order_acquire);
            std::uint64_t completed = shared->completed.load(std::memory_order_relaxed);
            while (completed < submitted && !quit.load(std::memory_order_relaxed) &&
                   !pauseRequested.load(std::memory_order_acquire)) {
                BlockSlot& slot = shared->ring[completed % kSlots];
                if ((slot.flags & kSlotReset) != 0) {
                    plugin->reset();
                }
                const bool failed = shared->status.load(std::memory_order_relaxed) == static_cast<std::uint32_t>(InstanceStatus::Failed);
                if (failed || !plugin->process(slot)) {
                    // The plugin reported an error: from now on it is bypassed (the editor shows why).
                    shared->status.store(static_cast<std::uint32_t>(InstanceStatus::Failed), std::memory_order_release);
                    for (int c = 0; c < kMaxChannels; ++c) {
                        std::memcpy(slot.output[c], slot.input[c], sizeof(float) * slot.frames);
                    }
                }
                ++completed;
                shared->completed.store(completed, std::memory_order_release);
                done->signal();
            }
        }
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
    }

    void stop() {
        quit.store(true, std::memory_order_release);
        {
            std::lock_guard lock(pauseMutex); // wakes an audio thread held by a restart
        }
        pauseChanged.notify_all();
        if (work) {
            work->signal();
        }
        if (audio.joinable()) {
            audio.join();
        }
        if (shared != nullptr) {
            shared->status.store(static_cast<std::uint32_t>(InstanceStatus::Stopped), std::memory_order_release);
        }
    }
};

class Server {
public:
    void handle(const std::string& line) {
        const json request = json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object()) {
            writeStdoutLine(toLine(json{{"ok", false}, {"error", "malformed request"}}));
            return;
        }
        json reply;
        try {
            reply = dispatch(request);
        } catch (const std::exception& e) {
            reply = json{{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
        }
        if (request.contains("id")) {
            reply["id"] = request["id"];
        }
        writeStdoutLine(toLine(reply));
    }

    bool quitting() const { return m_quit; }

    void shutdown() {
        for (auto& [id, instance] : m_instances) {
            instance->stop();
            instance->plugin.reset();
        }
        m_instances.clear();
        m_modules.clear();
    }

private:
    static json fail(const std::string& error) { return json{{"ok", false}, {"error", error}}; }

    Instance* find(const json& request) {
        const auto it = m_instances.find(request.at("instance").get<std::uint64_t>());
        return it != m_instances.end() ? it->second.get() : nullptr;
    }

    json dispatch(const json& request) {
        const std::string op = request.value("op", "");
        if (op == "hello") {
            return json{{"ok", true}, {"version", kProtocolVersion}, {"pid", currentProcessId()}};
        }
        if (op == "quit") {
            m_quit = true;
            PostQuitMessage(0);
            return json{{"ok", true}};
        }
        if (op == "create") {
            return create(request);
        }
        Instance* instance = request.contains("instance") ? find(request) : nullptr;
        if (instance == nullptr) {
            return fail("no such instance");
        }
        if (op == "destroy") {
            instance->stop();
            instance->plugin.reset();
            m_instances.erase(instance->id);
            return json{{"ok", true}};
        }
        if (op == "saveState") {
            const auto state = instance->plugin->saveState();
            if (!state) {
                return fail("the plugin could not save its state");
            }
            return json{{"ok", true}, {"state", base64Encode(state->data(), state->size())}};
        }
        if (op == "loadState") {
            const auto bytes = base64Decode(request.value("state", ""));
            if (!bytes || !instance->plugin->loadState(*bytes)) {
                return fail("the plugin did not accept the state");
            }
            return json{{"ok", true}};
        }
        if (op == "params") {
            return json{{"ok", true}, {"params", paramsJson(*instance->plugin, true)}};
        }
        if (op == "openEditor") {
            std::string error;
            if (!instance->plugin->openEditor(request.value("title", std::string("Plugin")), error)) {
                return fail(error);
            }
            return json{{"ok", true}};
        }
        if (op == "closeEditor") {
            instance->plugin->closeEditor();
            return json{{"ok", true}};
        }
        return fail("unknown op '" + op + "'");
    }

    json create(const json& request) {
        const auto id = request.at("instance").get<std::uint64_t>();
        if (m_instances.contains(id)) {
            return fail("the instance exists");
        }
        const std::filesystem::path path = std::filesystem::path(widen(request.at("path").get<std::string>()));
        std::string error;
        std::shared_ptr<PluginModule>& module = m_modules[path.wstring()];
        if (!module) {
            module = loadModule(path, error);
            if (!module) {
                m_modules.erase(path.wstring());
                return fail(error);
            }
        }
        auto instance = std::make_unique<Instance>();
        instance->id = id;
        InstanceCallbacks callbacks;
        callbacks.stateChanged = [this, id] { notify("stateChanged", id); };
        // Any thread (CLAP's request_restart may come from the audio thread): the restart runs on the main thread. A
        // report made while that restart activates the plugin again is the restart's own.
        callbacks.latencyChanged = [this, id, restarting = instance->restarting] {
            if (!restarting->load(std::memory_order_acquire)) {
                postToMainThread([this, id] { restart(id); });
            }
        };
        callbacks.editorClosed = [this, id] { notify("editorClosed", id); };
        instance->plugin = module->create(request.at("plugin").get<std::string>(), std::move(callbacks), error);
        if (!instance->plugin) {
            return fail(error);
        }
        if (const std::string state = request.value("state", ""); !state.empty()) {
            const auto bytes = base64Decode(state);
            if (!bytes || !instance->plugin->loadState(*bytes)) {
                return fail("the plugin did not accept its saved state");
            }
        }
        const double rate = request.value("sampleRate", 48000.0);
        instance->sampleRate = rate;
        if (!instance->plugin->activate(rate, error)) {
            return fail(error);
        }
        if (const int reported = instance->plugin->latency(); reported < 0 || reported > maxLatencyFrames(rate)) {
            return fail("the plugin reports a latency of " + std::to_string(reported) + " frames, more than " +
                        std::to_string(kMaxLatencySeconds) + " s");
        }
        instance->memory = SharedMemory::open(request.at("memory").get<std::string>(), sizeof(InstanceShared), error);
        instance->work = NamedEvent::open(request.at("work").get<std::string>(), error);
        instance->done = NamedEvent::open(request.at("done").get<std::string>(), error);
        if (!instance->memory || !instance->work || !instance->done) {
            return fail("cannot open the instance's shared memory: " + error);
        }
        instance->shared = static_cast<InstanceShared*>(instance->memory->data());
        if (instance->shared->magic != kSharedMagic || instance->shared->version != kProtocolVersion) {
            return fail("the editor speaks another protocol version");
        }
        const int latency = instance->plugin->latency();
        instance->shared->latency.store(static_cast<std::uint32_t>(std::max(latency, 0)), std::memory_order_release);
        instance->shared->status.store(static_cast<std::uint32_t>(InstanceStatus::Running), std::memory_order_release);
        json reply{{"ok", true}, {"latency", latency}, {"editor", instance->plugin->hasEditor()}};
        if (request.value("withParams", false)) {
            reply["params"] = paramsJson(*instance->plugin, true);
        }
        Instance* raw = instance.get();
        raw->audio = std::thread([raw] {
            try {
                raw->run();
            } catch (...) {
                // A plugin that throws out of its process call: the instance fails, the helper and its other instances
                // carry on.
                raw->shared->status.store(static_cast<std::uint32_t>(InstanceStatus::Failed), std::memory_order_release);
            }
        });
        m_instances.emplace(id, std::move(instance));
        return reply;
    }

    /// The plugin asked for a restart (its latency changed): with the audio thread stopped between blocks, deactivate it,
    /// activate it again and publish the latency it reports now (a latency over the limit, or a failed activation, fails
    /// the instance: the editor bypasses it).
    void restart(std::uint64_t id) {
        const auto it = m_instances.find(id);
        if (it == m_instances.end() || it->second->shared == nullptr || !it->second->plugin) {
            return;
        }
        Instance& instance = *it->second;
        if (!instance.pause()) {
            std::fprintf(stderr, "[plughost] instance %llu: the plugin did not stop for its restart\n",
                         static_cast<unsigned long long>(id));
            return;
        }
        instance.restarting->store(true, std::memory_order_release);
        instance.plugin->deactivate();
        std::string error;
        const bool active = instance.plugin->activate(instance.sampleRate, error);
        const int latency = active ? instance.plugin->latency() : 0;
        instance.restarting->store(false, std::memory_order_release);
        InstanceShared& shared = *instance.shared;
        if (!active || latency < 0 || latency > maxLatencyFrames(instance.sampleRate)) {
            std::fprintf(stderr, "[plughost] instance %llu: restart failed (%s, latency %d)\n", static_cast<unsigned long long>(id),
                         error.c_str(), latency);
            shared.status.store(static_cast<std::uint32_t>(InstanceStatus::Failed), std::memory_order_release);
        } else {
            shared.latency.store(static_cast<std::uint32_t>(latency), std::memory_order_release);
            shared.latencyGeneration.fetch_add(1, std::memory_order_acq_rel);
        }
        instance.resume();
        writeStdoutLine(toLine(json{{"event", "latencyChanged"}, {"instance", id}, {"latency", latency}}));
    }

    void notify(const char* event, std::uint64_t instance) {
        if (event == std::string_view("stateChanged")) {
            if (auto it = m_instances.find(instance); it != m_instances.end() && it->second->shared != nullptr) {
                it->second->shared->stateChanges.fetch_add(1, std::memory_order_release);
            }
        }
        writeStdoutLine(toLine(json{{"event", event}, {"instance", instance}}));
    }

    std::map<std::wstring, std::shared_ptr<PluginModule>> m_modules;
    std::map<std::uint64_t, std::unique_ptr<Instance>> m_instances;
    bool m_quit = false;
};

int serve() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = messageProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WabcutPlughostMessages";
    RegisterClassExW(&wc);
    gMessageWindow = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (gMessageWindow == nullptr) {
        return 3;
    }
    Server server;
    std::thread reader([&server] {
        try {
            while (auto line = readStdinLine()) {
                postToMainThread([&server, l = std::move(*line)] { server.handle(l); });
            }
        } catch (...) {
            std::fprintf(stderr, "[plughost] the control channel failed\n");
        }
        // The editor went away (or closed the channel): stop.
        postToMainThread([] { PostQuitMessage(0); });
    });
    reader.detach();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    server.shutdown();
    return 0;
}

} // namespace

void postToMainThread(std::function<void()> work) {
    {
        std::lock_guard lock(gWorkMutex);
        gWork.push_back(std::move(work));
    }
    PostMessageW(gMessageWindow, kWorkMessage, 0, 0);
}

std::shared_ptr<PluginModule> loadModule(const std::filesystem::path& path, std::string& error) {
    std::wstring ext = path.extension().wstring();
    for (wchar_t& c : ext) {
        c = static_cast<wchar_t>(towlower(c));
    }
    if (ext == L".clap") {
        return loadClapModule(path, error);
    }
    if (ext == L".vst3") {
        return loadVst3Module(path, error);
    }
    error = utf8Path(path) + ": not a .clap or .vst3 plugin";
    return nullptr;
}

} // namespace wabcut::plughost

int wmain(int argc, wchar_t** argv) {
    using namespace wabcut::plughost;
    // DLLs this process loads by name come from its own folder, System32 and the folders it adds, never the current
    // directory (a plugin binary itself keeps its own search path: ClapHost.cpp, Vst3Host.cpp).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    SetDllDirectoryW(L"");
    // A crashing plugin must end this process quietly (the editor badges it), not open a Windows error dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Before any plugin is loaded: what plugins print goes to the log, never into the control channel.
    if (std::string error; !takeControlChannel(error)) {
        std::fprintf(stderr, "[plughost] %s: plugins' output shares the control channel\n", error.c_str());
    }
    OleInitialize(nullptr); // plugin editors use drag and drop, the clipboard and COM
    int code = 1;
    if (argc >= 3 && std::wcscmp(argv[1], L"--scan") == 0) {
        code = scan(std::filesystem::path(argv[2]));
    } else if (argc >= 2 && std::wcscmp(argv[1], L"--serve") == 0) {
        code = serve();
    } else {
        std::fprintf(stderr, "usage: wabcut-plughost --scan <plugin> | --serve\n");
    }
    OleUninitialize();
    return code;
}
