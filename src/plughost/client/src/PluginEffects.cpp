#include <wabcut/plughost/PluginEffects.h>

#include <wabcut/audio/RealTime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace wabcut::plughost {

using audio::rt::CheckedMutex;

namespace {

constexpr std::string_view kStateSumKey = "plugin.state.sum";
constexpr std::size_t kStatePartChars = kMaxEffectText;
constexpr std::uint32_t kNoParam = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint64_t kNoBlock = std::numeric_limits<std::uint64_t>::max();
/// The fade from the dry signal back to the plugin's (after a late block, or once a loading plugin is ready).
constexpr float kFadeStep = 1.0f / static_cast<float>(kBlockFrames);

std::uint64_t fnv(std::string_view text) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
    }
    return h;
}

bool isStatePart(std::string_view key) {
    return key.starts_with(kStateKeyPrefix) && key != kStateSumKey;
}

/// The plugin parameter id of an effect parameter key ("p<id>"), or kNoParam.
std::uint32_t paramIdOf(const std::string& key) {
    if (key.size() > 1 && key[0] == 'p') {
        char* end = nullptr;
        const unsigned long v = std::strtoul(key.c_str() + 1, &end, 10);
        if (end != nullptr && *end == '\0') {
            return static_cast<std::uint32_t>(v);
        }
    }
    return kNoParam;
}

