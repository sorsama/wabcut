// A minimal CLAP effect for the plugin-host tests (A.6): stereo gain with sample-accurate parameter events, a
// latency it reports (a plain delay), a deliberate crash, a stall (to miss deadlines), bad samples and its state.
//
//   param 1 "Gain"     dB, -60..12, automatable: applied from the event's frame on
//   param 2 "Latency"  frames, 0..4800, not automatable: reported from activation; a change while active delays the
//                      sound by the new amount at once and asks the host for a restart (as a plugin that changes its
//                      latency from its own editor does), after which it reports it
//   param 3 "Crash"    0/1: on 1 the next process() dereferences null (the helper dies)
//   param 4 "Stall"    ms, 0..2000: process() sleeps that long (the host misses its deadline)
//   param 5 "Bad"      0/1/2: while 1 its output is NaN, while 2 it is 1e30
//
// The state may also carry (only then is it the longer blob) a delay in ms that loading and saving the state take (a
// slow plugin) and a flag that makes process() print to stdout (a chatty plugin).
//
// Built three times: the plugin, and (WABCUT_TEST_CRASH_ON_SCAN / WABCUT_TEST_HANG_ON_SCAN) one that crashes or hangs
// while being scanned, for the scanner's blocklist.

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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr clap_id kGain = 1;
constexpr clap_id kLatency = 2;
constexpr clap_id kCrash = 3;
constexpr clap_id kStall = 4;
constexpr clap_id kBad = 5;
/// The delay line holds the longest latency: a latency change needs no allocation on the audio thread.
constexpr std::size_t kDelayFrames = 4801;

struct Plugin {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;
    double gainDb = 0.0;
    double latency = 0.0;
    double crash = 0.0;
    double stall = 0.0;
    double bad = 0.0;
    double stateMs = 0.0;  ///< loading and saving the state take this long
    double chatter = 0.0;  ///< process() prints to stdout
    int activeLatency = 0; ///< what it reports (from its last activation)
    std::atomic<bool> restartAsked{false};
    std::vector<float> delay[2];
    std::size_t delayPos = 0;
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "org.wabcut.test.gain",
    "Wabcut Test Gain",
    "Wabcut",
    "https://github.com/wabcut",
    "",
    "",
    "1.0.0",
    "Gain, latency, crash and stall for Wabcut's plugin-host tests",
    kFeatures,
};

void setParam(Plugin* p, clap_id id, double value) {
    switch (id) {
    case kGain:
        p->gainDb = std::clamp(value, -60.0, 12.0);
        break;
    case kLatency:
        p->latency = std::clamp(std::round(value), 0.0, 4800.0);
        break;
    case kCrash:
        p->crash = value;
        break;
    case kStall:
        p->stall = std::clamp(value, 0.0, 2000.0);
        break;
    case kBad:
        p->bad = std::clamp(std::round(value), 0.0, 2.0);
        break;
    default:
        break;
    }
}

// ---- params -----------------------------------------------------------------------------------------------------

uint32_t CLAP_ABI paramsCount(const clap_plugin_t*) { return 5; }

bool CLAP_ABI paramsInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    std::memset(info, 0, sizeof *info);
    switch (index) {
    case 0:
        info->id = kGain;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        std::strcpy(info->name, "Gain");
        info->min_value = -60.0;
        info->max_value = 12.0;
        info->default_value = 0.0;
        return true;
    case 1:
        info->id = kLatency;
        info->flags = CLAP_PARAM_IS_STEPPED;
        std::strcpy(info->name, "Latency");
        info->min_value = 0.0;
        info->max_value = 4800.0;
        return true;
    case 2:
        info->id = kCrash;
        info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
        std::strcpy(info->name, "Crash");
        info->max_value = 1.0;
        return true;
    case 3:
        info->id = kStall;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        std::strcpy(info->name, "Stall");
        info->max_value = 2000.0;
        return true;
    case 4:
        info->id = kBad;
        info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
        std::strcpy(info->name, "Bad");
        info->max_value = 2.0;
        return true;
    default:
        return false;
    }
}

