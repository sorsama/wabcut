// wabcut-ofxhost: runs OpenFX video effect plug-ins outside the editor (0.7, docs/plugins.md, "OpenFX video plug-ins").
//
//   wabcut-ofxhost --scan <file>    prints the plug-ins of one .ofx binary as JSON and exits (the editor's scanner runs one
//                                   per file, so a plug-in that crashes while it is described only takes this process
//                                   down and goes on the blocklist)
//   wabcut-ofxhost --serve <file>   hosts instances of the plug-ins of one binary for the editor: JSON lines on stdin and
//                                   stdout, pictures through shared memory (Protocol.h)
//
// Serve mode is one thread: it reads a request, does it, answers. A plug-in that crashes ends this process (the editor
// notices the closed pipe, reports the exit code and disables the plug-in's instances); one that hangs is ended by the
// editor after its timeout.

#include "OfxHost.h"

#include <wabcut/ofxhost/Protocol.h>
#include <wabcut/plughost/Ipc.h>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

using nlohmann::json;
namespace ipc = wabcut::plughost;

namespace wabcut::ofxhost {

namespace {

/// One control line: never throws (a plug-in's name that is not UTF-8 is repaired, not fatal).
std::string toLine(const json& message) { return message.dump(-1, ' ', false, json::error_handler_t::replace); }

int scan(const std::filesystem::path& file) {
    std::string error;
    std::unique_ptr<OfxModule> module = OfxModule::load(file, error);
    if (!module) {
        ipc::writeStdoutLine(toLine(json{{"ok", false}, {"error", error}}));
        return 2;
    }
    ipc::writeStdoutLine(toLine(json{{"ok", true}, {"plugins", module->describeJson()}}));
    return 0;
}

template <class T>
T field(const json& j, const char* key, T fallback) {
    try {
        if (j.is_object()) {
            if (const auto it = j.find(key); it != j.end() && !it->is_null()) {
                return it->get<T>();
            }
        }
    } catch (const json::exception&) {
    }
    return fallback;
}

json failure(std::uint64_t id, const std::string& error) { return json{{"id", id}, {"ok", false}, {"error", error}}; }

struct Slot {
    std::unique_ptr<ipc::SharedMemory> memory;
    std::uint64_t capacity = 0;
    std::unique_ptr<OfxInstance> instance;
};

class Server {
public:
    explicit Server(std::unique_ptr<OfxModule> module, std::string loadError) : m_module(std::move(module)), m_loadError(std::move(loadError)) {}

    /// False when the helper should stop.
    bool handle(const std::string& line) {
        const json request = json::parse(line, nullptr, false);
        if (!request.is_object()) {
            return true;
        }
        const std::uint64_t id = field<std::uint64_t>(request, "id", 0);
        const std::string op = field<std::string>(request, "op", "");
        json reply;
        try {
            if (op == "quit") {
                return false;
            }
            reply = dispatch(op, request, id);
        } catch (const std::exception& e) {
            reply = failure(id, std::string("the helper failed: ") + e.what());
        }
        ipc::writeStdoutLine(toLine(reply));
        return true;
    }

private:
    json dispatch(const std::string& op, const json& request, std::uint64_t id) {
        if (op == "hello") {
            if (!m_module) {
                return failure(id, m_loadError);
            }
            m_module->describeAll();
            json ids = json::array();
            for (const auto& p : m_module->plugins()) {
                ids.push_back(p->id);
            }
            return json{{"id", id}, {"ok", true}, {"version", kProtocolVersion}, {"plugins", std::move(ids)}};
        }
        if (!m_module) {
            return failure(id, m_loadError);
        }
        if (op == "create") {
            return create(request, id);
        }
        if (op == "attach") {
            return attach(request, id);
        }
        if (op == "render") {
            return render(request, id);
        }
        if (op == "destroy") {
            m_slots.erase(field<std::uint64_t>(request, "instance", 0));
            return json{{"id", id}, {"ok", true}};
        }
        return failure(id, "unknown request '" + op + "'");
    }

    static std::unique_ptr<ipc::SharedMemory> openBlock(const std::string& name, std::uint64_t capacity, std::string& error) {
        if (capacity == 0 || capacity > kMaxFramePixels) {
            error = "the picture block has an unsupported size";
            return nullptr;
        }
        auto memory = ipc::SharedMemory::open(name, blockBytes(capacity), error);
        if (!memory) {
            return nullptr;
        }
        const auto* header = static_cast<const FrameHeader*>(memory->data());
        if (header->magic != kFrameMagic || header->version != kProtocolVersion || header->capacityPixels != capacity) {
            error = "the editor speaks another protocol version";
            return nullptr;
        }
        return memory;
    }