/// The plugin parameter ids an effect instance sets ("p<id>" keys), sorted.
std::vector<std::uint32_t> paramIdsOf(const EffectInstance& instance) {
    std::vector<std::uint32_t> out;
    for (const EffectParam& p : instance.params) {
        if (const std::uint32_t id = paramIdOf(p.key); id != kNoParam) {
            out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string displayName(const PluginInfo& plugin) {
    return plugin.vendor.empty() ? plugin.name : plugin.name + " (" + plugin.vendor + ")";
}

// ---- The processor ----------------------------------------------------------------------------------------------

/// One chain stage's parameter keys as plugin parameter ids (immutable once built), and the value last sent for each:
/// written only by the processor while it plays that stage (chains that share a processor never process at once).
struct KeyTable {
    std::vector<std::uint32_t> params;
    mutable std::vector<double> lastSent;
};

/// The plugin instance a processor plays, once there is one: at once (an idle one from the pool), or when the
/// factory's worker has loaded it. Ready and Failed are published with release; `instance` and `problem` are written
/// before and never after.
struct PendingInstance {
    enum : std::uint8_t { Waiting, Ready, Failed, Abandoned };
    std::atomic<std::uint8_t> state{Waiting};
    std::shared_ptr<PluginInstance> instance;
    std::string problem;
    CheckedMutex mutex; ///< for waits off the audio thread
    std::condition_variable_any changed;

    std::uint8_t current() const noexcept { return state.load(std::memory_order_acquire); }
    PluginInstance* ready() const noexcept { return current() == Ready ? instance.get() : nullptr; }

    /// Hands `made` (already counted as a user) to the processor; false when the processor went away meanwhile.
    bool publish(const std::shared_ptr<PluginInstance>& made) {
        instance = made;
        std::uint8_t expected = Waiting;
        const bool taken = state.compare_exchange_strong(expected, Ready, std::memory_order_acq_rel);
        wake();
        return taken;
    }
    void fail(std::string why) {
        problem = std::move(why);
        std::uint8_t expected = Waiting;
        state.compare_exchange_strong(expected, Failed, std::memory_order_acq_rel);
        wake();
    }
    /// Off the audio thread: waits until the plugin is ready or failed, at most `timeout`.
    void wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, timeout, [&] { return current() != Waiting; });
    }

private:
    void wake() {
        {
            std::lock_guard lock(mutex);
        }
        changed.notify_all();
    }
};

class PluginProcessor final : public audio::EffectProcessor {
public:
    struct Setup {
        std::weak_ptr<PluginEffects> owner;
        EffectInstanceId effect;
        std::string name; ///< for an offline render's incidents
        std::shared_ptr<PendingInstance> pending;
        int channels = 2; ///< the chain's
        int sampleRate = 48000;
        int latency = 0;  ///< the plugin's (while it loads: the one it is expected to have)
        bool live = false;
        std::chrono::milliseconds blockWait{3000};  ///< ahead of playback: how long a block may take
        std::chrono::milliseconds readyWait{20000}; ///< off the audio thread: how long the plugin may take to load
        std::shared_ptr<audio::RenderIncidents> incidents;
        std::vector<std::uint32_t> touched; ///< from the processor it replaces
    };

    explicit PluginProcessor(Setup setup)
        : m_owner(std::move(setup.owner)), m_effect(setup.effect), m_name(std::move(setup.name)),
          m_pending(std::move(setup.pending)), m_channels(std::max(setup.channels, 1)),
          m_pipeline(setup.live ? kLivePipelineFrames : kAheadPipelineFrames),
          m_maxLatency(static_cast<int>(std::clamp<std::int64_t>(maxLatencyFrames(setup.sampleRate), 1, 1 << 24))),
          m_live(setup.live), m_blockWait(setup.blockWait), m_readyWait(setup.readyWait), m_incidents(std::move(setup.incidents)),
          m_touched(std::move(setup.touched)) {
        m_latency = std::clamp(setup.latency, 0, m_maxLatency);
        m_reported.store(m_latency, std::memory_order_relaxed);
        // The dry signal of every channel, the whole latency deep (a bypassed plugin, a late block, the channels the
        // plugin does not take); on the master as deep as any latency its plugin may come to report.
        m_dryFrames = static_cast<std::size_t>(m_pipeline) + static_cast<std::size_t>(m_live ? m_maxLatency : m_latency) + 1;
        m_dry.assign(m_dryFrames * static_cast<std::size_t>(m_channels), 0.0f);
    }

    ~PluginProcessor() override {
        std::uint8_t state = PendingInstance::Waiting;
        if (m_pending->state.compare_exchange_strong(state, PendingInstance::Abandoned, std::memory_order_acq_rel)) {
            return; // still loading: the worker gives the instance to the pool
        }
        report(state);
        if (state != PendingInstance::Ready) {
            return;
        }
        std::shared_ptr<PluginInstance> instance = m_pending->instance;
        if (instance->dropUser()) { // the last processor playing it (a rebuilt one may have taken it over)
            if (auto owner = m_owner.lock()) {
                owner->release(m_effect, std::move(instance), touched());
            }
        }
    }

    EffectInstanceId effect() const noexcept { return m_effect; }
    std::shared_ptr<PluginInstance> playing() const { return m_pending->ready() != nullptr ? m_pending->instance : nullptr; }
    std::vector<std::uint32_t> touched() const {
        std::lock_guard lock(m_touchedMutex);
        return m_touched;
    }

    int latencyFrames() const noexcept override {
        if (m_live) {
            follow();
        }
        return m_pipeline + m_reported.load(std::memory_order_relaxed);
    }

    bool stale() const noexcept override {
        if (m_live) {
            return false; // follows its plugin's latency itself (latencyFrames)
        }
        const PluginInstance* instance = m_pending->ready();
        return instance != nullptr && instance->running() && std::clamp(instance->latency(), 0, m_maxLatency) != m_latency;
    }

    std::shared_ptr<const void> bindKeys(std::span<const std::string> keys) const override {
        auto table = std::make_shared<KeyTable>();
        table->params.reserve(keys.size());
        for (const std::string& k : keys) {
            table->params.push_back(paramIdOf(k));
        }
        table->lastSent.assign(keys.size(), std::numeric_limits<double>::quiet_NaN());
        std::lock_guard lock(m_touchedMutex);
        for (const std::uint32_t id : table->params) {
            if (id != kNoParam) {
                m_touched.push_back(id);
            }
        }
        std::sort(m_touched.begin(), m_touched.end());
        m_touched.erase(std::unique(m_touched.begin(), m_touched.end()), m_touched.end());
        return table;
    }

    bool acceptsData(const EffectInstance& instance) override {
        const PluginInstance* current = m_pending->ready();
        return current != nullptr && current->running() && current->hasKnownState(fnv(pluginState(instance)));
    }

    void process(float* io, int frames, const audio::ParamCell& cell) noexcept override {
        if (cell.offset == 0 || m_firstCall) {
            queueParameters(cell);
            m_firstCall = false;
        }
        const auto ch = static_cast<std::size_t>(m_channels);
        const auto dryFrames = static_cast<std::int64_t>(m_dryFrames);
        for (int k = 0; k < frames; ++k) {
            if (m_fill == 0 && m_instance == nullptr && m_pending->current() == PendingInstance::Ready) {
                attach(m_pos / kBlockFrames); // a plugin that was loading is ready: it starts with this block
                if (m_instance != nullptr) {
                    queueParameters(cell); // every bound value, at once
                }
            }
            float* frame = io + static_cast<std::size_t>(k) * ch;
            PluginInstance* instance = m_instance;
            const bool alive = instance != nullptr && instance->running();
            // In: the plugin takes L/R (a mono chain's one channel twice); the dry ring keeps every channel.
            m_input[0][static_cast<std::size_t>(m_fill)] = frame[0];
            m_input[1][static_cast<std::size_t>(m_fill)] = ch > 1 ? frame[1] : frame[0];
            std::copy_n(frame, ch, m_dry.data() + static_cast<std::size_t>(m_pos % dryFrames) * ch);
            if (++m_fill == kBlockFrames) {
                submit(alive);
            }
            // Out: the plugin's L/R, the other channels from the dry ring, all the whole latency late. A block that is
            // not there (late, bypassed, still loading) plays the dry signal instead.
            const std::int64_t back = m_pos - m_pipeline - m_latency;
            const float* dry = back >= 0 ? m_dry.data() + static_cast<std::size_t>(back % dryFrames) * ch : nullptr;
            const std::int64_t at = m_pos - m_pipeline;
            float wetL = 0.0f;
            float wetR = 0.0f;
            bool wet = false;
            if (alive && at >= m_wetFrom) {
                const std::uint64_t block = m_blockBase + static_cast<std::uint64_t>(at / kBlockFrames);
                if (completed(*instance, block)) {
                    const BlockSlot& slot = instance->shared().ring[block % kSlots];
                    const auto o = static_cast<std::size_t>(at % kBlockFrames);
                    wetL = slot.output[0][o];
                    wetR = slot.output[1][o];
                    wet = usable(*instance, block, wetL, wetR);
                }
            }
            if (wet) {
                if (m_wet < 1.0f) { // fading in from the dry signal
                    m_wet = std::min(1.0f, m_wet + kFadeStep);
                    const float dryL = dry != nullptr ? dry[0] : 0.0f;
                    const float dryR = dry != nullptr ? dry[ch > 1 ? 1 : 0] : 0.0f;
                    wetL = m_wet * wetL + (1.0f - m_wet) * dryL;
                    wetR = m_wet * wetR + (1.0f - m_wet) * dryR;
                }
                if (ch > 1) {
                    frame[0] = wetL;
                    frame[1] = wetR;
                    for (std::size_t c = 2; c < ch; ++c) {
                        frame[c] = dry != nullptr ? dry[c] : 0.0f;
                    }
                } else {
                    frame[0] = 0.5f * (wetL + wetR);
                }
            } else {
                if (at >= 0) {
                    m_wet = 0.0f; // the plugin's output fades back in when it is there again
                }
                for (std::size_t c = 0; c < ch; ++c) {
                    frame[c] = dry != nullptr ? dry[c] : 0.0f;
                }
            }
            ++m_pos;
        }
    }

    void reset() noexcept override {
        m_fill = 0;
        m_pos = 0;
        m_eventCount = 0;
        m_firstCall = true;
        m_resendAll = true;
        m_wet = 1.0f;
        m_wetFrom = 0;
        m_missedBlock = kNoBlock;
        std::fill(m_dry.begin(), m_dry.end(), 0.0f);
        if (m_live) {
            m_latency = m_reported.load(std::memory_order_relaxed); // what the master compensates from this restart on
        }
        attach(0); // the new stream's block 0 (an instance that already has blocks numbers on from there)
    }

private:
    bool realtime() const noexcept { return m_live && audio::rt::inRealtimeSection(); }

    /// The master's processor reports its plugin's latency as it is now (once loaded; after a restart the plugin asked
    /// for), and adopts it at its next reset(). Off the audio thread (an offline render of the master) it first waits
    /// for a loading plugin, so the master is primed with the latency it will have.
    void follow() const noexcept {
        if (!realtime() && m_pending->current() == PendingInstance::Waiting) {
            m_pending->wait(m_readyWait);
        }
        const PluginInstance* instance = m_pending->ready();
        if (instance != nullptr && instance->running()) {
            m_reported.store(std::clamp(instance->latency(), 0, m_maxLatency), std::memory_order_relaxed);
        }
    }

    /// Starts playing the plugin at stream block `s0` (a block boundary) when it is loaded and runs at the latency this
    /// processor compensates (otherwise a clip or track processor is stale and gets rebuilt, a master one adopts the
    /// latency at its next reset). Mid-stream its output takes over once its own latency has passed, fading in.
    void attach(std::int64_t s0) noexcept {
        PluginInstance* instance = m_pending->ready();
        if (instance == nullptr || std::clamp(instance->latency(), 0, m_maxLatency) != m_latency) {
            m_instance = nullptr;
            return;
        }
        m_instance = instance;
        // The instance's ring numbers on from what it was given before (by this processor or another).
        const std::uint64_t submitted = instance->shared().submitted.load(std::memory_order_acquire);
        m_nextBlock = submitted;
        m_blockBase = submitted - static_cast<std::uint64_t>(s0);
        m_wetFrom = s0 == 0 ? 0 : s0 * kBlockFrames + m_latency;
        if (s0 != 0) {
            m_wet = 0.0f;
        }
        m_resetNext = true;
        m_resendAll = true;
        m_eventCount = 0;
        m_missedBlock = kNoBlock;
    }

    void queueParameters(const audio::ParamCell& cell) noexcept {
        const auto* table = static_cast<const KeyTable*>(cell.binding);
        if (table != m_table) {
            m_table = table; // another chain's binding (a new master program): send everything once
            m_resendAll = true;
        }
        if (table == nullptr) {
            return;
        }
        if (m_resendAll) {
            std::fill(table->lastSent.begin(), table->lastSent.end(), std::numeric_limits<double>::quiet_NaN());
            m_resendAll = false;
        }
        double* lastSent = table->lastSent.data();
        const std::size_t count = std::min(table->params.size(), static_cast<std::size_t>(std::max(cell.count, 0)));
        for (std::size_t i = 0; i < count; ++i) {
            const double v = cell.start[i];
            if (table->params[i] == kNoParam || v == lastSent[i]) {
                continue;
            }
            lastSent[i] = v;
            if (m_eventCount >= static_cast<std::size_t>(kMaxBlockEvents)) {
                if (m_instance != nullptr) {
                    m_instance->shared().droppedEvents.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            m_events[m_eventCount++] = ParamEvent{table->params[i], static_cast<std::uint32_t>(m_fill), v};
        }
    }

    /// Whether block `block` is done. On the audio callback it never waits: a block that is not done at its first frame
    /// plays dry throughout (counted once). Ahead of playback it waits, at most m_blockWait, then the instance is hung.
    bool completed(PluginInstance& instance, std::uint64_t block) noexcept {
        InstanceShared& shared = instance.shared();
        if (realtime()) {
            if (m_missedBlock == block) {
                return false;
            }
            if (shared.completed.load(std::memory_order_acquire) > block) {
                return true;
            }
            m_missedBlock = block;
            instance.countMissed();
            ++m_missedHere;
            return false;
        }
        if (shared.completed.load(std::memory_order_acquire) > block) {
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + m_blockWait;
        while (shared.completed.load(std::memory_order_acquire) <= block) {
            if (!instance.running()) {
                return false;
            }
            if (!instance.waitDone(std::chrono::milliseconds(10)) && std::chrono::steady_clock::now() > deadline) {
                instance.markHung();
                return false;
            }
        }
        return true;
    }

    /// Whether the plugin's samples may play: NaN or infinity may not (the dry signal plays; counted once per block,
    /// and the instance fails after kMaxInvalidBlocks such blocks); anything else is clamped to +-kMaxOutputLevel.
    bool usable(PluginInstance& instance, std::uint64_t block, float& left, float& right) noexcept {
        const bool finite = std::isfinite(left) && std::isfinite(right);
        if (finite && std::fabs(left) <= kMaxOutputLevel && std::fabs(right) <= kMaxOutputLevel) {
            return true;
        }
        if (block != m_invalidBlock) {
            m_invalidBlock = block;
            instance.countInvalid();
            ++m_invalidHere;
        }
        if (!finite) {
            if (block != m_nonFiniteBlock) {
                m_nonFiniteBlock = block;
                if (++m_nonFiniteBlocks >= kMaxInvalidBlocks) {
                    instance.markInvalid();
                }
            }
            return false;
        }
        left = std::clamp(left, -kMaxOutputLevel, kMaxOutputLevel);
        right = std::clamp(right, -kMaxOutputLevel, kMaxOutputLevel);
        return true;
    }

    void submit(bool alive) noexcept {
        const std::uint64_t block = m_nextBlock++;
        m_fill = 0;
        if (!alive) {
            m_eventCount = 0;
            return;
        }
        PluginInstance& instance = *m_instance;
        InstanceShared& shared = instance.shared();
        // The slot must be free: the helper done with the block that used it last. Ahead of playback, wait for that.
        // On the audio callback, never: a helper that far behind has missed those blocks (they play dry), and reusing
        // the slot can only spoil audio that is not played.
        if (!realtime()) {
            const auto deadline = std::chrono::steady_clock::now() + m_blockWait;
            while (shared.completed.load(std::memory_order_acquire) + static_cast<std::uint64_t>(kSlots) <= block) {
                if (!instance.running() ||
                    (!instance.waitDone(std::chrono::milliseconds(10)) && std::chrono::steady_clock::now() > deadline)) {
                    instance.markHung();
                    m_eventCount = 0;
                    return;
                }
            }
        }
        BlockSlot& slot = shared.ring[block % kSlots];
        slot.block = block;
        slot.frames = static_cast<std::uint32_t>(kBlockFrames);
        slot.flags = m_resetNext ? kSlotReset : 0u;
        m_resetNext = false;
        slot.eventCount = static_cast<std::uint32_t>(m_eventCount);
        std::copy_n(m_events.begin(), m_eventCount, slot.events);
        m_eventCount = 0;
        std::memcpy(slot.input[0], m_input[0].data(), sizeof(float) * kBlockFrames);
        std::memcpy(slot.input[1], m_input[1].data(), sizeof(float) * kBlockFrames);
        shared.submitted.store(block + 1, std::memory_order_release);
        instance.signalWork();
    }

    /// An offline render's incidents, when the processor goes away (control side).
    void report(std::uint8_t state) const {
        if (!m_incidents) {
            return;
        }
        if (state == PendingInstance::Failed) {
            m_incidents->add(m_name + " could not be loaded (" + m_pending->problem + "): its processing is missing");
            return;
        }
        if (state != PendingInstance::Ready) {
            return;
        }
        const PluginInstance& instance = *m_pending->instance;
        if (!instance.running()) {
            m_incidents->add(m_name + ": " + instance.problem() + "; its processing is missing from then on");
        }
        if (m_invalidHere > 0) {
            m_incidents->add(m_name + " produced invalid samples in " + std::to_string(m_invalidHere) +
                             " blocks (replaced by the dry signal, or clamped)");
        }
        if (m_missedHere > 0) {
            m_incidents->add(m_name + " was late for " + std::to_string(m_missedHere) + " blocks (played without it)");
        }
    }

    std::weak_ptr<PluginEffects> m_owner;
    EffectInstanceId m_effect;
    std::string m_name;
    std::shared_ptr<PendingInstance> m_pending;
    const int m_channels;
    const int m_pipeline;
    const int m_maxLatency;
    const bool m_live;
    const std::chrono::milliseconds m_blockWait;
    const std::chrono::milliseconds m_readyWait;
    std::shared_ptr<audio::RenderIncidents> m_incidents;
    mutable CheckedMutex m_touchedMutex;
    mutable std::vector<std::uint32_t> m_touched; ///< parameters any of its bindings may have sent (sorted)

    int m_latency = 0;                      ///< the plugin latency the output is aligned to
    mutable std::atomic<int> m_reported{0}; ///< what latencyFrames() reports (a master's: its plugin's latency now)
    PluginInstance* m_instance = nullptr;   ///< the instance played (processing thread), once attached
    const KeyTable* m_table = nullptr;      ///< the binding last played (compared, never read once its chain is gone)
    bool m_resendAll = true;
    std::array<ParamEvent, kMaxBlockEvents> m_events{};
    std::size_t m_eventCount = 0;
    std::array<std::array<float, kBlockFrames>, 2> m_input{};
    int m_fill = 0;           ///< frames of the current block
    std::int64_t m_pos = 0;   ///< stream frame (since the last reset)
    std::uint64_t m_nextBlock = 0;
    std::uint64_t m_blockBase = 0; ///< block number of the stream's block 0 (modulo 2^64)
    std::uint64_t m_missedBlock = kNoBlock;
    std::uint64_t m_invalidBlock = kNoBlock;
    std::uint64_t m_nonFiniteBlock = kNoBlock;
    std::uint64_t m_nonFiniteBlocks = 0;
    std::uint64_t m_missedHere = 0;
    std::uint64_t m_invalidHere = 0;
    bool m_resetNext = true;
    bool m_firstCall = true;
    float m_wet = 1.0f;          ///< the plugin's share of the output (1: all of it; below: fading in from dry)
    std::int64_t m_wetFrom = 0;  ///< output frames before this play dry (a plugin attached mid-stream)
    std::vector<float> m_dry;
    std::size_t m_dryFrames = 1;
};

} // namespace

// ---- State in the model -----------------------------------------------------------------------------------------

std::string pluginState(const EffectInstance& effect) {
    std::string out;
    for (const EffectData& d : effect.data) { // sorted by key: the parts come in order
        if (isStatePart(d.key)) {
            if (const std::string* text = d.text()) {
                out += *text;
            }
        }
    }
    return out;
}

std::expected<void, Error> setPluginState(EffectInstance& effect, std::string_view base64) {
    if (base64.size() > (kMaxStateBytes + 2) / 3 * 4) {
        return std::unexpected(Error(ErrorCode::Unsupported, "the plugin's state is " + std::to_string(base64.size() / 4 * 3 / 1024) +
                                                                 " KB; a plugin effect keeps at most " +
                                                                 std::to_string(kMaxStateBytes / 1024) + " KB"));
    }
    std::erase_if(effect.data, [](const EffectData& d) { return d.key.starts_with(kStateKeyPrefix); });
    for (std::size_t part = 0; part * kStatePartChars < base64.size(); ++part) {
        char key[32];
        std::snprintf(key, sizeof key, "%s%03zu", std::string(kStateKeyPrefix).c_str(), part);
        effect.setData(EffectData{key, std::string(base64.substr(part * kStatePartChars, kStatePartChars))});
    }
    if (!base64.empty()) {
        char sum[24];
        std::snprintf(sum, sizeof sum, "%016llx", static_cast<unsigned long long>(fnv(base64)));
        effect.setData(EffectData{std::string(kStateSumKey), std::string(sum)});
    }
    return {};
}

EffectInstance makePluginEffect(const PluginInfo& plugin, EffectInstanceId id) {
    EffectInstance e;
    e.id = id;
    e.effect = plugin.effectId();
    e.version = 1;
    e.setData(EffectData{std::string(kPluginNameKey), plugin.vendor.empty() ? plugin.name : plugin.name + " (" + plugin.vendor + ")"});
    return e;
}

std::string_view toString(PluginEffectStatus status) {
    switch (status) {
    case PluginEffectStatus::NotPlugin:
        return "notPlugin";
    case PluginEffectStatus::Ready:
        return "ready";
    case PluginEffectStatus::Missing:
        return "missing";
    case PluginEffectStatus::Crashed:
        return "crashed";
    case PluginEffectStatus::Failed:
        return "failed";
    case PluginEffectStatus::Hung:
        return "hung";
    }
    return "?";
}

// ---- PluginEffects ----------------------------------------------------------------------------------------------

struct PluginEffects::State {
    std::shared_ptr<PluginHost> host;
    std::shared_ptr<PluginCatalog> catalog;
    mutable CheckedMutex mutex;
    struct Idle {
        std::shared_ptr<PluginInstance> instance;
        std::chrono::steady_clock::time_point since;
        std::vector<std::uint32_t> touched; ///< parameters its last processor may have changed (sorted)
    };
    std::multimap<std::uint64_t, Idle> pool;                           ///< by effect instance id
    std::map<std::uint64_t, std::weak_ptr<PluginInstance>> live;      ///< the most recent processor's, by effect id
    std::map<std::uint64_t, std::string> problems;                     ///< why an effect could not be instantiated
    std::map<std::uint64_t, std::shared_ptr<PluginInstance>> editors;  ///< instances opened only for their editor
    /// The latency the last instance of an effect had, with that state at that rate: what a loading processor of it
    /// reports until its plugin is ready.
    struct Hint {
        std::uint64_t hash = 0;
        double sampleRate = 0.0;
        int latency = 0;
    };
    std::map<std::uint64_t, Hint> hints;

    // Work off the calling thread (loading plugins, editor and state requests), in order, on a few workers.
    CheckedMutex jobsMutex;
    std::condition_variable_any jobsChanged;
    std::deque<std::function<void()>> jobs;
    std::vector<std::jthread> workers; ///< last: stopped and joined first, while everything above still exists

    static constexpr std::size_t kMaxIdle = 32;
    static constexpr std::chrono::seconds kIdleLife{60};
    static constexpr int kWorkers = 2;

    /// Caller holds `mutex`.
    void trim() {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(pool, [&](const auto& e) { return !e.second.instance->running() || now - e.second.since > kIdleLife; });
        while (pool.size() > kMaxIdle) {
            auto oldest = std::min_element(pool.begin(), pool.end(),
                                           [](const auto& a, const auto& b) { return a.second.since < b.second.since; });
            pool.erase(oldest);
        }
    }

    void release(std::uint64_t effect, std::shared_ptr<PluginInstance> instance, std::vector<std::uint32_t> touched) {
        if (!instance || !instance->running()) {
            return;
        }
        std::lock_guard lock(mutex);
        pool.emplace(effect, Idle{std::move(instance), std::chrono::steady_clock::now(), std::move(touched)});
        trim();
    }

    std::shared_ptr<PluginInstance> instanceFor(std::uint64_t effect) const {
        std::lock_guard lock(mutex);
        if (const auto it = live.find(effect); it != live.end()) {
            if (auto instance = it->second.lock()) {
                return instance;
            }
        }
        if (const auto it = editors.find(effect); it != editors.end()) {
            return it->second;
        }
        return nullptr;
    }

    /// The latency a processor of `effect` reports while its plugin loads.
    int guess(std::uint64_t effect, std::uint64_t hash, double sampleRate, int scanned) const {
        std::lock_guard lock(mutex);
        if (const auto it = hints.find(effect); it != hints.end() && it->second.hash == hash && it->second.sampleRate == sampleRate) {
            return it->second.latency;
        }
        return scanned;
    }

    /// Caller holds `mutex`: `instance` plays `effect` now.
    void playing(std::uint64_t effect, const std::shared_ptr<PluginInstance>& instance, std::uint64_t hash) {
        problems.erase(effect);
        live[effect] = instance;
        hints[effect] = Hint{hash, instance->sampleRate(), instance->latency()};
    }

    /// `chosen` (an idle instance, or null) made to hold `state`, else a new instance: blocks on the helper.
    std::expected<std::shared_ptr<PluginInstance>, std::string> load(const PluginInfo& plugin, double sampleRate,
                                                                     const std::string& state, std::uint64_t hash,
                                                                     std::uint64_t effect, std::shared_ptr<PluginInstance> chosen,
                                                                     bool stale) {
        if (chosen && (stale || !chosen->hasKnownState(hash))) {
            // Another state (an undo, a preset): load it, or start afresh when there is none to load.
            if (state.empty() || !chosen->loadState(state)) {
                chosen.reset();
            } else {
                chosen->setKnownState(hash);
            }
        }
        if (!chosen) {
            auto made = host->instantiate(plugin, sampleRate, state);
            if (!made) {
                std::lock_guard lock(mutex);
                problems[effect] = made.error().message;
                return std::unexpected(made.error().message);
            }
            chosen = std::move(*made);
            chosen->setKnownState(hash);
        }
        std::lock_guard lock(mutex);
        playing(effect, chosen, hash);
        return chosen;
    }

    std::expected<void, Error> openEditor(const EffectInstance& effect, double sampleRate) {
        const auto plugin = catalog->find(effect.effect);
        if (!plugin) {
            return std::unexpected(Error(ErrorCode::NotFound, "this plugin is not installed on this computer"));
        }
        std::shared_ptr<PluginInstance> instance = instanceFor(effect.id.value);
        if (!instance || !instance->running()) {
            const std::string state = pluginState(effect);
            auto made = host->instantiate(*plugin, sampleRate, state);
            if (!made) {
                return std::unexpected(made.error());
            }
            instance = std::move(*made);
            instance->setKnownState(fnv(state));
            std::lock_guard lock(mutex);
            editors[effect.id.value] = instance;
        }
        return instance->openEditor(plugin->vendor.empty() ? plugin->name : plugin->name + " - " + plugin->vendor);
    }

    std::expected<std::string, Error> captureState(const EffectInstance& effect) {
        const std::shared_ptr<PluginInstance> instance = instanceFor(effect.id.value);
        if (!instance) {
            return std::unexpected(Error(ErrorCode::NotFound, "this plugin effect has no running instance"));
        }
        auto state = instance->saveState();
        if (!state) {
            return std::unexpected(state.error());
        }
        instance->setKnownState(fnv(*state));
        return state;
    }

    void post(std::function<void()> job) {
        std::lock_guard lock(jobsMutex);
        if (workers.empty()) {
            for (int i = 0; i < kWorkers; ++i) {
                workers.emplace_back([this](std::stop_token stop) { work(stop); });
            }
        }
        jobs.push_back(std::move(job));
        jobsChanged.notify_one();
    }

    void work(std::stop_token stop) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(jobsMutex);
                if (!jobsChanged.wait(lock, stop, [&] { return !jobs.empty(); })) {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            try {
                job();
            } catch (...) {
                std::fprintf(stderr, "[plugins] a plugin job failed with an exception\n"); // never takes the worker down
            }
        }
    }
};

PluginEffects::PluginEffects(std::shared_ptr<PluginHost> host, std::shared_ptr<PluginCatalog> catalog)
    : m_state(std::make_unique<State>()) {
    m_state->host = std::move(host);
    m_state->catalog = std::move(catalog);
}

std::shared_ptr<PluginEffects> PluginEffects::create(std::shared_ptr<PluginHost> host, std::shared_ptr<PluginCatalog> catalog) {
    return std::shared_ptr<PluginEffects>(new PluginEffects(std::move(host), std::move(catalog)));
}

PluginEffects::~PluginEffects() = default;

std::shared_ptr<PluginHost> PluginEffects::host() const { return m_state->host; }
std::shared_ptr<PluginCatalog> PluginEffects::catalog() const { return m_state->catalog; }

bool PluginEffects::implements(std::string_view effectId) const {
    if (!effectId.starts_with(kPluginEffectPrefix)) {
        return false;
    }
    const auto plugin = m_state->catalog->find(effectId);
    return plugin && plugin->effect && plugin->problem.empty();
}

std::unique_ptr<audio::EffectProcessor> PluginEffects::create(const EffectInstance& instance, const EffectDescriptor& descriptor,
                                                              int channels, int sampleRate, audio::ChainContext context,
                                                              audio::EffectProcessor* replacing) {
    (void)descriptor;
    const auto plugin = m_state->catalog->find(instance.effect);
    if (!plugin) {
        return nullptr;
    }
    const std::string state = pluginState(instance);
    const std::uint64_t hash = fnv(state);
    const bool offline = audio::inOfflineRender();
    const HostOptions& options = m_state->host->options();
    PluginProcessor::Setup setup;
    setup.owner = weak_from_this();
    setup.effect = instance.id;
    setup.name = displayName(*plugin);
    setup.pending = std::make_shared<PendingInstance>();
    setup.channels = channels;
    setup.sampleRate = sampleRate;
    setup.live = context == audio::ChainContext::Live;
    setup.blockWait = offline ? options.blockTimeout : std::min(options.blockTimeout, options.playbackBlockTimeout);
    setup.readyWait = options.requestTimeout;
    if (offline) {
        setup.incidents = audio::offlineRenderIncidents();
    }
    // A processor that has its instance now (counted as one of its users).
    const auto withInstance = [&](const std::shared_ptr<PluginInstance>& ready) -> std::unique_ptr<audio::EffectProcessor> {
        setup.latency = ready->latency();
        ready->addUser();
        setup.pending->publish(ready);
        return std::make_unique<PluginProcessor>(std::move(setup));
    };

    // Rebuilt because its plugin now reports another latency: the new processor takes the instance over (with the state
    // its editor may have given it); the chain stops playing the old one before the new one first processes.
    if (const auto* old = dynamic_cast<const PluginProcessor*>(replacing); old != nullptr && old->effect() == instance.id) {
        if (const auto taken = old->playing(); taken && taken->running() && taken->sampleRate() == sampleRate) {
            setup.touched = old->touched();
            return withInstance(taken);
        }
    }

    std::shared_ptr<PluginInstance> chosen;
    bool stale = false; ///< its parameters may differ from the state's where this effect does not set them
    {
        // An idle instance of this effect at this rate, preferably one that already has this state.
        std::lock_guard lock(m_state->mutex);
        m_state->trim();
        const std::vector<std::uint32_t> sets = paramIdsOf(instance);
        const auto range = m_state->pool.equal_range(instance.id.value);
        auto pick = range.second;
        bool pickStale = false;
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.instance->sampleRate() != sampleRate) {
                continue;
            }
            // Parameters the last processor sent but this one will not keep the values it left behind.
            const bool itStale = !std::includes(sets.begin(), sets.end(), it->second.touched.begin(), it->second.touched.end());
            const bool better = pick == range.second || (pickStale && !itStale) ||
                                (pickStale == itStale && it->second.instance->hasKnownState(hash));
            if (better) {
                pick = it;
                pickStale = itStale;
            }
        }
        if (pick != range.second) {
            chosen = std::move(pick->second.instance);
            stale = pickStale;
            m_state->pool.erase(pick);
        }
        if (chosen && !stale && chosen->hasKnownState(hash)) {
            m_state->playing(instance.id.value, chosen, hash); // ready as it is: no request to the helper
        }
    }
    if (chosen && !stale && chosen->hasKnownState(hash)) {
        return withInstance(chosen);
    }
    if (offline || !options.deferInstantiation) {
        // Loaded here: an offline render (export) has the plugin from its first sample.
        auto loaded = m_state->load(*plugin, static_cast<double>(sampleRate), state, hash, instance.id.value, std::move(chosen), stale);
        if (!loaded) {
            setup.latency = plugin->latency; // bypassed as if it had the latency it was scanned with
            setup.pending->fail(loaded.error());
            return std::make_unique<PluginProcessor>(std::move(setup));
        }
        return withInstance(*loaded);
    }
    // Loaded on a worker (the feeder, the thread starting playback never wait for a helper): until it is ready the
    // processor plays the dry signal, delayed by the latency the plugin is expected to have.
    setup.latency = m_state->guess(instance.id.value, hash, static_cast<double>(sampleRate), plugin->latency);
    m_state->post([self = m_state.get(), pending = setup.pending, info = *plugin, rate = static_cast<double>(sampleRate),
                   stateText = state, hash, id = instance.id.value, idle = std::move(chosen), stale]() mutable {
        auto loaded = self->load(info, rate, stateText, hash, id, std::move(idle), stale);
        if (!loaded) {
            pending->fail(loaded.error());
            return;
        }
        (*loaded)->addUser();
        if (!pending->publish(*loaded) && (*loaded)->dropUser()) {
            self->release(id, *loaded, {}); // its processor went away meanwhile
        }
    });
    return std::make_unique<PluginProcessor>(std::move(setup));
}

