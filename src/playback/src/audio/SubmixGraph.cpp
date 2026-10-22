#include "SubmixGraph.h"

#include "Fader.h"

#include <wabcut/audio/Gain.h>
#include <wabcut/playback/audio/ChannelLayout.h>

#include <wabcut/model/SubmixOps.h>

#include <algorithm>
#include <cmath>

namespace wabcut::playback::detail {

/// A delay line of a fixed length (its delay), allocated on the control side and shared by successive programs with the
/// same delays, so a parameter change does not lose what it holds.
struct SubmixGraph::Delay {
    Delay(int frames, int channelCount)
        : length(frames), channels(channelCount),
          buffer(static_cast<std::size_t>(frames + 1) * static_cast<std::size_t>(channelCount), 0.0f) {}

    const int length;
    const int channels;
    std::vector<float> buffer;
    std::size_t write = 0; ///< frame slot written next

    void clear() noexcept {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        write = 0;
    }

    /// Delays `frames` interleaved frames in place by `length`.
    void process(float* io, std::int64_t frames) noexcept {
        const auto ch = static_cast<std::size_t>(channels);
        const auto slots = static_cast<std::size_t>(length) + 1;
        for (std::int64_t f = 0; f < frames; ++f) {
            float* x = io + static_cast<std::size_t>(f) * ch;
            float* w = buffer.data() + write * ch;
            const std::size_t read = (write + 1) % slots; // the oldest slot: written `length` frames ago
            float* r = buffer.data() + read * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                w[c] = x[c];
            }
            for (std::size_t c = 0; c < ch; ++c) {
                x[c] = r[c];
            }
            write = read;
        }
    }
};

struct SubmixGraph::Program {
    struct Send {
        int destination = 0; ///< submix index
        bool pre = false;
        bool muted = false;
        ParamValue level;
        ParamValue pan;
        TrackSend source; ///< as the model had it (to tell whether a program changed)
    };
    struct Route {
        TrackId track;
        int output = -1; ///< -1: the Mix
        TrackId outputTrack;
        std::vector<Send> sends;
    };
    struct Bus {
        TrackPtr track;   ///< the submix track this bus plays
        bool mono = false;
        int output = -1;  ///< -1: the Mix
        std::unique_ptr<audio::EffectChain> chain;
        int alignment = 0;   ///< A: the latest path into it
        int latency = 0;     ///< its chain's
        int outputDelay = 0; ///< what its output waits for its destination's alignment
        std::shared_ptr<Delay> trackDelay;  ///< the tracks' inputs, by `alignment`
        std::shared_ptr<Delay> outputDelayLine;
    };

    std::vector<Route> routes; ///< per slot
    std::vector<Bus> buses;    ///< per submix (at most kMaxSubmixes)
    std::vector<int> order;    ///< processing order of the buses
    int mixAlignment = 0;
    std::shared_ptr<Delay> mixDelay; ///< the tracks' inputs of the Mix, by mixAlignment
    std::uint64_t key = 0;
    bool routed = false;
};

namespace {

/// The submix index of `id` among the first `count` submix tracks, or -1.
int submixIndexOf(const Sequence& seq, TrackId id, int count) {
    for (int i = 0; i < count && i < static_cast<int>(seq.submixTracks.size()); ++i) {
        if (seq.submixTracks[static_cast<std::size_t>(i)]->id == id) {
            return i;
        }
    }
    return -1;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 0x100000001b3ULL; }

} // namespace

