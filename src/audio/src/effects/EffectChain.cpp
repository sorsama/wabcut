#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/audio/dsp/Compressor.h>
#include <wabcut/audio/dsp/Denoiser.h>
#include <wabcut/audio/dsp/Equalizer.h>
#include <wabcut/audio/dsp/Limiter.h>
#include <wabcut/audio/dsp/Repair.h>
#include <wabcut/audio/dsp/Space.h>
#include <wabcut/audio/RealTime.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

namespace wabcut::audio {

namespace {

constexpr std::string_view kGain = "wabcut.audio.gain";
constexpr std::string_view kEq3 = "wabcut.audio.eq3";
constexpr std::string_view kCompressor = "wabcut.audio.compressor";
constexpr std::string_view kLimiter = "wabcut.audio.limiter";
constexpr std::string_view kDenoise = "wabcut.audio.denoise";
// Essential Sound (A.7).
constexpr std::string_view kFilter = "wabcut.audio.filter";
constexpr std::string_view kDeHum = "wabcut.audio.dehum";
constexpr std::string_view kDeEss = "wabcut.audio.deess";
constexpr std::string_view kExpander = "wabcut.audio.expander";
constexpr std::string_view kReverb = "wabcut.audio.reverb";
constexpr std::string_view kWidth = "wabcut.audio.width";

/// Index of a parameter in the descriptor's order (the descriptors are fixed data, so a miss is a programming error
/// caught by the tests; it reads the first parameter rather than out of bounds).
int indexOf(const EffectDescriptor& d, std::string_view key) noexcept {
    for (std::size_t i = 0; i < d.params.size(); ++i) {
        if (d.params[i].key == key) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

double dbToGain(double db) noexcept { return db <= -144.0 ? 0.0 : std::pow(10.0, db / 20.0); }

class GainProcessor final : public EffectProcessor {
public:
    GainProcessor(const EffectDescriptor& d, int channels) : m_channels(channels), m_gain(indexOf(d, "gain")) {}

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double g0 = dbToGain(cell.start[m_gain]);
        const double g1 = dbToGain(cell.end[m_gain]);
        if (g0 == 1.0 && g1 == 1.0) {
            return;
        }
        const auto ch = static_cast<std::size_t>(m_channels);
        const double step = (g1 - g0) / cell.length;
        for (int i = 0; i < frames; ++i) {
            const auto g = static_cast<float>(g0 + step * (cell.offset + i));
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] *= g;
            }
        }
    }
    void reset() noexcept override {}

private:
    int m_channels;
    int m_gain;
};

class Eq3Processor final : public EffectProcessor {
public:
    Eq3Processor(const EffectDescriptor& d, int channels, int sampleRate)
        : m_eq(channels, sampleRate), m_lowF(indexOf(d, "lowFrequency")), m_lowG(indexOf(d, "lowGain")),
          m_midF(indexOf(d, "midFrequency")), m_midG(indexOf(d, "midGain")), m_midQ(indexOf(d, "midQ")),
          m_highF(indexOf(d, "highFrequency")), m_highG(indexOf(d, "highGain")), m_sampleRate(sampleRate) {}

    int settleFrames() const noexcept override { return m_sampleRate / 20; } // 50 ms: well past the shelves' ringing

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        m_eq.setSettings({v[m_lowF], v[m_lowG], v[m_midF], v[m_midG], v[m_midQ], v[m_highF], v[m_highG]});
        m_eq.process(io, frames);
    }
    void reset() noexcept override { m_eq.reset(); }

private:
    dsp::Eq3 m_eq;
    int m_lowF, m_lowG, m_midF, m_midG, m_midQ, m_highF, m_highG;
    int m_sampleRate;
};

class CompressorProcessor final : public EffectProcessor {
public:
    CompressorProcessor(const EffectDescriptor& d, int channels, int sampleRate)
        : m_comp(channels, sampleRate), m_threshold(indexOf(d, "threshold")), m_ratio(indexOf(d, "ratio")),
          m_attack(indexOf(d, "attack")), m_release(indexOf(d, "release")), m_makeup(indexOf(d, "makeup")) {}

    int settleFrames() const noexcept override { return m_comp.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        dsp::CompressorSettings s;
        s.thresholdDb = v[m_threshold];
        s.ratio = v[m_ratio];
        s.attackMs = v[m_attack];
        s.releaseMs = v[m_release];
        s.makeupDb = v[m_makeup];
        m_comp.setSettings(s);
        m_comp.process(io, frames);
    }
    void reset() noexcept override { m_comp.reset(); }

private:
    dsp::Compressor m_comp;
    int m_threshold, m_ratio, m_attack, m_release, m_makeup;
};

