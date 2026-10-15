// wabcut-plughost: VST3 plugins (the Steinberg VST3 SDK 3.8, MIT). One PluginModule per binary (a .vst3 file, or the
// binary inside a .vst3 bundle), one Vst3Instance per plugin instance: component + edit controller (separate or a
// single component), connected, with the controller's edits (its editor) forwarded to the processor.

#include "Editor.h"
#include "HostedPlugin.h"

#include <wabcut/plughost/Ipc.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wabcut::plughost {

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

std::string utf8(const char16_t* text) {
    std::wstring w;
    for (const char16_t* p = text; p != nullptr && *p != 0; ++p) {
        w.push_back(static_cast<wchar_t>(*p));
    }
    return narrow(w);
}

std::string hexId(const TUID id) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (int i = 0; i < 16; ++i) {
        const auto b = static_cast<unsigned char>(id[i]);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 15]);
    }
    return out;
}

bool parseId(const std::string& text, TUID out) {
    if (text.size() != 32) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        unsigned value = 0;
        if (std::sscanf(text.c_str() + 2 * i, "%2X", &value) != 1) {
            return false;
        }
        out[i] = static_cast<char>(value);
    }
    return true;
}

/// An in-memory IBStream.
class MemoryStream final : public U::Implements<U::Directly<IBStream>> {
public:
    MemoryStream() = default;
    explicit MemoryStream(std::vector<std::uint8_t> data) : m_data(std::move(data)) {}

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        const auto n = static_cast<std::size_t>(std::clamp<int64>(static_cast<int64>(m_data.size()) - m_pos, 0, numBytes));
        if (n > 0) {
            std::memcpy(buffer, m_data.data() + m_pos, n);
        }
        m_pos += static_cast<int64>(n);
        if (numBytesRead != nullptr) {
            *numBytesRead = static_cast<int32>(n);
        }
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (numBytes < 0) {
            return kInvalidArgument;
        }
        const auto end = static_cast<std::size_t>(m_pos) + static_cast<std::size_t>(numBytes);
        if (end > m_data.size()) {
            m_data.resize(end);
        }
        std::memcpy(m_data.data() + m_pos, buffer, static_cast<std::size_t>(numBytes));
        m_pos += numBytes;
        if (numBytesWritten != nullptr) {
            *numBytesWritten = numBytes;
        }
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 next = pos;
        if (mode == kIBSeekCur) {
            next = m_pos + pos;
        } else if (mode == kIBSeekEnd) {
            next = static_cast<int64>(m_data.size()) + pos;
        }
        if (next < 0) {
            return kInvalidArgument;
        }
        m_pos = next;
        if (result != nullptr) {
            *result = m_pos;
        }
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (pos == nullptr) {
            return kInvalidArgument;
        }
        *pos = m_pos;
        return kResultOk;
    }

    const std::vector<std::uint8_t>& data() const { return m_data; }

private:
    std::vector<std::uint8_t> m_data;
    int64 m_pos = 0;
};

class Vst3Instance;

/// The controller's channel back to the host: edits from its editor and restart requests.
class ComponentHandler final : public U::Implements<U::Directly<IComponentHandler>> {
public:
    explicit ComponentHandler(Vst3Instance* owner) : m_owner(owner) {}
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID) override;
    tresult PLUGIN_API restartComponent(int32 flags) override;
    void detach() { m_owner = nullptr; }

private:
    Vst3Instance* m_owner;
};

/// The editor window's side of IPlugView.
class PlugFrame final : public U::Implements<U::Directly<IPlugFrame>> {
public:
    explicit PlugFrame(EditorWindow* window) : m_window(window) {}
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override {
        if (m_window == nullptr || view == nullptr || size == nullptr) {
            return kInvalidArgument;
        }
        m_window->setClientSize(size->getWidth(), size->getHeight());
        view->onSize(size);
        return kResultOk;
    }
    void detach() { m_window = nullptr; }

private:
    EditorWindow* m_window;
};

class Vst3Module;

class Vst3Instance final : public HostedInstance {
public:
    Vst3Instance(std::shared_ptr<Vst3Module> module, InstanceCallbacks callbacks)
        : m_module(std::move(module)), m_callbacks(std::move(callbacks)) {}
    ~Vst3Instance() override;

    bool init(IPluginFactory* factory, const std::string& id, std::string& error);