    json create(const json& request, std::uint64_t id) {
        const std::uint64_t instance = field<std::uint64_t>(request, "instance", 0);
        const std::string plugin = field<std::string>(request, "plugin", "");
        const int width = field<int>(request, "width", 0);
        const int height = field<int>(request, "height", 0);
        PluginRuntime* rt = m_module->find(plugin);
        if (rt == nullptr) {
            return failure(id, "this binary has no plug-in '" + plugin + "'");
        }
        if (instance == 0 || width <= 0 || height <= 0) {
            return failure(id, "bad create request");
        }
        Slot slot;
        std::string error;
        slot.capacity = field<std::uint64_t>(request, "capacity", 0);
        slot.memory = openBlock(field<std::string>(request, "memory", ""), slot.capacity, error);
        if (!slot.memory) {
            return failure(id, error);
        }
        slot.instance = OfxInstance::create(*rt, width, height, field<double>(request, "fps", 24.0), error);
        if (!slot.instance) {
            return failure(id, error);
        }
        const bool frameVarying = slot.instance->frameVarying();
        m_slots[instance] = std::move(slot);
        return json{{"id", id}, {"ok", true}, {"frameVarying", frameVarying}};
    }

    json attach(const json& request, std::uint64_t id) {
        const auto it = m_slots.find(field<std::uint64_t>(request, "instance", 0));
        if (it == m_slots.end()) {
            return failure(id, "no such instance");
        }
        std::string error;
        const std::uint64_t capacity = field<std::uint64_t>(request, "capacity", 0);
        auto memory = openBlock(field<std::string>(request, "memory", ""), capacity, error);
        if (!memory) {
            return failure(id, error);
        }
        it->second.memory = std::move(memory);
        it->second.capacity = capacity;
        return json{{"id", id}, {"ok", true}};
    }

    json render(const json& request, std::uint64_t id) {
        const auto it = m_slots.find(field<std::uint64_t>(request, "instance", 0));
        if (it == m_slots.end()) {
            return failure(id, "no such instance");
        }
        Slot& slot = it->second;
        OfxInstance::RenderArgs args;
        args.width = field<int>(request, "width", 0);
        args.height = field<int>(request, "height", 0);
        args.time = field<double>(request, "time", 0.0);
        args.fps = field<double>(request, "fps", 24.0);
        args.pixelAspect = field<double>(request, "par", 1.0);
        args.hasAlpha = field<bool>(request, "hasAlpha", true);
        if (args.width <= 0 || args.height <= 0 ||
            static_cast<std::uint64_t>(args.width) * static_cast<std::uint64_t>(args.height) > slot.capacity) {
            return failure(id, "the picture does not fit the shared block");
        }
        json params = request.contains("params") ? request["params"] : json::object();
        args.params = &params;
        auto* base = static_cast<std::uint8_t*>(slot.memory->data());
        const auto* in = reinterpret_cast<const float*>(base + inputOffset());
        auto* out = reinterpret_cast<float*>(base + outputOffset(slot.capacity));
        std::string error;
        OutputAlpha alpha = OutputAlpha::Premultiplied;
        const auto start = std::chrono::steady_clock::now();
        if (!slot.instance->render(args, in, out, error, alpha)) {
            return failure(id, error);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return json{{"id", id}, {"ok", true}, {"ms", ms}, {"outputAlpha", alpha == OutputAlpha::Opaque ? "opaque" : alpha == OutputAlpha::Unpremultiplied ? "unpremultiplied" : "premultiplied"}};
    }

    std::unique_ptr<OfxModule> m_module;
    std::string m_loadError;
    std::map<std::uint64_t, Slot> m_slots;
};

int serve(const std::filesystem::path& file) {
    std::string error;
    std::unique_ptr<OfxModule> module = OfxModule::load(file, error);
    Server server(std::move(module), error);
    while (auto line = ipc::readStdinLine()) {
        if (!server.handle(*line)) {
            break;
        }
    }
    return 0;
}

} // namespace
} // namespace wabcut::ofxhost

int wmain(int argc, wchar_t** argv) {
    using namespace wabcut::ofxhost;
    // DLLs this process loads by name come from its own folder, System32 and the folders it adds, never the current
    // directory (a plug-in binary keeps its own folder on the search path: OfxModule::load).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    SetDllDirectoryW(L"");
    // A crashing plug-in must end this process quietly (the editor badges it), not open a Windows error dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Before any plug-in is loaded: what plug-ins print goes to the log, never into the control channel.
    if (std::string error; !ipc::takeControlChannel(error)) {
        std::fprintf(stderr, "[ofxhost] %s: plug-ins' output shares the control channel\n", error.c_str());
    }
    int code = 1;
    if (argc >= 3 && std::wcscmp(argv[1], L"--scan") == 0) {
        code = scan(std::filesystem::path(argv[2]));
    } else if (argc >= 3 && std::wcscmp(argv[1], L"--serve") == 0) {
        code = serve(std::filesystem::path(argv[2]));
    } else {
        std::fprintf(stderr, "usage: wabcut-ofxhost --scan <plug-in.ofx> | --serve <plug-in.ofx>\n");
    }
    return code;
}