bool CLAP_ABI paramsValue(const clap_plugin_t* plugin, clap_id id, double* value) {
    Plugin* p = self(plugin);
    switch (id) {
    case kGain:
        *value = p->gainDb;
        return true;
    case kLatency:
        *value = p->latency;
        return true;
    case kCrash:
        *value = p->crash;
        return true;
    case kStall:
        *value = p->stall;
        return true;
    case kBad:
        *value = p->bad;
        return true;
    default:
        return false;
    }
}

bool CLAP_ABI paramsValueToText(const clap_plugin_t*, clap_id, double value, char* text, uint32_t size) {
    std::snprintf(text, size, "%.2f", value);
    return true;
}

bool CLAP_ABI paramsTextToValue(const clap_plugin_t*, clap_id, const char* text, double* value) {
    *value = std::atof(text);
    return true;
}

void CLAP_ABI paramsFlush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t*) {
    Plugin* p = self(plugin);
    for (uint32_t i = 0; i < in->size(in); ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            setParam(p, e->param_id, e->value);
        }
    }
}

const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsValueToText, paramsTextToValue, paramsFlush};

// ---- latency, audio ports, state --------------------------------------------------------------------------------

uint32_t CLAP_ABI latencyGet(const clap_plugin_t* plugin) { return static_cast<uint32_t>(self(plugin)->activeLatency); }
const clap_plugin_latency_t kLatencyExt = {latencyGet};

uint32_t CLAP_ABI portsCount(const clap_plugin_t*, bool) { return 1; }
bool CLAP_ABI portsGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
    if (index != 0) {
        return false;
    }
    std::memset(info, 0, sizeof *info);
    info->id = isInput ? 0 : 1;
    std::strcpy(info->name, isInput ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

struct StateBlob {
    std::uint32_t magic = 0x57544731; // "WTG1"
    double gainDb, latency, crash, stall;
    double stateMs, chatter; ///< the longer blob only
};
constexpr std::size_t kShortBlob = offsetof(StateBlob, stateMs);

bool CLAP_ABI stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    const Plugin* p = self(plugin);
    if (p->stateMs > 0.0) {
        Sleep(static_cast<DWORD>(p->stateMs));
    }
    StateBlob blob;
    std::memset(&blob, 0, sizeof blob); // no stray padding bytes in the state
    blob.magic = 0x57544731;
    blob.gainDb = p->gainDb;
    blob.latency = p->latency;
    blob.crash = p->crash;
    blob.stall = p->stall;
    blob.stateMs = p->stateMs;
    blob.chatter = p->chatter;
    const std::size_t size = p->stateMs > 0.0 || p->chatter > 0.0 ? sizeof blob : kShortBlob;
    return stream->write(stream, &blob, size) == static_cast<int64_t>(size);
}

bool CLAP_ABI stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    StateBlob blob;
    std::memset(&blob, 0, sizeof blob);
    const int64_t got = stream->read(stream, &blob, sizeof blob);
    if ((got != static_cast<int64_t>(kShortBlob) && got != static_cast<int64_t>(sizeof blob)) || blob.magic != 0x57544731) {
        return false;
    }
    Plugin* p = self(plugin);
    setParam(p, kGain, blob.gainDb);
    setParam(p, kLatency, blob.latency);
    setParam(p, kCrash, blob.crash);
    setParam(p, kStall, blob.stall);
    p->stateMs = std::clamp(blob.stateMs, 0.0, 5000.0);
    p->chatter = blob.chatter;
    if (p->stateMs > 0.0) {
        Sleep(static_cast<DWORD>(p->stateMs));
    }
    return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

// ---- the plugin ---------------------------------------------------------------------------------------------------

bool CLAP_ABI pluginInit(const clap_plugin_t*) { return true; }
void CLAP_ABI pluginDestroy(const clap_plugin_t* plugin) { delete self(plugin); }