    std::vector<ParamInfo> params() override;
    std::optional<double> paramValue(std::uint32_t id) override;
    bool activate(double sampleRate, std::string& error) override;
    void deactivate() override;
    int latency() override { return m_processor != nullptr ? static_cast<int>(m_processor->getLatencySamples()) : 0; }
    bool process(BlockSlot& slot) noexcept override;
    void reset() noexcept override;
    std::optional<std::vector<std::uint8_t>> saveState() override;
    bool loadState(const std::vector<std::uint8_t>& state) override;
    bool hasEditor() override;
    bool openEditor(const std::string& title, std::string& error) override;
    void closeEditor() override;

    // ComponentHandler.
    void edited(ParamID id, ParamValue value) { m_transfer.addChange(id, value, 0); }
    void editEnded() {
        if (m_callbacks.stateChanged) {
            m_callbacks.stateChanged();
        }
    }
    void restartRequested(int32 flags) {
        if ((flags & kLatencyChanged) != 0 && m_callbacks.latencyChanged) {
            m_callbacks.latencyChanged();
        }
    }

private:
    void destroyEditor();

    std::shared_ptr<Vst3Module> m_module;
    InstanceCallbacks m_callbacks;
    IPtr<IComponent> m_component;
    IPtr<IAudioProcessor> m_processor;
    IPtr<IEditController> m_controller;
    bool m_singleComponent = false;
    IPtr<IConnectionPoint> m_componentPoint;
    IPtr<IConnectionPoint> m_controllerPoint;
    IPtr<ComponentHandler> m_handler;
    bool m_active = false;
    bool m_processing = false;
    double m_sampleRate = 48000.0;
    std::int64_t m_position = 0; ///< continuous sample position for ProcessContext

    ParameterChanges m_changes{kMaxBlockEvents};
    ParameterChanges m_outChanges{64};
    ParameterChangeTransfer m_transfer{256};

    std::unique_ptr<EditorWindow> m_window;
    IPtr<IPlugView> m_view;
    IPtr<PlugFrame> m_frame;
};

tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue value) {
    if (m_owner != nullptr) {
        m_owner->edited(id, value);
    }
    return kResultOk;
}
tresult PLUGIN_API ComponentHandler::endEdit(ParamID) {
    if (m_owner != nullptr) {
        m_owner->editEnded();
    }
    return kResultOk;
}
tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags) {
    if (m_owner != nullptr) {
        m_owner->restartRequested(flags);
    }
    return kResultOk;
}

class Vst3Module final : public PluginModule, public std::enable_shared_from_this<Vst3Module> {
public:
    Vst3Module(HMODULE library, IPluginFactory* factory) : m_library(library), m_factory(factory) {}
    ~Vst3Module() override {
        m_factory = nullptr; // releases
        if (m_library != nullptr) {
            using ExitProc = bool(PLUGIN_API*)();
            if (auto exit = reinterpret_cast<ExitProc>(GetProcAddress(m_library, "ExitDll"))) {
                exit();
            }
            FreeLibrary(m_library);
        }
    }

    FUnknown* hostContext() { return m_host; }

    std::vector<PluginDescription> plugins() override {
        std::vector<PluginDescription> out;
        PFactoryInfo factoryInfo;
        m_factory->getFactoryInfo(&factoryInfo);
        IPtr<IPluginFactory2> factory2 = U::cast<IPluginFactory2>(m_factory);
        const int32 count = m_factory->countClasses();
        for (int32 i = 0; i < count; ++i) {
            PClassInfo info;
            if (m_factory->getClassInfo(i, &info) != kResultOk || std::strcmp(info.category, kVstAudioEffectClass) != 0) {
                continue;
            }
            PluginDescription p;
            p.format = "vst3";
            p.id = hexId(info.cid);
            p.name = info.name;
            p.vendor = factoryInfo.vendor;
            PClassInfo2 info2;
            if (factory2 && factory2->getClassInfo2(i, &info2) == kResultOk) {
                p.category = info2.subCategories;
                if (info2.vendor[0] != 0) {
                    p.vendor = info2.vendor;
                }
                p.version = info2.version;
            }
            p.effect = p.category.find("Instrument") == std::string::npos || p.category.find("Fx") != std::string::npos;
            out.push_back(std::move(p));
        }
        return out;
    }

    std::unique_ptr<HostedInstance> create(const std::string& id, InstanceCallbacks callbacks, std::string& error) override {
        auto instance = std::make_unique<Vst3Instance>(shared_from_this(), std::move(callbacks));
        if (!instance->init(m_factory, id, error)) {
            return nullptr;
        }
        return instance;
    }

private:
    HMODULE m_library = nullptr;
    IPtr<IPluginFactory> m_factory;
    IPtr<HostApplication> m_host = owned(new HostApplication());
};

// ---- Vst3Instance -----------------------------------------------------------------------------------------------

