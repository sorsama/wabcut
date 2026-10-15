// wabcut-plughost: CLAP plugins (the clap headers, MIT). One PluginModule per .clap file (its clap_entry), one
// ClapInstance per plugin instance.

#include "Editor.h"
#include "HostedPlugin.h"

#include <wabcut/plughost/Ipc.h>

#include <clap/clap.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace wabcut::plughost {

namespace {

thread_local bool tAudioThread = false;
DWORD gMainThread = GetCurrentThreadId();

class ClapModule;

class ClapInstance final : public HostedInstance {
public:
    ClapInstance(std::shared_ptr<ClapModule> module, InstanceCallbacks callbacks);
    ~ClapInstance() override;

    bool init(const clap_plugin_factory_t* factory, const std::string& id, std::string& error);

    std::vector<ParamInfo> params() override;
    std::optional<double> paramValue(std::uint32_t id) override;
    bool activate(double sampleRate, std::string& error) override;
    void deactivate() override;
    int latency() override;
    bool process(BlockSlot& slot) noexcept override;
    void reset() noexcept override;
    std::optional<std::vector<std::uint8_t>> saveState() override;
    bool loadState(const std::vector<std::uint8_t>& state) override;
    bool hasEditor() override;
    bool openEditor(const std::string& title, std::string& error) override;
    void closeEditor() override;

private:
    // ---- clap_host -------------------------------------------------------------------------------------------
    static ClapInstance* self(const clap_host_t* host) { return static_cast<ClapInstance*>(host->host_data); }
    static const void* CLAP_ABI hostGetExtension(const clap_host_t* host, const char* id);
    static void CLAP_ABI hostRequestRestart(const clap_host_t* host);
    static void CLAP_ABI hostRequestProcess(const clap_host_t*) {}
    static void CLAP_ABI hostRequestCallback(const clap_host_t* host);
    static void CLAP_ABI hostLog(const clap_host_t*, clap_log_severity severity, const char* msg);
    static bool CLAP_ABI hostIsMainThread(const clap_host_t*) { return GetCurrentThreadId() == gMainThread; }
    static bool CLAP_ABI hostIsAudioThread(const clap_host_t*) { return tAudioThread; }
    static void CLAP_ABI hostParamsRescan(const clap_host_t*, clap_param_rescan_flags) {}
    static void CLAP_ABI hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
    static void CLAP_ABI hostParamsRequestFlush(const clap_host_t*) {}
    static void CLAP_ABI hostStateMarkDirty(const clap_host_t* host);
    static void CLAP_ABI hostLatencyChanged(const clap_host_t* host);
    static void CLAP_ABI hostGuiResizeHintsChanged(const clap_host_t*) {}
    static bool CLAP_ABI hostGuiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height);
    static bool CLAP_ABI hostGuiRequestShow(const clap_host_t*) { return true; }
    static bool CLAP_ABI hostGuiRequestHide(const clap_host_t*) { return false; }
    static void CLAP_ABI hostGuiClosed(const clap_host_t* host, bool wasDestroyed);

    // ---- events ----------------------------------------------------------------------------------------------
    static uint32_t CLAP_ABI inSize(const clap_input_events_t* list);
    static const clap_event_header_t* CLAP_ABI inGet(const clap_input_events_t* list, uint32_t index);
    static bool CLAP_ABI outTryPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

    void destroyGui();

    std::shared_ptr<ClapModule> m_module;
    InstanceCallbacks m_callbacks;
    clap_host_t m_host{};
    const clap_plugin_t* m_plugin = nullptr;
    const clap_plugin_params_t* m_params = nullptr;
    const clap_plugin_latency_t* m_latency = nullptr;
    const clap_plugin_state_t* m_state = nullptr;
    const clap_plugin_gui_t* m_gui = nullptr;
    bool m_active = false;
    bool m_processing = false;
    std::vector<std::pair<clap_id, void*>> m_cookies; ///< by param id: the plugin's cookie for fast lookups

    // Events of the block being processed (audio thread).
    std::vector<clap_event_param_value_t> m_events;
    // The editor (main thread).
    std::unique_ptr<EditorWindow> m_window;
    bool m_guiCreated = false;
};