SubmixGraph::SubmixGraph(int trackSlots, int channels, int sampleRate, int maxBlockFrames)
    : m_slots(std::max(trackSlots, 0)), m_channels(std::max(channels, 1)), m_sampleRate(std::max(sampleRate, 1)),
      m_maxBlock(std::max(maxBlockFrames, 16)), m_rampFrames(std::max(sampleRate / 100, 1)),
      m_live(std::make_unique<LiveFader[]>(kMaxSubmixes)),
      m_submixTrack(std::make_unique<std::atomic<std::uint64_t>[]>(kMaxSubmixes)) {
    m_owned = std::make_unique<Program>();
    m_owned->routes.resize(static_cast<std::size_t>(m_slots));
    m_current.store(m_owned.get(), std::memory_order_seq_cst);
    const auto block = static_cast<std::size_t>(m_maxBlock) * static_cast<std::size_t>(m_channels);
    m_inputs.assign(block * (kMaxSubmixes + 1), 0.0f);
    m_send.assign(block, 0.0f);
    m_work.assign(block, 0.0f);
    m_out.assign(block, 0.0f);
    m_bus.assign(block, 0.0f);
    m_sendGain.assign(static_cast<std::size_t>(m_slots), std::array<float, 5>{});
    for (int i = 0; i < kMaxSubmixes; ++i) {
        m_submixTrack[static_cast<std::size_t>(i)].store(0);
    }
}

SubmixGraph::~SubmixGraph() = default;

// ---- Control side ----------------------------------------------------------------------------------------------------