class LimiterProcessor final : public EffectProcessor {
public:
    LimiterProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_ceiling(indexOf(d, "ceiling")), m_release(indexOf(d, "release")),
          m_limiter(channels, sampleRate, values[static_cast<std::size_t>(indexOf(d, "lookahead"))]) {}

    int latencyFrames() const noexcept override { return m_limiter.latencyFrames(); }
    int settleFrames() const noexcept override { return m_limiter.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        m_limiter.setSettings(cell.start[m_ceiling], cell.start[m_release]);
        m_limiter.process(io, frames);
    }
    void reset() noexcept override { m_limiter.reset(); }

private:
    int m_ceiling;
    int m_release;
    dsp::Limiter m_limiter;
};

class DenoiseProcessor final : public EffectProcessor {
public:
    DenoiseProcessor(const EffectDescriptor& d, int channels, int sampleRate)
        : m_amount(indexOf(d, "amount")), m_denoiser(channels, sampleRate) {}

    int latencyFrames() const noexcept override { return m_denoiser.latencyFrames(); }
    int settleFrames() const noexcept override { return m_denoiser.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        m_denoiser.setAmount(cell.start[m_amount]);
        m_denoiser.process(io, frames);
    }
    void reset() noexcept override { m_denoiser.reset(); }

private:
    int m_amount;
    dsp::Denoiser m_denoiser;
};

// ---- Essential Sound (A.7) ----------------------------------------------------------------------------------------

class FilterProcessor final : public EffectProcessor {
public:
    FilterProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_low(indexOf(d, "lowCut")), m_high(indexOf(d, "highCut")),
          m_filter(channels, sampleRate, values[static_cast<std::size_t>(indexOf(d, "slope"))]), m_sampleRate(sampleRate) {}

    int settleFrames() const noexcept override { return m_sampleRate / 10; } // 100 ms: a 20 Hz high-pass's ringing

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        m_filter.setFrequencies(cell.start[m_low], cell.start[m_high]);
        m_filter.process(io, frames);
    }
    void reset() noexcept override { m_filter.reset(); }

private:
    int m_low;
    int m_high;
    dsp::CutFilter m_filter;
    int m_sampleRate;
};

class DeHumProcessor final : public EffectProcessor {
public:
    DeHumProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_reduction(indexOf(d, "reduction")),
          m_dehum(channels, sampleRate, values[static_cast<std::size_t>(indexOf(d, "frequency"))],
                  static_cast<int>(std::lround(values[static_cast<std::size_t>(indexOf(d, "harmonics"))]))),
          m_sampleRate(sampleRate) {}

    int settleFrames() const noexcept override { return m_sampleRate / 4; } // narrow notches ring for ~0.1 s

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        m_dehum.setReduction(cell.start[m_reduction]);
        m_dehum.process(io, frames);
    }
    void reset() noexcept override { m_dehum.reset(); }

private:
    int m_reduction;
    dsp::DeHum m_dehum;
    int m_sampleRate;
};

class DeEssProcessor final : public EffectProcessor {
public:
    DeEssProcessor(const EffectDescriptor& d, int channels, int sampleRate)
        : m_threshold(indexOf(d, "threshold")), m_reduction(indexOf(d, "reduction")), m_frequency(indexOf(d, "frequency")),
          m_deess(channels, sampleRate) {}

    int settleFrames() const noexcept override { return m_deess.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        m_deess.setSettings({v[m_threshold], v[m_reduction], v[m_frequency]});
        m_deess.process(io, frames);
    }
    void reset() noexcept override { m_deess.reset(); }

private:
    int m_threshold, m_reduction, m_frequency;
    dsp::DeEsser m_deess;
};

class ExpanderProcessor final : public EffectProcessor {
public:
    ExpanderProcessor(const EffectDescriptor& d, int channels, int sampleRate)
        : m_threshold(indexOf(d, "threshold")), m_ratio(indexOf(d, "ratio")), m_range(indexOf(d, "range")),
          m_attack(indexOf(d, "attack")), m_release(indexOf(d, "release")), m_expander(channels, sampleRate) {}