std::string PluginEffects::problem(const EffectInstance& effect) const {
    if (!effect.effect.starts_with(kPluginEffectPrefix)) {
        return {};
    }
    const auto plugin = m_state->catalog->find(effect.effect);
    if (!plugin) {
        const EffectData* name = effect.findData(kPluginNameKey);
        return "Missing plugin: " + (name != nullptr && name->text() != nullptr ? *name->text() : effect.effect);
    }
    if (!plugin->problem.empty()) {
        return displayName(*plugin) + ": " + plugin->problem;
    }
    std::lock_guard lock(m_state->mutex);
    if (const auto it = m_state->problems.find(effect.id.value); it != m_state->problems.end()) {
        return displayName(*plugin) + " could not be loaded: " + it->second;
    }
    return {};
}

std::expected<void, Error> PluginEffects::prepare(const EffectInstance& effect, int sampleRate) {
    const auto plugin = m_state->catalog->find(effect.effect);
    if (!plugin) {
        return std::unexpected(Error(ErrorCode::NotFound, "this plugin is not installed on this computer"));
    }
    const std::string state = pluginState(effect);
    const std::uint64_t hash = fnv(state);
    {
        std::lock_guard lock(m_state->mutex);
        m_state->trim();
        const auto range = m_state->pool.equal_range(effect.id.value);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.instance->sampleRate() == sampleRate && it->second.instance->hasKnownState(hash)) {
                return {};
            }
        }
    }
    auto made = m_state->host->instantiate(*plugin, static_cast<double>(sampleRate), state);
    if (!made) {
        std::lock_guard lock(m_state->mutex);
        m_state->problems[effect.id.value] = made.error().message;
        return std::unexpected(made.error());
    }
    (*made)->setKnownState(hash);
    {
        std::lock_guard lock(m_state->mutex);
        m_state->problems.erase(effect.id.value);
        m_state->hints[effect.id.value] = State::Hint{hash, static_cast<double>(sampleRate), (*made)->latency()};
    }
    m_state->release(effect.id.value, std::move(*made), {});
    return {};
}