void SubmixGraph::update(const Sequence* seq, std::span<const TrackId> slotTracks) {
    collect();
    const int submixes = seq != nullptr ? std::min(static_cast<int>(seq->submixTracks.size()), kMaxSubmixes) : 0;
    m_dropped.store(seq != nullptr ? std::max(static_cast<int>(seq->submixTracks.size()) - kMaxSubmixes, 0) : 0,
                    std::memory_order_relaxed);
    const bool routing = seq != nullptr && usesSubmixRouting(*seq);

    // What the new program would route; unchanged: keep the current one (its chains keep their state).
    auto next = std::make_unique<Program>();
    next->routes.resize(static_cast<std::size_t>(m_slots));
    for (int i = 0; i < m_slots && i < static_cast<int>(slotTracks.size()); ++i) {
        Program::Route& r = next->routes[static_cast<std::size_t>(i)];
        r.track = slotTracks[static_cast<std::size_t>(i)];
        const Track* t = routing && r.track.isValid() ? seq->findTrack(r.track) : nullptr;
        if (t == nullptr) {
            continue;
        }
        r.outputTrack = t->output;
        r.output = t->output.isValid() ? submixIndexOf(*seq, t->output, submixes) : -1;
        for (const TrackSend& s : t->sends) {
            const int destination = submixIndexOf(*seq, s.submix, submixes);
            if (destination < 0) {
                continue; // a submix past kMaxSubmixes: not mixed (droppedSubmixes)
            }
            r.sends.push_back(Program::Send{destination, s.preFader, s.muted, s.level, s.pan, s});
        }
        next->routed = next->routed || r.output >= 0 || !r.sends.empty();
    }
    if (routing) {
        for (int k = 0; k < submixes; ++k) {
            Program::Bus bus;
            bus.track = seq->submixTracks[static_cast<std::size_t>(k)];
            bus.mono = bus.track->format == ChannelFormat::Mono;
            bus.output = bus.track->output.isValid() ? submixIndexOf(*seq, bus.track->output, submixes) : -1;
            next->buses.push_back(std::move(bus));
        }
    }
    {
        const Program& old = *m_owned;
        bool same = old.routed == next->routed && old.buses.size() == next->buses.size() && old.routes.size() == next->routes.size();
        for (std::size_t i = 0; same && i < next->routes.size(); ++i) {
            const Program::Route& a = old.routes[i];
            const Program::Route& b = next->routes[i];
            same = a.track == b.track && a.output == b.output && a.outputTrack == b.outputTrack && a.sends.size() == b.sends.size();
            for (std::size_t k = 0; same && k < b.sends.size(); ++k) {
                same = a.sends[k].source == b.sends[k].source && a.sends[k].destination == b.sends[k].destination;
            }
        }
        for (std::size_t k = 0; same && k < next->buses.size(); ++k) {
            same = old.buses[k].track == next->buses[k].track && old.buses[k].output == next->buses[k].output;
        }
        if (same) {
            return;
        }
    }

    // The chains: a bus of the same submix track (and channel layout) shares the old chain's processors.
    for (Program::Bus& bus : next->buses) {
        bus.chain = std::make_unique<audio::EffectChain>(bus.mono ? 1 : m_channels, m_sampleRate, audio::ChainContext::Live);
        const audio::EffectChain* adopt = nullptr;
        for (const Program::Bus& previous : m_owned->buses) {
            if (previous.track && previous.track->id == bus.track->id && previous.mono == bus.mono && previous.chain) {
                adopt = previous.chain.get();
            }
        }
        bus.chain->bind(bus.track->effects, adopt);
        bus.latency = bus.chain->latencyFrames();
    }
    // Processing order and alignment (the latest path into each bus).
    if (seq != nullptr && !next->buses.empty()) {
        if (const auto order = submixProcessingOrder(*seq)) {
            for (const std::size_t k : *order) {
                if (k < next->buses.size()) {
                    next->order.push_back(static_cast<int>(k));
                }
            }
        }
    }
    std::uint64_t uncompensated = 0;
    for (const int k : next->order) {
        Program::Bus& bus = next->buses[static_cast<std::size_t>(k)];
        const int arrives = bus.alignment + bus.latency;
        if (bus.output >= 0) {
            Program::Bus& to = next->buses[static_cast<std::size_t>(bus.output)];
            to.alignment = std::max(to.alignment, arrives);
        } else {
            next->mixAlignment = std::max(next->mixAlignment, arrives);
        }
    }
    const auto clampDelay = [&](int frames) {
        if (frames > kMaxCompensation) {
            ++uncompensated;
            return kMaxCompensation;
        }
        return std::max(frames, 0);
    };
    std::uint64_t key = 0xcbf29ce484222325ULL;
    for (Program::Bus& bus : next->buses) {
        const int destination = bus.output >= 0 ? next->buses[static_cast<std::size_t>(bus.output)].alignment : next->mixAlignment;
        bus.outputDelay = clampDelay(destination - bus.alignment - bus.latency);
        bus.alignment = clampDelay(bus.alignment);
        key = mix(mix(key, static_cast<std::uint64_t>(bus.alignment)), static_cast<std::uint64_t>(bus.outputDelay));
    }
    next->mixAlignment = clampDelay(next->mixAlignment);
    key = mix(key, static_cast<std::uint64_t>(next->mixAlignment));
    next->key = key;
    m_uncompensated.fetch_add(uncompensated, std::memory_order_relaxed);
    // Delay lines: the same delays keep the old lines (and what they hold).
    const bool sameDelays = key == m_owned->key && next->buses.size() == m_owned->buses.size();
    const auto line = [&](int frames, const std::shared_ptr<Delay>& old) -> std::shared_ptr<Delay> {
        if (frames <= 0) {
            return nullptr;
        }
        if (sameDelays && old && old->length == frames) {
            return old;
        }
        return std::make_shared<Delay>(frames, m_channels);
    };
    for (std::size_t k = 0; k < next->buses.size(); ++k) {
        Program::Bus& bus = next->buses[k];
        const Program::Bus* old = k < m_owned->buses.size() ? &m_owned->buses[k] : nullptr;
        bus.trackDelay = line(bus.alignment, old != nullptr ? old->trackDelay : nullptr);
        bus.outputDelayLine = line(bus.outputDelay, old != nullptr ? old->outputDelayLine : nullptr);
    }
    next->mixDelay = line(next->mixAlignment, m_owned->mixDelay);

    for (int k = 0; k < kMaxSubmixes; ++k) {
        const std::uint64_t id = k < static_cast<int>(next->buses.size()) ? next->buses[static_cast<std::size_t>(k)].track->id.value : 0;
        if (m_submixTrack[static_cast<std::size_t>(k)].exchange(id, std::memory_order_acq_rel) != id) {
            m_live[static_cast<std::size_t>(k)].clear(); // the index plays another submix now
        }
    }
    m_current.store(next.get(), std::memory_order_seq_cst);
    const std::uint64_t mark = m_renderSeq.load(std::memory_order_seq_cst);
    m_retired.emplace_back(std::move(m_owned), mark);
    m_owned = std::move(next);
}