    int settleFrames() const noexcept override { return m_expander.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        m_expander.setSettings({v[m_threshold], v[m_ratio], v[m_range], v[m_attack], v[m_release]});
        m_expander.process(io, frames);
    }
    void reset() noexcept override { m_expander.reset(); }

private:
    int m_threshold, m_ratio, m_range, m_attack, m_release;
    dsp::Expander m_expander;
};

class ReverbProcessor final : public EffectProcessor {
public:
    ReverbProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_decay(indexOf(d, "decay")), m_damping(indexOf(d, "damping")), m_mix(indexOf(d, "mix")), m_width(indexOf(d, "width")),
          m_reverb(channels, sampleRate, values[static_cast<std::size_t>(indexOf(d, "size"))],
                   values[static_cast<std::size_t>(indexOf(d, "predelay"))]) {}

    int settleFrames() const noexcept override { return m_reverb.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        m_reverb.setSettings({v[m_decay], v[m_damping], v[m_mix], v[m_width]});
        m_reverb.process(io, frames);
    }
    void reset() noexcept override { m_reverb.reset(); }

private:
    int m_decay, m_damping, m_mix, m_width;
    dsp::Reverb m_reverb;
};

class WidthProcessor final : public EffectProcessor {
public:
    WidthProcessor(const EffectDescriptor& d, int channels, int sampleRate)
        : m_width(indexOf(d, "width")), m_bass(indexOf(d, "bassMono")), m_stereo(channels, sampleRate) {}

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        m_stereo.setSettings(cell.start[m_width], cell.start[m_bass]);
        m_stereo.process(io, frames);
    }
    void reset() noexcept override { m_stereo.reset(); }

private:
    int m_width;
    int m_bass;
    dsp::StereoWidth m_stereo;
};

std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

} // namespace

namespace {

bool isBuiltinAudioEffect(std::string_view id) {
    return id == kGain || id == kEq3 || id == kCompressor || id == kLimiter || id == kDenoise || id == kFilter || id == kDeHum ||
           id == kDeEss || id == kExpander || id == kReverb || id == kWidth;
}

struct FactorySlot {
    rt::CheckedMutex mutex;
    std::shared_ptr<ExternalEffectFactory> factory;
};

FactorySlot& factorySlot() {
    static FactorySlot slot;
    return slot;
}

} // namespace

void setExternalEffectFactory(std::shared_ptr<ExternalEffectFactory> factory) {
    FactorySlot& slot = factorySlot();
    std::lock_guard lock(slot.mutex);
    slot.factory = std::move(factory);
}

std::shared_ptr<ExternalEffectFactory> externalEffectFactory() {
    FactorySlot& slot = factorySlot();
    std::lock_guard lock(slot.mutex);
    return slot.factory;
}

bool isImplementedAudioEffect(std::string_view id) {
    if (isBuiltinAudioEffect(id)) {
        return true;
    }
    if (!id.starts_with(kPluginEffectPrefix)) {
        return false;
    }
    const auto factory = externalEffectFactory();
    return factory && factory->implements(id);
}

std::string externalEffectProblem(const EffectInstance& instance) {
    if (!instance.enabled || !instance.effect.starts_with(kPluginEffectPrefix)) {
        return {};
    }
    if (const auto factory = externalEffectFactory()) {
        return factory->problem(instance);
    }
    return "audio plugins are not available in this build (" + instance.effect + ")";
}

// ---- Offline renders ----------------------------------------------------------------------------------------

void RenderIncidents::add(std::string message) {
    std::lock_guard lock(m_mutex);
    if (std::find(m_messages.begin(), m_messages.end(), message) == m_messages.end()) {
        m_messages.push_back(std::move(message));
    }
}

std::vector<std::string> RenderIncidents::messages() const {
    std::lock_guard lock(m_mutex);
    return m_messages;
}

namespace {
thread_local int t_offlineDepth = 0;
thread_local std::shared_ptr<RenderIncidents> t_incidents;
} // namespace

ScopedOfflineRender::ScopedOfflineRender(std::shared_ptr<RenderIncidents> incidents) noexcept : m_previous(t_incidents) {
    ++t_offlineDepth;
    if (incidents) {
        t_incidents = std::move(incidents);
    }
}

ScopedOfflineRender::~ScopedOfflineRender() {
    --t_offlineDepth;
    t_incidents = std::move(m_previous);
}

bool inOfflineRender() noexcept { return t_offlineDepth > 0; }