Vst3Instance::~Vst3Instance() {
    destroyEditor();
    deactivate();
    if (m_componentPoint && m_controllerPoint) {
        m_componentPoint->disconnect(m_controllerPoint);
        m_controllerPoint->disconnect(m_componentPoint);
    }
    if (m_controller) {
        m_controller->setComponentHandler(nullptr);
        if (!m_singleComponent) {
            m_controller->terminate();
        }
    }
    if (m_handler) {
        m_handler->detach();
    }
    m_controller = nullptr;
    m_processor = nullptr;
    if (m_component) {
        m_component->terminate();
    }
    m_component = nullptr;
}

bool Vst3Instance::init(IPluginFactory* factory, const std::string& id, std::string& error) {
    TUID cid;
    if (!parseId(id, cid)) {
        error = "'" + id + "' is not a VST3 class id";
        return false;
    }
    IComponent* component = nullptr;
    if (factory->createInstance(cid, IComponent::iid, reinterpret_cast<void**>(&component)) != kResultOk || component == nullptr) {
        error = "the VST3 component could not be created";
        return false;
    }
    m_component = owned(component);
    if (m_component->initialize(m_module->hostContext()) != kResultOk) {
        error = "the VST3 component failed to initialise";
        m_component = nullptr;
        return false;
    }
    m_processor = U::cast<IAudioProcessor>(m_component);
    if (!m_processor) {
        error = "the VST3 component is not an audio processor";
        return false;
    }
    TUID controllerId;
    if (m_component->getControllerClassId(controllerId) == kResultOk) {
        IEditController* controller = nullptr;
        if (factory->createInstance(controllerId, IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk &&
            controller != nullptr) {
            m_controller = owned(controller);
            if (m_controller->initialize(m_module->hostContext()) != kResultOk) {
                m_controller = nullptr;
            }
        }
    }
    if (!m_controller) {
        m_controller = U::cast<IEditController>(m_component);
        m_singleComponent = static_cast<bool>(m_controller);
    }
    if (m_controller && !m_singleComponent) {
        m_componentPoint = U::cast<IConnectionPoint>(m_component);
        m_controllerPoint = U::cast<IConnectionPoint>(m_controller);
        if (m_componentPoint && m_controllerPoint) {
            m_componentPoint->connect(m_controllerPoint);
            m_controllerPoint->connect(m_componentPoint);
        }
        // The controller starts from the component's state.
        IPtr<MemoryStream> state = owned(new MemoryStream());
        if (m_component->getState(state) == kResultOk) {
            state->seek(0, IBStream::kIBSeekSet, nullptr);
            m_controller->setComponentState(state);
        }
    }
    if (m_controller) {
        m_handler = owned(new ComponentHandler(this));
        m_controller->setComponentHandler(m_handler);
    }
    // Stereo in, stereo out.
    SpeakerArrangement in = SpeakerArr::kStereo;
    SpeakerArrangement out = SpeakerArr::kStereo;
    if (m_component->getBusCount(kAudio, kInput) < 1 || m_component->getBusCount(kAudio, kOutput) < 1) {
        error = "the plugin has no audio input or output (not an audio effect)";
        return false;
    }
    if (m_processor->setBusArrangements(&in, 1, &out, 1) != kResultTrue) {
        SpeakerArrangement current = 0;
        if (m_processor->getBusArrangement(kInput, 0, current) != kResultTrue || SpeakerArr::getChannelCount(current) != 2) {
            error = "the plugin does not process stereo";
            return false;
        }
    }
    m_component->activateBus(kAudio, kInput, 0, true);
    m_component->activateBus(kAudio, kOutput, 0, true);
    return true;
}

std::vector<ParamInfo> Vst3Instance::params() {
    std::vector<ParamInfo> out;
    if (!m_controller) {
        return out;
    }
    const int32 count = m_controller->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        ParameterInfo info{};
        if (m_controller->getParameterInfo(i, info) != kResultOk) {
            continue;
        }
        ParamInfo p;
        p.id = info.id;
        p.name = utf8(info.title);
        p.unit = utf8(info.units);
        p.minValue = 0.0;
        p.maxValue = 1.0;
        p.defaultValue = info.defaultNormalizedValue;
        p.automatable = (info.flags & ParameterInfo::kCanAutomate) != 0;
        p.stepped = info.stepCount > 0;
        p.hidden = (info.flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden)) != 0;
        out.push_back(std::move(p));
    }
    return out;
}

std::optional<double> Vst3Instance::paramValue(std::uint32_t id) {
    if (!m_controller) {
        return std::nullopt;
    }
    return m_controller->getParamNormalized(id);
}