void SubmixGraph::collect() {
    const std::uint64_t now = m_renderSeq.load(std::memory_order_seq_cst);
    std::erase_if(m_retired, [now](const auto& retired) {
        const std::uint64_t mark = retired.second;
        return mark % 2 == 0 || now > mark;
    });
}

bool SubmixGraph::routed() const noexcept { return m_owned && m_owned->routed; }

int SubmixGraph::indexOf(TrackId submix) const noexcept {
    for (int k = 0; k < kMaxSubmixes; ++k) {
        if (submix.isValid() && m_submixTrack[static_cast<std::size_t>(k)].load(std::memory_order_acquire) == submix.value) {
            return k;
        }
    }
    return -1;
}

void SubmixGraph::applyFlags(const Sequence* seq, std::span<const TrackId> slotTracks, audio::Mixer& mixer) const {
    const int submixes = seq != nullptr ? std::min(static_cast<int>(seq->submixTracks.size()), kMaxSubmixes) : 0;
    const bool routing = seq != nullptr && usesSubmixRouting(*seq);
    // Which submixes each source reaches (outputs and sends, then submix outputs), as bit sets.
    const auto downstreamOfSubmix = [&](int k) {
        std::uint32_t reached = 0;
        for (int at = k, guard = 0; at >= 0 && guard <= submixes; ++guard) {
            reached |= 1u << at;
            const Track& t = *seq->submixTracks[static_cast<std::size_t>(at)];
            at = t.output.isValid() ? submixIndexOf(*seq, t.output, submixes) : -1;
        }
        return reached;
    };
    std::vector<const Track*> tracks(static_cast<std::size_t>(m_slots), nullptr);
    std::vector<std::uint32_t> reaches(static_cast<std::size_t>(m_slots), 0);
    for (int i = 0; i < m_slots && i < static_cast<int>(slotTracks.size()); ++i) {
        const Track* t = seq != nullptr && slotTracks[static_cast<std::size_t>(i)].isValid()
                             ? seq->findTrack(slotTracks[static_cast<std::size_t>(i)])
                             : nullptr;
        tracks[static_cast<std::size_t>(i)] = t;
        if (t != nullptr && routing) {
            std::uint32_t r = 0;
            if (const int k = t->output.isValid() ? submixIndexOf(*seq, t->output, submixes) : -1; k >= 0) {
                r |= downstreamOfSubmix(k);
            }
            for (const TrackSend& s : t->sends) {
                if (const int k = submixIndexOf(*seq, s.submix, submixes); k >= 0) {
                    r |= downstreamOfSubmix(k);
                }
            }
            reaches[static_cast<std::size_t>(i)] = r;
        }
    }
    std::uint32_t soloedSubmixes = 0;
    std::vector<std::uint32_t> submixReaches(static_cast<std::size_t>(submixes), 0);
    for (int k = 0; k < submixes; ++k) {
        if (seq->submixTracks[static_cast<std::size_t>(k)]->solo) {
            soloedSubmixes |= 1u << k;
        }
        submixReaches[static_cast<std::size_t>(k)] = downstreamOfSubmix(k);
    }
    // Submixes fed (directly or not) by a soloed track or submix.
    std::uint32_t fedBySolo = 0;
    for (int i = 0; i < m_slots; ++i) {
        const Track* t = tracks[static_cast<std::size_t>(i)];
        if (t != nullptr && t->solo) {
            fedBySolo |= reaches[static_cast<std::size_t>(i)];
        }
    }
    for (int k = 0; k < submixes; ++k) {
        if ((soloedSubmixes >> k) & 1u) {
            fedBySolo |= submixReaches[static_cast<std::size_t>(k)];
        }
    }
    for (int i = 0; i < m_slots; ++i) {
        const Track* t = tracks[static_cast<std::size_t>(i)];
        mixer.setBusMuted(i, t == nullptr || t->muted || !t->enabled);
        const bool soloed = t != nullptr && (t->solo || (routing && (reaches[static_cast<std::size_t>(i)] & soloedSubmixes) != 0));
        mixer.setBusSoloed(i, soloed);
    }
    for (int k = 0; k < kMaxSubmixes; ++k) {
        const Track* s = k < submixes ? seq->submixTracks[static_cast<std::size_t>(k)].get() : nullptr;
        mixer.setBusMuted(busOf(k), s == nullptr || s->muted || !s->enabled);
        const bool soloed = s != nullptr && routing &&
                            (s->solo || ((fedBySolo >> k) & 1u) != 0 || (submixReaches[static_cast<std::size_t>(k)] & soloedSubmixes) != 0);
        mixer.setBusSoloed(busOf(k), soloed);
    }
}