std::shared_ptr<RenderIncidents> offlineRenderIncidents() noexcept { return t_incidents; }

std::unique_ptr<EffectProcessor> createEffectProcessor(const EffectDescriptor& d, int channels, int sampleRate,
                                                       std::span<const double> values) {
    channels = std::clamp(channels, 1, dsp::kMaxDspChannels);
    if (d.params.size() > static_cast<std::size_t>(kMaxEffectParams) || values.size() < d.params.size()) {
        return nullptr;
    }
    if (d.id == kGain) {
        return std::make_unique<GainProcessor>(d, channels);
    }
    if (d.id == kEq3) {
        return std::make_unique<Eq3Processor>(d, channels, sampleRate);
    }
    if (d.id == kCompressor) {
        return std::make_unique<CompressorProcessor>(d, channels, sampleRate);
    }
    if (d.id == kLimiter) {
        return std::make_unique<LimiterProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kDenoise) {
        return std::make_unique<DenoiseProcessor>(d, channels, sampleRate);
    }
    if (d.id == kFilter) {
        return std::make_unique<FilterProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kDeHum) {
        return std::make_unique<DeHumProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kDeEss) {
        return std::make_unique<DeEssProcessor>(d, channels, sampleRate);
    }
    if (d.id == kExpander) {
        return std::make_unique<ExpanderProcessor>(d, channels, sampleRate);
    }
    if (d.id == kReverb) {
        return std::make_unique<ReverbProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kWidth) {
        return std::make_unique<WidthProcessor>(d, channels, sampleRate);
    }
    return nullptr;
}

// ---- EffectChain --------------------------------------------------------------------------------------------

struct EffectChain::Stage {
    EffectInstanceId id;
    std::string effect;
    const EffectDescriptor* descriptor = nullptr;
    bool external = false;               ///< a plugin (the external factory's processor)
    std::vector<double> constants;       ///< built-in: the non-animatable values the processor was built for
    std::uint64_t dataHash = 0;          ///< external: the instance data (a plugin's saved state) it was built for
    std::shared_ptr<EffectProcessor> processor;
    std::vector<std::string> keys;       ///< external: the instance's parameter keys, in cell order
    std::shared_ptr<const void> binding; ///< external: what processor->bindKeys(keys) returned (immutable)
    std::vector<const ParamValue*> values; ///< into the bound stack; null = the default
    std::vector<double> defaults;
    bool animated = false;               ///< some bound value is a curve
    std::int64_t cell = INT64_MIN;       ///< cell the cached values belong to
    std::vector<double> start;
    std::vector<double> end;

    void evaluate(std::vector<double>& out, std::int64_t frame, const CurveClock& clock) const noexcept {
        const Ticks t = animated ? clock.curveTimeAt(frame) : Ticks{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            const ParamValue* v = values[i];
            out[i] = v != nullptr ? v->valueAt(t) : defaults[i];
        }
    }
};

EffectChain::EffectChain(int channels, int sampleRate, ChainContext context)
    : m_channels(std::clamp(channels, 1, dsp::kMaxDspChannels)), m_sampleRate(std::max(sampleRate, 1)), m_context(context) {}
EffectChain::~EffectChain() = default;
EffectChain::EffectChain(EffectChain&&) noexcept = default;
EffectChain& EffectChain::operator=(EffectChain&&) noexcept = default;

namespace {

/// The descriptor of an instance the chain runs, or null for one it bypasses; `external` says who implements it.
const EffectDescriptor* runnable(const EffectInstance& instance, bool& external) {
    external = false;
    if (!instance.enabled || !isEvaluable(instance, EffectKind::Audio)) {
        return nullptr;
    }
    const EffectDescriptor* d = EffectRegistry::builtin().find(instance.effect);
    if (d == nullptr) {
        return nullptr;
    }
    if (isBuiltinAudioEffect(instance.effect)) {
        return d->params.size() <= static_cast<std::size_t>(kMaxEffectParams) ? d : nullptr;
    }
    const auto factory = externalEffectFactory();
    if (factory && factory->implements(instance.effect)) {
        external = true;
        return d;
    }
    return nullptr;
}

/// What an external stage's processor was built from: the instance's data. Texts over 256 bytes (a plugin's base64
/// state parts) count by their key and length only, so the feeder's rebind stays cheap; plugin effects keep a short
/// checksum entry of their state ("plugin.state.sum"), which changes whenever the state does.
std::uint64_t hashData(const EffectInstance& instance) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto bytes = [&h](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 0x100000001b3ULL;
        }
    };
    for (const EffectData& d : instance.data) {
        bytes(d.key.data(), d.key.size());
        if (const std::string* text = d.text()) {
            const std::size_t size = text->size();
            if (size <= 256) {
                bytes(text->data(), size);
            } else {
                bytes(&size, sizeof size);
            }
        } else if (const auto* points = d.points()) {
            for (const EffectPoint& pt : *points) {
                bytes(&pt.x, sizeof pt.x);
                bytes(&pt.y, sizeof pt.y);
            }
        }
        h = (h ^ 0xffu) * 0x100000001b3ULL;
    }
    return h;
}