bool Vst3Instance::activate(double sampleRate, std::string& error) {
    if (m_active) {
        return true;
    }
    m_sampleRate = sampleRate;
    ProcessSetup setup{kRealtime, kSample32, kBlockFrames, sampleRate};
    if (m_processor->setupProcessing(setup) != kResultOk) {
        error = "the plugin refused the processing setup (" + std::to_string(static_cast<int>(sampleRate)) + " Hz)";
        return false;
    }
    if (m_component->setActive(true) != kResultOk) {
        error = "the plugin refused to activate";
        return false;
    }
    m_active = true;
    return true;
}

void Vst3Instance::deactivate() {
    if (!m_active) {
        return;
    }
    if (m_processing) {
        m_processor->setProcessing(false);
        m_processing = false;
    }
    m_component->setActive(false);
    m_active = false;
}

bool Vst3Instance::process(BlockSlot& slot) noexcept {
    if (!m_processing) {
        m_processor->setProcessing(true);
        m_processing = true;
    }
    m_changes.clearQueue();
    for (std::uint32_t i = 0; i < slot.eventCount && i < static_cast<std::uint32_t>(kMaxBlockEvents); ++i) {
        const ParamEvent& e = slot.events[i];
        int32 queueIndex = 0;
        if (IParamValueQueue* queue = m_changes.addParameterData(e.param, queueIndex)) {
            int32 point = 0;
            queue->addPoint(static_cast<int32>(std::min<std::uint32_t>(e.offset, slot.frames > 0 ? slot.frames - 1 : 0)),
                            std::clamp(e.value, 0.0, 1.0), point);
        }
    }
    m_transfer.transferChangesTo(m_changes); // edits made in the plugin's own editor
    m_outChanges.clearQueue();
    float* in[kMaxChannels] = {slot.input[0], slot.input[1]};
    float* out[kMaxChannels] = {slot.output[0], slot.output[1]};
    AudioBusBuffers inputs{};
    inputs.numChannels = 2;
    inputs.channelBuffers32 = in;
    AudioBusBuffers outputs{};
    outputs.numChannels = 2;
    outputs.channelBuffers32 = out;
    ProcessContext context{};
    context.state = ProcessContext::kContTimeValid;
    context.sampleRate = m_sampleRate;
    context.projectTimeSamples = m_position;
    context.continousTimeSamples = m_position;
    ProcessData data;
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = static_cast<int32>(slot.frames);
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inputs;
    data.outputs = &outputs;
    data.inputParameterChanges = &m_changes;
    data.outputParameterChanges = &m_outChanges;
    data.processContext = &context;
    m_position += slot.frames;
    return m_processor->process(data) == kResultOk;
}

void Vst3Instance::reset() noexcept {
    // VST3 has no reset: stopping and restarting processing clears a plugin's buffers and envelopes.
    if (m_processing) {
        m_processor->setProcessing(false);
        m_processor->setProcessing(true);
    }
    m_position = 0;
}

std::optional<std::vector<std::uint8_t>> Vst3Instance::saveState() {
    // "WV31", the component's state, the controller's state (each with a 32-bit length).
    IPtr<MemoryStream> component = owned(new MemoryStream());
    if (m_component->getState(component) != kResultOk) {
        return std::nullopt;
    }
    IPtr<MemoryStream> controller = owned(new MemoryStream());
    if (m_controller && !m_singleComponent) {
        m_controller->getState(controller);
    }
    std::vector<std::uint8_t> out{'W', 'V', '3', '1'};
    const auto append = [&](const std::vector<std::uint8_t>& part) {
        const auto n = static_cast<std::uint32_t>(part.size());
        for (int k = 0; k < 4; ++k) {
            out.push_back(static_cast<std::uint8_t>(n >> (8 * k)));
        }
        out.insert(out.end(), part.begin(), part.end());
    };
    append(component->data());
    append(controller->data());
    return out;
}