// ---- Audio thread ----------------------------------------------------------------------------------------------------

void SubmixGraph::beginRender() noexcept {
    m_renderSeq.fetch_add(1, std::memory_order_seq_cst); // odd: rendering
    m_active = m_current.load(std::memory_order_seq_cst);
}

void SubmixGraph::endRender() noexcept {
    m_active = nullptr;
    m_renderSeq.fetch_add(1, std::memory_order_seq_cst); // even: done
}

bool SubmixGraph::active() const noexcept { return m_active != nullptr && m_active->routed; }

int SubmixGraph::latencyFrames() const noexcept { return active() ? m_active->mixAlignment : 0; }

std::uint64_t SubmixGraph::delayKey() const noexcept { return active() ? m_active->key : 0; }

void SubmixGraph::restart(Ticks origin, std::int64_t ticksPerFrame) noexcept {
    m_clock = audio::CurveClock{origin, ticksPerFrame, nullptr};
    m_frame = 0;
    for (auto& g : m_sendGain) {
        g.fill(-1.0f); // the first block of a send starts at its target
    }
    if (m_active == nullptr) {
        return;
    }
    for (Program::Bus& bus : m_active->buses) {
        if (bus.chain) {
            bus.chain->reset();
        }
        if (bus.trackDelay) {
            bus.trackDelay->clear();
        }
        if (bus.outputDelayLine) {
            bus.outputDelayLine->clear();
        }
    }
    if (m_active->mixDelay) {
        m_active->mixDelay->clear();
    }
}

std::span<float> SubmixGraph::inputOf(int destination) noexcept {
    const auto block = static_cast<std::size_t>(m_maxBlock) * static_cast<std::size_t>(m_channels);
    const std::size_t index = destination < 0 ? static_cast<std::size_t>(kMaxSubmixes) : static_cast<std::size_t>(destination);
    return std::span<float>(m_inputs.data() + index * block, static_cast<std::size_t>(m_frames) * static_cast<std::size_t>(m_channels));
}

void SubmixGraph::beginBlock(Ticks at, std::int64_t frames) noexcept {
    m_at = at;
    m_frames = std::min<std::int64_t>(frames, m_maxBlock);
    if (!active()) {
        return;
    }
    for (std::size_t k = 0; k < m_active->buses.size(); ++k) {
        const std::span<float> in = inputOf(static_cast<int>(k));
        std::fill(in.begin(), in.end(), 0.0f);
    }
    const std::span<float> mixIn = inputOf(-1);
    std::fill(mixIn.begin(), mixIn.end(), 0.0f);
}

namespace {

/// A send's audibility ramp over a block: from `current` (-1: none yet, start at the target) to `target`.
struct Ramp {
    float from;
    float to;
};

} // namespace