class ClapModule final : public PluginModule, public std::enable_shared_from_this<ClapModule> {
public:
    ClapModule(HMODULE library, const clap_plugin_entry_t* entry) : m_library(library), m_entry(entry) {}
    ~ClapModule() override {
        if (m_entry != nullptr) {
            m_entry->deinit();
        }
        if (m_library != nullptr) {
            FreeLibrary(m_library);
        }
    }

    const clap_plugin_factory_t* factory() const {
        return static_cast<const clap_plugin_factory_t*>(m_entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    }

    std::vector<PluginDescription> plugins() override {
        std::vector<PluginDescription> out;
        const clap_plugin_factory_t* f = factory();
        if (f == nullptr) {
            return out;
        }
        const uint32_t count = f->get_plugin_count(f);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
            if (d == nullptr || d->id == nullptr) {
                continue;
            }
            PluginDescription p;
            p.format = "clap";
            p.id = d->id;
            p.name = d->name != nullptr ? d->name : d->id;
            p.vendor = d->vendor != nullptr ? d->vendor : "";
            p.version = d->version != nullptr ? d->version : "";
            bool audioEffect = false;
            bool instrument = false;
            for (const char* const* feature = d->features; feature != nullptr && *feature != nullptr; ++feature) {
                if (!p.category.empty()) {
                    p.category += ";";
                }
                p.category += *feature;
                audioEffect = audioEffect || std::strcmp(*feature, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0;
                instrument = instrument || std::strcmp(*feature, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0;
            }
            p.effect = audioEffect || !instrument;
            out.push_back(std::move(p));
        }
        return out;
    }

    std::unique_ptr<HostedInstance> create(const std::string& id, InstanceCallbacks callbacks, std::string& error) override {
        const clap_plugin_factory_t* f = factory();
        if (f == nullptr) {
            error = "the CLAP file has no plugin factory";
            return nullptr;
        }
        auto instance = std::make_unique<ClapInstance>(shared_from_this(), std::move(callbacks));
        if (!instance->init(f, id, error)) {
            return nullptr;
        }
        return instance;
    }

private:
    HMODULE m_library = nullptr;
    const clap_plugin_entry_t* m_entry = nullptr;
};

// ---- ClapInstance -----------------------------------------------------------------------------------------------

ClapInstance::ClapInstance(std::shared_ptr<ClapModule> module, InstanceCallbacks callbacks)
    : m_module(std::move(module)), m_callbacks(std::move(callbacks)) {
    m_host.clap_version = CLAP_VERSION;
    m_host.host_data = this;
    m_host.name = "Wabcut";
    m_host.vendor = "Wabcut";
    m_host.url = "https://github.com/wabcut";
    m_host.version = "0.1";
    m_host.get_extension = hostGetExtension;
    m_host.request_restart = hostRequestRestart;
    m_host.request_process = hostRequestProcess;
    m_host.request_callback = hostRequestCallback;
    m_events.reserve(kMaxBlockEvents);
}

ClapInstance::~ClapInstance() {
    destroyGui();
    if (m_plugin != nullptr) {
        deactivate();
        m_plugin->destroy(m_plugin);
    }
}

bool ClapInstance::init(const clap_plugin_factory_t* factory, const std::string& id, std::string& error) {
    m_plugin = factory->create_plugin(factory, &m_host, id.c_str());
    if (m_plugin == nullptr) {
        error = "the plugin '" + id + "' could not be created";
        return false;
    }
    if (!m_plugin->init(m_plugin)) {
        error = "the plugin '" + id + "' failed to initialise";
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
        return false;
    }
    m_params = static_cast<const clap_plugin_params_t*>(m_plugin->get_extension(m_plugin, CLAP_EXT_PARAMS));
    m_latency = static_cast<const clap_plugin_latency_t*>(m_plugin->get_extension(m_plugin, CLAP_EXT_LATENCY));
    m_state = static_cast<const clap_plugin_state_t*>(m_plugin->get_extension(m_plugin, CLAP_EXT_STATE));
    m_gui = static_cast<const clap_plugin_gui_t*>(m_plugin->get_extension(m_plugin, CLAP_EXT_GUI));
    // Stereo in and out, or at least one port of up to two channels each way.
    if (const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(m_plugin->get_extension(m_plugin, CLAP_EXT_AUDIO_PORTS))) {
        if (ports->count(m_plugin, true) < 1 || ports->count(m_plugin, false) < 1) {
            error = "the plugin has no audio input or output (not an audio effect)";
            return false;
        }
    }
    if (m_params != nullptr) {
        const uint32_t count = m_params->count(m_plugin);
        for (uint32_t i = 0; i < count; ++i) {
            clap_param_info_t info{};
            if (m_params->get_info(m_plugin, i, &info)) {
                m_cookies.emplace_back(info.id, info.cookie);
            }
        }
        std::sort(m_cookies.begin(), m_cookies.end());
    }
    return true;
}

std::vector<ParamInfo> ClapInstance::params() {
    std::vector<ParamInfo> out;
    if (m_params == nullptr) {
        return out;
    }
    const uint32_t count = m_params->count(m_plugin);
    for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!m_params->get_info(m_plugin, i, &info)) {
            continue;
        }
        ParamInfo p;
        p.id = info.id;
        p.name = info.name;
        p.module = info.module;
        p.minValue = info.min_value;
        p.maxValue = info.max_value;
        p.defaultValue = info.default_value;
        p.automatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
        p.stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        p.hidden = (info.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) != 0;
        out.push_back(std::move(p));
    }
    return out;
}