bool CLAP_ABI pluginActivate(const clap_plugin_t* plugin, double, uint32_t, uint32_t) {
    Plugin* p = self(plugin);
    p->activeLatency = static_cast<int>(p->latency);
    for (auto& d : p->delay) {
        d.assign(kDelayFrames, 0.0f);
    }
    p->delayPos = 0;
    p->restartAsked.store(false);
    return true;
}
void CLAP_ABI pluginDeactivate(const clap_plugin_t*) {}
bool CLAP_ABI pluginStart(const clap_plugin_t*) { return true; }
void CLAP_ABI pluginStop(const clap_plugin_t*) {}

void CLAP_ABI pluginReset(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    for (auto& d : p->delay) {
        std::fill(d.begin(), d.end(), 0.0f);
    }
    p->delayPos = 0;
}

clap_process_status CLAP_ABI pluginProcess(const clap_plugin_t* plugin, const clap_process_t* process) {
    Plugin* p = self(plugin);
    const uint32_t frames = process->frames_count;
    const uint32_t events = process->in_events->size(process->in_events);
    uint32_t next = 0;
    float* const* in = process->audio_inputs[0].data32;
    float* const* out = process->audio_outputs[0].data32;
    for (uint32_t k = 0; k < frames; ++k) {
        while (next < events) {
            const clap_event_header_t* h = process->in_events->get(process->in_events, next);
            if (h->time > k) {
                break;
            }
            if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
                setParam(p, e->param_id, e->value);
            }
            ++next;
        }
        if (p->crash > 0.5) {
            volatile int* nowhere = nullptr;
            *nowhere = 42; // the deliberate crash
        }
        const float g = static_cast<float>(std::pow(10.0, p->gainDb / 20.0));
        const auto latency = static_cast<std::size_t>(p->latency); // a change applies at once (see the file comment)
        for (int c = 0; c < 2; ++c) {
            // A plain delay of `latency` frames, then the gain.
            p->delay[c][p->delayPos] = in[c][k];
            out[c][k] = p->delay[c][(p->delayPos + kDelayFrames - latency) % kDelayFrames] * g;
            if (p->bad > 1.5) {
                out[c][k] = 1e30f;
            } else if (p->bad > 0.5) {
                out[c][k] = std::numeric_limits<float>::quiet_NaN();
            }
        }
        p->delayPos = (p->delayPos + 1) % kDelayFrames;
    }
    if (static_cast<int>(p->latency) != p->activeLatency && !p->restartAsked.exchange(true)) {
        p->host->request_restart(p->host); // so the host reads the new latency (thread-safe)
    }
    if (p->chatter > 0.0) {
        std::printf("chatter from process(), no newline ");
        std::fflush(stdout);
    }
    if (p->stall > 0.0) {
        Sleep(static_cast<DWORD>(p->stall));
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &kParams;
    }
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) {
        return &kLatencyExt;
    }
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &kPorts;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &kState;
    }
    return nullptr;
}

void CLAP_ABI pluginOnMainThread(const clap_plugin_t*) {}

// ---- factory and entry ------------------------------------------------------------------------------------------

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* CLAP_ABI factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (std::strcmp(id, kDescriptor.id) != 0) {
        return nullptr;
    }
    auto* p = new Plugin();
    p->host = host;
    p->plugin.desc = &kDescriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = pluginInit;
    p->plugin.destroy = pluginDestroy;
    p->plugin.activate = pluginActivate;
    p->plugin.deactivate = pluginDeactivate;
    p->plugin.start_processing = pluginStart;
    p->plugin.stop_processing = pluginStop;
    p->plugin.reset = pluginReset;
    p->plugin.process = pluginProcess;
    p->plugin.get_extension = pluginExtension;
    p->plugin.on_main_thread = pluginOnMainThread;
    return &p->plugin;
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool CLAP_ABI entryInit(const char*) {
#if defined(WABCUT_TEST_CRASH_ON_SCAN)
    volatile int* nowhere = nullptr;
    *nowhere = 1;
#elif defined(WABCUT_TEST_HANG_ON_SCAN)
    Sleep(600000);
#endif
    return true;
}
void CLAP_ABI entryDeinit() {}
const void* CLAP_ABI entryFactory(const char* id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr; }

} // namespace

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
}