void SubmixGraph::sendPre(int slot, const float* pre, const audio::Mixer& mixer) noexcept {
    if (!active() || slot < 0 || slot >= m_slots) {
        return;
    }
    const Program::Route& route = m_active->routes[static_cast<std::size_t>(slot)];
    const auto ch = static_cast<std::size_t>(m_channels);
    const std::size_t samples = static_cast<std::size_t>(m_frames) * ch;
    const bool audible = mixer.busAudible(slot);
    for (std::size_t k = 0; k < route.sends.size() && k < 5; ++k) {
        const Program::Send& send = route.sends[k];
        if (!send.pre) {
            continue;
        }
        float& current = m_sendGain[static_cast<std::size_t>(slot)][k];
        const float target = audible && !send.muted ? 1.0f : 0.0f;
        const Ramp ramp{current < 0.0f ? target : current, target};
        current = target;
        if (ramp.from == 0.0f && ramp.to == 0.0f) {
            continue;
        }
        std::copy_n(pre, samples, m_send.data());
        applyTimelineFader(send.level, send.pan, m_at, m_frames, m_send.data(), m_channels, m_clock.ticksPerFrame);
        const std::span<float> dst = inputOf(send.destination);
        const double inv = 1.0 / static_cast<double>(std::max<std::int64_t>(m_frames, 1));
        for (std::int64_t f = 0; f < m_frames; ++f) {
            const auto g = ramp.from == ramp.to ? ramp.to
                                                : static_cast<float>(ramp.from + (ramp.to - ramp.from) * static_cast<double>(f + 1) * inv);
            for (std::size_t c = 0; c < ch; ++c) {
                dst[static_cast<std::size_t>(f) * ch + c] += m_send[static_cast<std::size_t>(f) * ch + c] * g;
            }
        }
    }
}

void SubmixGraph::sendPost(int slot, const float* post, const audio::Mixer& mixer) noexcept {
    if (!active() || slot < 0 || slot >= m_slots) {
        return;
    }
    const Program::Route& route = m_active->routes[static_cast<std::size_t>(slot)];
    const auto ch = static_cast<std::size_t>(m_channels);
    const std::size_t samples = static_cast<std::size_t>(m_frames) * ch;
    const bool audible = mixer.busAudible(slot);
    for (std::size_t k = 0; k < route.sends.size() && k < 5; ++k) {
        const Program::Send& send = route.sends[k];
        if (send.pre) {
            continue;
        }
        float& current = m_sendGain[static_cast<std::size_t>(slot)][k];
        const float target = audible && !send.muted ? 1.0f : 0.0f;
        const Ramp ramp{current < 0.0f ? target : current, target};
        current = target;
        if (ramp.from == 0.0f && ramp.to == 0.0f) {
            continue;
        }
        std::copy_n(post, samples, m_send.data());
        applyTimelineFader(send.level, send.pan, m_at, m_frames, m_send.data(), m_channels, m_clock.ticksPerFrame);
        const std::span<float> dst = inputOf(send.destination);
        const double inv = 1.0 / static_cast<double>(std::max<std::int64_t>(m_frames, 1));
        for (std::int64_t f = 0; f < m_frames; ++f) {
            const auto g = ramp.from == ramp.to ? ramp.to
                                                : static_cast<float>(ramp.from + (ramp.to - ramp.from) * static_cast<double>(f + 1) * inv);
            for (std::size_t c = 0; c < ch; ++c) {
                dst[static_cast<std::size_t>(f) * ch + c] += m_send[static_cast<std::size_t>(f) * ch + c] * g;
            }
        }
    }
}

std::span<float> SubmixGraph::outputOf(int slot) noexcept {
    if (!active() || slot < 0 || slot >= m_slots) {
        return {};
    }
    const int output = m_active->routes[static_cast<std::size_t>(slot)].output;
    if (output >= 0) {
        return inputOf(output);
    }
    return m_active->mixAlignment > 0 ? inputOf(-1) : std::span<float>{};
}