double constantOf(const EffectInstance& instance, const EffectParamInfo& info) {
    const ParamValue* v = instance.find(info.key);
    return v != nullptr ? v->constant() : info.defaultValue;
}

bool sameBuild(const EffectChain::Stage& s, const EffectInstance& instance, const EffectDescriptor& d, bool external) {
    if (s.id != instance.id || s.effect != instance.effect || s.external != external) {
        return false;
    }
    if (external) {
        return s.dataHash == hashData(instance);
    }
    for (std::size_t i = 0; i < d.params.size(); ++i) {
        const double c = d.params[i].animatable ? 0.0 : constantOf(instance, d.params[i]);
        if (i >= s.constants.size() || s.constants[i] != c) {
            return false;
        }
    }
    return true;
}

/// Whether an external stage binds exactly `instance`'s parameter keys.
bool sameKeys(const EffectChain::Stage& s, const EffectInstance& instance) {
    if (s.keys.size() != instance.params.size()) {
        return false;
    }
    for (std::size_t i = 0; i < s.keys.size(); ++i) {
        if (s.keys[i] != instance.params[i].key) {
            return false;
        }
    }
    return true;
}

/// Points `s`'s values at `instance` (built-in: the descriptor's parameters; external: the instance's own).
void bindValues(EffectChain::Stage& s, const EffectInstance& instance, const EffectDescriptor& d) {
    s.animated = false;
    if (s.external) {
        for (std::size_t i = 0; i < instance.params.size(); ++i) {
            s.values[i] = &instance.params[i].value;
            s.animated = s.animated || instance.params[i].value.isCurve();
        }
    } else {
        for (std::size_t i = 0; i < d.params.size(); ++i) {
            const ParamValue* v = instance.find(d.params[i].key);
            s.values[i] = v;
            s.animated = s.animated || (v != nullptr && v->isCurve());
        }
    }
    s.cell = INT64_MIN; // values may have changed: evaluate again
}

} // namespace