void PluginEffects::release(EffectInstanceId effect, std::shared_ptr<PluginInstance> instance, std::vector<std::uint32_t> touched) {
    m_state->release(effect.value, std::move(instance), std::move(touched));
}

std::shared_ptr<PluginInstance> PluginEffects::instanceFor(EffectInstanceId effect) const {
    return m_state->instanceFor(effect.value);
}

PluginEffectReport PluginEffects::status(const EffectInstance& effect) const {
    PluginEffectReport report;
    if (!effect.effect.starts_with(kPluginEffectPrefix)) {
        return report;
    }
    const auto plugin = m_state->catalog->find(effect.effect);
    if (!plugin) {
        const EffectData* name = effect.findData(kPluginNameKey);
        report.status = PluginEffectStatus::Missing;
        report.message = "Missing plugin: " + (name != nullptr && name->text() != nullptr ? *name->text() : effect.effect);
        return report;
    }
    if (!plugin->problem.empty()) {
        report.status = PluginEffectStatus::Failed;
        report.message = plugin->name + ": " + plugin->problem;
        return report;
    }
    if (const auto instance = instanceFor(effect.id)) {
        report.missedBlocks = instance->missedBlocks();
        switch (instance->health()) {
        case InstanceHealth::Running:
            report.status = PluginEffectStatus::Ready;
            break;
        case InstanceHealth::Crashed:
            report.status = PluginEffectStatus::Crashed;
            break;
        case InstanceHealth::Failed:
            report.status = PluginEffectStatus::Failed;
            break;
        case InstanceHealth::Hung:
            report.status = PluginEffectStatus::Hung;
            break;
        }
        report.message = instance->problem();
        return report;
    }
    std::lock_guard lock(m_state->mutex);
    if (const auto it = m_state->problems.find(effect.id.value); it != m_state->problems.end()) {
        report.status = PluginEffectStatus::Failed;
        report.message = it->second;
        return report;
    }
    report.status = PluginEffectStatus::Ready;
    return report;
}

std::expected<void, Error> PluginEffects::openEditor(const EffectInstance& effect, double sampleRate) {
    return m_state->openEditor(effect, sampleRate);
}

std::expected<std::string, Error> PluginEffects::captureState(const EffectInstance& effect) {
    return m_state->captureState(effect);
}

void PluginEffects::openEditorAsync(const EffectInstance& effect, double sampleRate,
                                    std::function<void(std::expected<void, Error>)> done) {
    m_state->post([self = m_state.get(), effect, sampleRate, callback = std::move(done)] {
        auto result = self->openEditor(effect, sampleRate);
        if (callback) {
            callback(std::move(result));
        }
    });
}

void PluginEffects::captureStateAsync(const EffectInstance& effect, std::function<void(std::expected<std::string, Error>)> done) {
    m_state->post([self = m_state.get(), effect, callback = std::move(done)] {
        auto result = self->captureState(effect);
        if (callback) {
            callback(std::move(result));
        }
    });
}

void installPluginEffects(const std::shared_ptr<PluginEffects>& effects) {
    audio::setExternalEffectFactory(effects);
    if (effects) {
        setEffectDescriptorProvider(effects->catalog()->descriptors());
    }
}

} // namespace wabcut::plughost