void SubmixGraph::finishBlock(audio::Mixer& mixer, std::span<float> mixBlock) noexcept {
    if (!active()) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    const std::int64_t n = m_frames;
    const std::size_t samples = static_cast<std::size_t>(n) * ch;
    const std::int64_t tpf = m_clock.ticksPerFrame;
    // The tracks' inputs, aligned to each bus's latest path.
    for (std::size_t k = 0; k < m_active->buses.size(); ++k) {
        if (const auto& delay = m_active->buses[k].trackDelay) {
            delay->process(inputOf(static_cast<int>(k)).data(), n);
        }
    }
    if (m_active->mixDelay) {
        m_active->mixDelay->process(inputOf(-1).data(), n);
    }
    for (const int k : m_active->order) {
        Program::Bus& bus = m_active->buses[static_cast<std::size_t>(k)];
        const Track& track = *bus.track;
        const std::span<float> in = inputOf(k);
        float* out = m_out.data();
        LiveFader& live = m_live[static_cast<std::size_t>(k)];
        const std::int64_t first = timelineFrame(m_at, tpf);
        const Ticks t{first * tpf};
        if (bus.mono) {
            // Folded to mono (the stereo mean, BS.775 for 5.1), its chain and fader at one channel, then panned.
            float* mono = m_work.data();
            for (std::int64_t f = 0; f < n; ++f) {
                const float* x = in.data() + static_cast<std::size_t>(f) * ch;
                float sum = 0.0f;
                if (ch == 6) {
                    sum = 0.5f * (x[0] + x[1]) + 0.70710678f * x[2] + 0.35355339f * (x[4] + x[5]);
                } else if (ch >= 2) {
                    sum = 0.5f * (x[0] + x[1]);
                } else {
                    sum = x[0];
                }
                mono[f] = sum;
            }
            bus.chain->process(mono, static_cast<int>(n), m_frame, m_clock);
            const bool liveGain = live.engaged() && live.apply(mono, n, 1, faderGain(track.volume.valueAt(t)), audio::StereoGains{},
                                                               m_rampFrames, false);
            if (!liveGain) {
                applyFader(track.volume, nullptr, first, first + n, mono, 1, [tpf](std::int64_t frame) { return Ticks{frame * tpf}; });
            }
            // Constant-power pan onto L / R (the parameter grid: gains at cell boundaries, linear between).
            constexpr std::int64_t cell = audio::kParamCellFrames;
            for (std::int64_t f = 0; f < n; ++f) {
                const std::int64_t frame = first + f;
                const std::int64_t c0 = (frame >= 0 ? frame / cell : -((-frame + cell - 1) / cell)) * cell;
                const audio::StereoGains a = audio::constantPowerPan(static_cast<float>(track.pan.valueAt(Ticks{c0 * tpf})));
                const audio::StereoGains b = track.pan.isConstant()
                                                 ? a
                                                 : audio::constantPowerPan(static_cast<float>(track.pan.valueAt(Ticks{(c0 + cell) * tpf})));
                const double u = static_cast<double>(frame - c0) / static_cast<double>(cell);
                float* o = out + static_cast<std::size_t>(f) * ch;
                std::fill(o, o + ch, 0.0f);
                if (ch >= 2) {
                    o[0] = mono[f] * static_cast<float>(a.left + (b.left - a.left) * u);
                    o[1] = mono[f] * static_cast<float>(a.right + (b.right - a.right) * u);
                } else {
                    o[0] = mono[f];
                }
            }
        } else {
            std::copy_n(in.data(), samples, out);
            bus.chain->process(out, static_cast<int>(n), m_frame, m_clock);
            const bool stereo = m_channels == 2;
            const bool liveDone =
                live.engaged() &&
                live.apply(out, n, m_channels, faderGain(track.volume.valueAt(t)),
                           stereo ? audio::stereoBalance(static_cast<float>(track.pan.valueAt(t))) : audio::StereoGains{}, m_rampFrames,
                           stereo);
            if (!liveDone) {
                applyTimelineFader(track.volume, track.pan, m_at, n, out, m_channels, tpf);
            }
        }
        // Its mixer bus (mute, solo, meter), then its wait for the destination's alignment, then the destination.
        const std::span<float> busOut(m_bus.data(), samples);
        std::fill(busOut.begin(), busOut.end(), 0.0f);
        mixer.addBusTo(busOf(k), std::span<const float>(out, samples), m_channels, busOut);
        if (bus.outputDelayLine) {
            bus.outputDelayLine->process(busOut.data(), n);
        }
        float* to = bus.output >= 0 ? inputOf(bus.output).data() : mixBlock.data();
        for (std::size_t s = 0; s < samples; ++s) {
            to[s] += busOut[s];
        }
    }
    // The tracks that go straight to the Mix, delayed by its alignment.
    if (m_active->mixAlignment > 0) {
        const std::span<float> mixIn = inputOf(-1);
        for (std::size_t s = 0; s < samples; ++s) {
            mixBlock[s] += mixIn[s];
        }
    }
    m_frame += n;
}

} // namespace wabcut::playback::detail