std::optional<double> ClapInstance::paramValue(std::uint32_t id) {
    double v = 0.0;
    if (m_params != nullptr && m_params->get_value(m_plugin, id, &v)) {
        return v;
    }
    return std::nullopt;
}

bool ClapInstance::activate(double sampleRate, std::string& error) {
    if (m_active) {
        return true;
    }
    if (!m_plugin->activate(m_plugin, sampleRate, 1, static_cast<uint32_t>(kBlockFrames))) {
        error = "the plugin refused to activate at " + std::to_string(static_cast<int>(sampleRate)) + " Hz";
        return false;
    }
    m_active = true;
    return true;
}

void ClapInstance::deactivate() {
    if (m_active) {
        m_plugin->deactivate(m_plugin);
        m_active = false;
        m_processing = false;
    }
}

int ClapInstance::latency() { return m_latency != nullptr && m_active ? static_cast<int>(m_latency->get(m_plugin)) : 0; }

uint32_t CLAP_ABI ClapInstance::inSize(const clap_input_events_t* list) {
    return static_cast<uint32_t>(static_cast<const ClapInstance*>(list->ctx)->m_events.size());
}

const clap_event_header_t* CLAP_ABI ClapInstance::inGet(const clap_input_events_t* list, uint32_t index) {
    const auto* self = static_cast<const ClapInstance*>(list->ctx);
    return index < self->m_events.size() ? &self->m_events[index].header : nullptr;
}

bool ClapInstance::process(BlockSlot& slot) noexcept {
    tAudioThread = true;
    if (!m_processing) {
        if (!m_plugin->start_processing(m_plugin)) {
            return false;
        }
        m_processing = true;
    }
    m_events.clear();
    for (std::uint32_t i = 0; i < slot.eventCount && i < static_cast<std::uint32_t>(kMaxBlockEvents); ++i) {
        const ParamEvent& e = slot.events[i];
        clap_event_param_value_t ev{};
        ev.header.size = sizeof ev;
        ev.header.time = std::min<std::uint32_t>(e.offset, slot.frames > 0 ? slot.frames - 1 : 0);
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = e.param;
        const auto c = std::lower_bound(m_cookies.begin(), m_cookies.end(), std::pair<clap_id, void*>{e.param, nullptr},
                                        [](const auto& a, const auto& b) { return a.first < b.first; });
        ev.cookie = c != m_cookies.end() && c->first == e.param ? c->second : nullptr;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = e.value;
        m_events.push_back(ev);
    }
    float* in[kMaxChannels] = {slot.input[0], slot.input[1]};
    float* out[kMaxChannels] = {slot.output[0], slot.output[1]};
    clap_audio_buffer_t input{};
    input.data32 = in;
    input.channel_count = 2;
    clap_audio_buffer_t output{};
    output.data32 = out;
    output.channel_count = 2;
    const clap_input_events_t inEvents{this, inSize, inGet};
    const clap_output_events_t outEvents{this, outTryPush};
    clap_process_t p{};
    p.steady_time = -1;
    p.frames_count = slot.frames;
    p.audio_inputs = &input;
    p.audio_outputs = &output;
    p.audio_inputs_count = 1;
    p.audio_outputs_count = 1;
    p.in_events = &inEvents;
    p.out_events = &outEvents;
    const clap_process_status status = m_plugin->process(m_plugin, &p);
    return status != CLAP_PROCESS_ERROR;
}