bool EffectChain::bind(const EffectStack& stack, const EffectChain* adoptFrom) {
    // Fast path (the feeder rebinds every chunk): same processors in the same order, so only the parameter values
    // are rebound, without allocating.
    if (adoptFrom == nullptr) {
        std::size_t j = 0;
        bool same = true;
        for (const EffectInstance& instance : stack) {
            bool external = false;
            const EffectDescriptor* d = runnable(instance, external);
            if (d == nullptr) {
                continue;
            }
            if (j < m_stages.size() && external && m_stages[j].id == instance.id && m_stages[j].effect == instance.effect &&
                m_stages[j].external && m_stages[j].dataHash != hashData(instance) && m_stages[j].processor->acceptsData(instance)) {
                m_stages[j].dataHash = hashData(instance); // the plugin already has this state: keep it running
            }
            if (j >= m_stages.size() || !sameBuild(m_stages[j], instance, *d, external) ||
                (external && (!sameKeys(m_stages[j], instance) || m_stages[j].processor->stale()))) {
                same = false;
                break;
            }
            ++j;
        }
        if (same && j == m_stages.size()) {
            j = 0;
            for (const EffectInstance& instance : stack) {
                bool external = false;
                const EffectDescriptor* d = runnable(instance, external);
                if (d != nullptr) {
                    bindValues(m_stages[j++], instance, *d);
                }
            }
            return false;
        }
    }
    const std::vector<Stage>& source = adoptFrom != nullptr ? adoptFrom->m_stages : m_stages;
    const auto factory = externalEffectFactory();
    std::vector<Stage> next;
    next.reserve(stack.size());
    for (const EffectInstance& instance : stack) {
        bool external = false;
        const EffectDescriptor* d = runnable(instance, external);
        if (d == nullptr) {
            continue;
        }
        Stage stage;
        stage.id = instance.id;
        stage.effect = instance.effect;
        stage.descriptor = d;
        stage.external = external;
        std::vector<double> initial;
        if (external) {
            stage.dataHash = hashData(instance);
            for (const EffectParam& p : instance.params) {
                stage.keys.push_back(p.key);
                const EffectParamInfo* info = d->param(p.key);
                stage.defaults.push_back(info != nullptr ? info->defaultValue : 0.0);
            }
        } else {
            for (const EffectParamInfo& info : d->params) {
                stage.defaults.push_back(info.defaultValue);
                initial.push_back(constantOf(instance, info));
                stage.constants.push_back(info.animatable ? 0.0 : initial.back());
            }
        }
        const std::size_t count = stage.defaults.size();
        stage.values.assign(count, nullptr);
        stage.start.assign(count, 0.0);
        stage.end.assign(count, 0.0);
        bindValues(stage, instance, *d);
        auto same = std::find_if(source.begin(), source.end(),
                                 [&](const Stage& s) { return sameBuild(s, instance, *d, external); });
        if (same == source.end() && external) {
            // A plugin whose state was just saved from its own instance (its editor) keeps playing, as on the fast path.
            same = std::find_if(source.begin(), source.end(), [&](const Stage& s) {
                return s.external && s.id == instance.id && s.effect == instance.effect && s.processor->acceptsData(instance);
            });
        }
        // A stale external processor is rebuilt; the factory gets it so the new one can take over its plugin.
        const bool reuse = same != source.end() && !(external && same->processor->stale());
        if (reuse) {
            stage.processor = same->processor;
        } else if (external) {
            EffectProcessor* replacing = same != source.end() ? same->processor.get() : nullptr;
            stage.processor = factory ? std::shared_ptr<EffectProcessor>(
                                            factory->create(instance, *d, m_channels, m_sampleRate, m_context, replacing))
                                      : nullptr;
        } else {
            stage.processor = createEffectProcessor(*d, m_channels, m_sampleRate, std::span<const double>(initial));
        }
        if (!stage.processor) {
            continue;
        }
        if (external) {
            // Never changes the processor itself: `source` may be a chain the audio thread is playing.
            stage.binding = reuse && sameKeys(*same, instance) ? same->binding : stage.processor->bindKeys(stage.keys);
        }
        next.push_back(std::move(stage));
    }
    bool changed = next.size() != m_stages.size();
    for (std::size_t i = 0; !changed && i < next.size(); ++i) {
        changed = next[i].processor != m_stages[i].processor;
    }
    m_stages = std::move(next);
    m_settle = 0;
    for (const Stage& s : m_stages) {
        m_settle = std::max(m_settle, s.processor->settleFrames());
    }
    return changed;
}

int EffectChain::latencyFrames() const noexcept {
    int total = 0;
    for (const Stage& s : m_stages) {
        total += s.processor->latencyFrames();
    }
    return total;
}

void EffectChain::process(float* interleaved, int frames, std::int64_t firstFrame, const CurveClock& clock) noexcept {
    if (m_stages.empty() || frames <= 0) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    int done = 0;
    while (done < frames) {
        const std::int64_t frame = firstFrame + done;
        const std::int64_t cell = floorDiv(frame, kParamCellFrames);
        const auto offset = static_cast<int>(frame - cell * kParamCellFrames);
        const int n = std::min(frames - done, kParamCellFrames - offset);
        float* block = interleaved + static_cast<std::size_t>(done) * ch;
        for (Stage& s : m_stages) {
            if (s.cell != cell) {
                if (s.cell == cell - 1) {
                    s.start.swap(s.end);
                } else {
                    s.evaluate(s.start, cell * kParamCellFrames, clock);
                }
                s.evaluate(s.end, (cell + 1) * kParamCellFrames, clock);
                s.cell = cell;
            }
            const ParamCell values{s.start.data(), s.end.data(), offset, kParamCellFrames,
                                   static_cast<int>(s.start.size()), s.binding.get()};
            s.processor->process(block, n, values);
        }
        done += n;
    }
}

void EffectChain::reset() noexcept {
    for (Stage& s : m_stages) {
        s.processor->reset();
        s.cell = INT64_MIN;
    }
}

} // namespace wabcut::audio