bool Vst3Instance::loadState(const std::vector<std::uint8_t>& state) {
    if (state.size() < 12 || std::memcmp(state.data(), "WV31", 4) != 0) {
        return false;
    }
    std::size_t pos = 4;
    const auto part = [&]() -> std::optional<std::vector<std::uint8_t>> {
        if (pos + 4 > state.size()) {
            return std::nullopt;
        }
        std::uint32_t n = 0;
        for (int k = 0; k < 4; ++k) {
            n |= static_cast<std::uint32_t>(state[pos + static_cast<std::size_t>(k)]) << (8 * k);
        }
        pos += 4;
        if (pos + n > state.size()) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes(state.begin() + static_cast<std::ptrdiff_t>(pos), state.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return bytes;
    };
    const auto component = part();
    const auto controller = part();
    if (!component || !controller) {
        return false;
    }
    IPtr<MemoryStream> c = owned(new MemoryStream(*component));
    if (m_component->setState(c) != kResultOk) {
        return false;
    }
    if (m_controller) {
        c->seek(0, IBStream::kIBSeekSet, nullptr);
        m_controller->setComponentState(c);
        if (!m_singleComponent && !controller->empty()) {
            IPtr<MemoryStream> e = owned(new MemoryStream(*controller));
            m_controller->setState(e);
        }
    }
    return true;
}

bool Vst3Instance::hasEditor() {
    if (!m_controller) {
        return false;
    }
    IPtr<IPlugView> view = owned(m_controller->createView(ViewType::kEditor));
    return view && view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue;
}

bool Vst3Instance::openEditor(const std::string& title, std::string& error) {
    if (m_window) {
        ShowWindow(static_cast<HWND>(m_window->hwnd()), SW_SHOW);
        SetForegroundWindow(static_cast<HWND>(m_window->hwnd()));
        return true;
    }
    if (!m_controller) {
        error = "the plugin has no editor";
        return false;
    }
    m_view = owned(m_controller->createView(ViewType::kEditor));
    if (!m_view || m_view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
        m_view = nullptr;
        error = "the plugin has no editor";
        return false;
    }
    ViewRect rect{0, 0, 640, 400};
    m_view->getSize(&rect);
    EditorWindow::Callbacks callbacks;
    callbacks.closed = [this] {
        destroyEditor();
        if (m_callbacks.editorClosed) {
            m_callbacks.editorClosed();
        }
    };
    callbacks.resized = [this](int w, int h) {
        if (m_view && m_view->canResize() == kResultTrue) {
            ViewRect r{0, 0, w, h};
            m_view->checkSizeConstraint(&r);
            m_view->onSize(&r);
        }
    };
    m_window = EditorWindow::create(title, rect.getWidth(), rect.getHeight(), m_view->canResize() == kResultTrue,
                                    std::move(callbacks), error);
    if (!m_window) {
        m_view = nullptr;
        return false;
    }
    m_frame = owned(new PlugFrame(m_window.get()));
    m_view->setFrame(m_frame);
    if (m_view->attached(m_window->hwnd(), kPlatformTypeHWND) != kResultOk) {
        error = "the plugin could not attach its editor";
        destroyEditor();
        return false;
    }
    m_window->show();
    return true;
}

void Vst3Instance::closeEditor() { destroyEditor(); }

void Vst3Instance::destroyEditor() {
    if (m_view) {
        m_view->removed();
        m_view->setFrame(nullptr);
        m_view = nullptr;
    }
    if (m_frame) {
        m_frame->detach();
        m_frame = nullptr;
    }
    m_window.reset();
}

/// The binary of a .vst3 path: the file itself, or the bundle's Contents/x86_64-win/<name>.vst3 - or, when the
/// vendor named it otherwise (ElgatoEq.vst3 holds ElgatoEq_v1.3.0.110.vst3), the one .vst3 file there.
std::filesystem::path binaryOf(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return path;
    }
    const std::filesystem::path folder = path / "Contents" / "x86_64-win";
    if (std::filesystem::exists(folder / path.filename(), ec)) {
        return folder / path.filename();
    }
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".vst3") {
            return entry.path();
        }
    }
    return folder / path.filename();
}

} // namespace

std::shared_ptr<PluginModule> loadVst3Module(const std::filesystem::path& path, std::string& error) {
    const std::filesystem::path binary = binaryOf(path);
    HMODULE library = LoadLibraryExW(binary.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (library == nullptr) {
        error = "cannot load " + utf8Path(binary) + " (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    using InitProc = bool(PLUGIN_API*)();
    if (auto init = reinterpret_cast<InitProc>(GetProcAddress(library, "InitDll")); init != nullptr && !init()) {
        error = utf8Path(binary) + ": InitDll failed";
        FreeLibrary(library);
        return nullptr;
    }
    using FactoryProc = IPluginFactory*(PLUGIN_API*)();
    auto getFactory = reinterpret_cast<FactoryProc>(GetProcAddress(library, "GetPluginFactory"));
    IPluginFactory* factory = getFactory != nullptr ? getFactory() : nullptr;
    if (factory == nullptr) {
        error = utf8Path(binary) + " is not a VST3 plugin (no GetPluginFactory)";
        FreeLibrary(library);
        return nullptr;
    }
    return std::make_shared<Vst3Module>(library, factory);
}

} // namespace wabcut::plughost