void ClapInstance::reset() noexcept {
    tAudioThread = true;
    if (m_active) {
        m_plugin->reset(m_plugin);
    }
}

std::optional<std::vector<std::uint8_t>> ClapInstance::saveState() {
    if (m_state == nullptr) {
        return std::vector<std::uint8_t>{};
    }
    std::vector<std::uint8_t> bytes;
    clap_ostream_t stream{&bytes, [](const clap_ostream_t* s, const void* buffer, uint64_t size) -> int64_t {
                              auto* out = static_cast<std::vector<std::uint8_t>*>(s->ctx);
                              const auto* p = static_cast<const std::uint8_t*>(buffer);
                              out->insert(out->end(), p, p + size);
                              return static_cast<int64_t>(size);
                          }};
    if (!m_state->save(m_plugin, &stream)) {
        return std::nullopt;
    }
    return bytes;
}

bool ClapInstance::loadState(const std::vector<std::uint8_t>& state) {
    if (m_state == nullptr) {
        return state.empty();
    }
    struct Reader {
        const std::vector<std::uint8_t>* data;
        std::size_t pos;
    } reader{&state, 0};
    clap_istream_t stream{&reader, [](const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t {
                              auto* r = static_cast<Reader*>(s->ctx);
                              const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(size), r->data->size() - r->pos);
                              std::memcpy(buffer, r->data->data() + r->pos, n);
                              r->pos += n;
                              return static_cast<int64_t>(n);
                          }};
    return m_state->load(m_plugin, &stream);
}

bool ClapInstance::hasEditor() {
    return m_gui != nullptr && (m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_WIN32, false) ||
                                m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_WIN32, true));
}

bool ClapInstance::openEditor(const std::string& title, std::string& error) {
    if (m_window) {
        ShowWindow(static_cast<HWND>(m_window->hwnd()), SW_SHOW);
        SetForegroundWindow(static_cast<HWND>(m_window->hwnd()));
        return true;
    }
    if (!hasEditor()) {
        error = "the plugin has no editor";
        return false;
    }
    const bool embedded = m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_WIN32, false);
    if (!m_gui->create(m_plugin, CLAP_WINDOW_API_WIN32, !embedded)) {
        error = "the plugin could not create its editor";
        return false;
    }
    m_guiCreated = true;
    uint32_t width = 640;
    uint32_t height = 400;
    m_gui->get_size(m_plugin, &width, &height);
    if (!embedded) {
        m_gui->suggest_title(m_plugin, title.c_str());
        m_gui->show(m_plugin);
        return true;
    }
    EditorWindow::Callbacks callbacks;
    callbacks.closed = [this] {
        destroyGui();
        if (m_callbacks.editorClosed) {
            m_callbacks.editorClosed();
        }
    };
    callbacks.resized = [this](int w, int h) {
        if (m_gui != nullptr && m_guiCreated && m_gui->can_resize(m_plugin)) {
            auto cw = static_cast<uint32_t>(w);
            auto ch = static_cast<uint32_t>(h);
            m_gui->adjust_size(m_plugin, &cw, &ch);
            m_gui->set_size(m_plugin, cw, ch);
        }
    };
    m_window = EditorWindow::create(title, static_cast<int>(width), static_cast<int>(height), m_gui->can_resize(m_plugin),
                                    std::move(callbacks), error);
    if (!m_window) {
        destroyGui();
        return false;
    }
    clap_window_t window{};
    window.api = CLAP_WINDOW_API_WIN32;
    window.win32 = m_window->hwnd();
    if (!m_gui->set_parent(m_plugin, &window)) {
        error = "the plugin could not attach its editor";
        destroyGui();
        return false;
    }
    m_gui->show(m_plugin);
    m_window->show();
    return true;
}

void ClapInstance::closeEditor() { destroyGui(); }

void ClapInstance::destroyGui() {
    if (m_guiCreated && m_gui != nullptr) {
        m_gui->hide(m_plugin);
        m_gui->destroy(m_plugin);
    }
    m_guiCreated = false;
    m_window.reset();
}

const void* CLAP_ABI ClapInstance::hostGetExtension(const clap_host_t*, const char* id) {
    static const clap_host_log_t log{hostLog};
    static const clap_host_thread_check_t threadCheck{hostIsMainThread, hostIsAudioThread};
    static const clap_host_params_t params{hostParamsRescan, hostParamsClear, hostParamsRequestFlush};
    static const clap_host_state_t state{hostStateMarkDirty};
    static const clap_host_latency_t latency{hostLatencyChanged};
    static const clap_host_gui_t gui{hostGuiResizeHintsChanged, hostGuiRequestResize, hostGuiRequestShow, hostGuiRequestHide,
                                     hostGuiClosed};
    if (std::strcmp(id, CLAP_EXT_LOG) == 0) {
        return &log;
    }
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) {
        return &threadCheck;
    }
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &params;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &state;
    }
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) {
        return &latency;
    }
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
        return &gui;
    }
    return nullptr;
}

void CLAP_ABI ClapInstance::hostRequestRestart(const clap_host_t* host) { hostLatencyChanged(host); }

void CLAP_ABI ClapInstance::hostRequestCallback(const clap_host_t* host) {
    ClapInstance* instance = self(host);
    postToMainThread([instance] {
        if (instance->m_plugin != nullptr) {
            instance->m_plugin->on_main_thread(instance->m_plugin);
        }
    });
}

void CLAP_ABI ClapInstance::hostLog(const clap_host_t*, clap_log_severity severity, const char* msg) {
    if (severity >= CLAP_LOG_ERROR && msg != nullptr) {
        std::fprintf(stderr, "[clap] %s\n", msg);
    }
}

void CLAP_ABI ClapInstance::hostStateMarkDirty(const clap_host_t* host) {
    ClapInstance* instance = self(host);
    if (instance->m_callbacks.stateChanged) {
        instance->m_callbacks.stateChanged();
    }
}

void CLAP_ABI ClapInstance::hostLatencyChanged(const clap_host_t* host) {
    ClapInstance* instance = self(host);
    if (instance->m_callbacks.latencyChanged) {
        instance->m_callbacks.latencyChanged();
    }
}

bool CLAP_ABI ClapInstance::hostGuiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height) {
    ClapInstance* instance = self(host);
    if (instance->m_window) {
        instance->m_window->setClientSize(static_cast<int>(width), static_cast<int>(height));
        return true;
    }
    return false;
}

void CLAP_ABI ClapInstance::hostGuiClosed(const clap_host_t* host, bool) {
    ClapInstance* instance = self(host);
    postToMainThread([instance] {
        instance->destroyGui();
        if (instance->m_callbacks.editorClosed) {
            instance->m_callbacks.editorClosed();
        }
    });
}

} // namespace

std::shared_ptr<PluginModule> loadClapModule(const std::filesystem::path& path, std::string& error) {
    HMODULE library = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (library == nullptr) {
        error = "cannot load " + utf8Path(path) + " (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    const auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(library, "clap_entry"));
    if (entry == nullptr || !clap_version_is_compatible(entry->clap_version)) {
        error = utf8Path(path) + " is not a compatible CLAP plugin";
        FreeLibrary(library);
        return nullptr;
    }
    if (!entry->init(narrow(path.wstring()).c_str())) {
        error = utf8Path(path) + ": the CLAP entry failed to initialise";
        FreeLibrary(library);
        return nullptr;
    }
    return std::make_shared<ClapModule>(library, entry);
}

} // namespace wabcut::plughost
