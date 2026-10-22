#include <wabcut/playback/audio/OfflineMixdown.h>

#include "Fader.h"
#include "MasterBus.h"
#include "SubmixGraph.h"

#include <wabcut/audio/Gain.h>
#include <wabcut/audio/Mixer.h>
#include <wabcut/audio/RealTime.h>
#include <wabcut/audio/effects/EffectChain.h>
#include <wabcut/playback/audio/ChannelLayout.h>

#include <wabcut/model/SubmixOps.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace wabcut::playback {

namespace {

/// Plug-ins in an offline render load before they play (review A5-P2-5: the live player lets them load on a worker and
/// plays dry meanwhile). Export opens its own scope with its incident log; every other mixdown (the loudness analysis,
/// stems) gets one here.
struct OfflineScope {
    std::optional<audio::ScopedOfflineRender> scope;
    OfflineScope() {
        if (!audio::inOfflineRender()) {
            scope.emplace();
        }
    }
};

} // namespace

struct OfflineMixdown::Impl {
    struct Bus {
        const Track* track = nullptr;
        std::unique_ptr<TrackRenderer> renderer;
        bool audible = false;
    };

    Impl(std::shared_ptr<const Sequence> seq, int out, int sampleRate, Ticks from, AudioFeed& f, OfflineMixdownOptions o)
        : sequence(std::move(seq)), feed(f), options(std::move(o)),
          channels(sequence->masterFormat == ChannelFormat::Surround51 ? 6 : out), outputChannels(out), rate{sampleRate, 1},
          tpf(rate.ticksPerUnit()), start(from), routed(usesSubmixRouting(*sequence)),
          mixer(static_cast<int>(sequence->audioTracks.size()) + (routed ? detail::SubmixGraph::kMaxSubmixes : 0), channels,
                sampleRate, kBlockFrames),
          master(channels, sampleRate) {
        const int ch = channels;
        if (outputChannels != channels) { // A.9: a 5.1 master rendered to stereo (or mono): ITU-R BS.775
            downmix = formatMatrix(ChannelFormat::Surround51, channels, formatForChannels(outputChannels), outputChannels);
            // Review A5-P2-2: the soft clip guard runs on what is delivered, once, after the downmix (it sums up to 2.41
            // times what a guard before it let through, and a second guard would bend loud samples twice).
            guardAfterDownmix = mixer.softClipEnabled();
            mixer.setSoftClipEnabled(false);
        }
        const auto& tracks = sequence->audioTracks;
        const auto selected = [&](const Track& t) {
            return !options.tracks ||
                   std::find(options.tracks->begin(), options.tracks->end(), t.id) != options.tracks->end();
        };
        std::vector<TrackId> ids;
        for (const TrackPtr& t : tracks) {
            ids.push_back(t ? t->id : TrackId{});
        }
        if (routed) {
            // p02 audio: submixes and sends, as the player routes them; the faders then apply here (pre-fader sends).
            graph = std::make_unique<detail::SubmixGraph>(static_cast<int>(tracks.size()), ch, sampleRate, kBlockFrames);
            graph->update(sequence.get(), ids);
            if (options.honourMuteSolo) {
                graph->applyFlags(sequence.get(), ids, mixer);
            }
        }
        // Mute and solo: the player's rules (TimelineAudioPlayer::applyTrackFlags), or with routing the graph's
        // (SubmixGraph::applyFlags: a soloed submix sounds with what feeds it); an unselected track is muted on top.
        std::vector<bool> muted(tracks.size(), false);
        std::vector<bool> soloed(tracks.size(), false);
        bool anySolo = false;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const Track* t = tracks[i].get();
            const int index = static_cast<int>(i);
            if (routed && options.honourMuteSolo) {
                muted[i] = mixer.busMuted(index) || t == nullptr || !selected(*t);
                soloed[i] = mixer.busSoloed(index);
            } else {
                muted[i] = t == nullptr || !selected(*t) || (options.honourMuteSolo && (t->muted || !t->enabled));
                soloed[i] = options.honourMuteSolo && t != nullptr && t->solo;
            }
            mixer.setBusMuted(index, muted[i]);
            mixer.setBusSoloed(index, soloed[i]);
            anySolo = anySolo || (soloed[i] && t != nullptr && selected(*t));
        }
        if (graph) {
            for (int k = 0; k < detail::SubmixGraph::kMaxSubmixes; ++k) {
                anySolo = anySolo || mixer.busSoloed(graph->busOf(k));
            }
        }
        TrackRendererOptions rendererOptions = options.renderer;
        if (routed) {
            rendererOptions.trackFader = false;
        }
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            Bus bus;
            bus.track = tracks[i].get();
            bus.audible = !muted[i] && (!anySolo || soloed[i]);
            if (bus.audible) {
                bus.renderer = std::make_unique<TrackRenderer>(ch, rate, rendererOptions);
                bus.renderer->setRedactions(options.redactions);
                bus.renderer->setAudioTakes(options.takes);
                bus.renderer->setProject(options.project);
            }
            buses.push_back(std::move(bus));
        }
        const std::size_t blockSamples = static_cast<std::size_t>(kBlockFrames) * static_cast<std::size_t>(ch);
        busBuffer.assign(blockSamples, 0.0f);
        block.assign(blockSamples, 0.0f);
        scratch.assign(blockSamples, 0.0f);
        master.update(options.masterEffects ? sequence->audioEffects : EffectStack{},
                      options.masterEffects ? sequence->masterVolume : ParamValue{});
    }

    std::shared_ptr<const Sequence> sequence;
    AudioFeed& feed;
    const OfflineMixdownOptions options;
    const int channels;       ///< the mix: the master's format (6 for 5.1), else the output's
    const int outputChannels; ///< what render() hands out
    ChannelMatrix downmix;    ///< mix -> output when they differ
    bool guardAfterDownmix = false; ///< the master's soft clip guard runs after the downmix instead
    const Rate rate;
    const std::int64_t tpf;
    const Ticks start;
    const bool routed; ///< p02 audio: the sequence routes tracks through submixes
    audio::Mixer mixer;
    detail::MasterBus master;
    std::unique_ptr<detail::SubmixGraph> graph;
    std::vector<Bus> buses;
    std::vector<float> busBuffer;
    std::vector<float> block;
    std::vector<float> scratch;
    std::size_t blockPos = kBlockFrames; ///< frames of `block` already handed out (empty: the first read renders)
    std::int64_t blocksRendered = 0;
    Ticks readCursor;                    ///< timeline time the tracks are read at next (output + master latency)
    bool primed = false;
    TrackRenderStats stats;

    /// One mixer block of every audible track read at `at` into `out` (at most kBlockFrames), through the master.
    void mixAt(Ticks at, std::span<float> out) {
        const auto ch = static_cast<std::size_t>(channels);
        const std::size_t n = out.size() / ch;
        mixer.beginBlock(out);
        const bool active = graph && graph->active();
        if (active) {
            graph->beginBlock(at, static_cast<std::int64_t>(n));
        }
        for (std::size_t i = 0; i < buses.size(); ++i) {
            Bus& bus = buses[i];
            if (!bus.audible) {
                continue; // the mixer would silence it anyway
            }
            const std::span<float> buffer(busBuffer.data(), n * ch);
            bus.renderer->render(*bus.track, at, buffer, feed, stats);
            if (routed) {
                // As the player's callback: pre-fader sends, the fader on the grid from timeline 0, post-fader sends,
                // then the track's output.
                const int slot = static_cast<int>(i);
                if (active) {
                    graph->sendPre(slot, buffer.data(), mixer);
                }
                detail::applyTimelineFader(bus.track->volume, bus.track->pan, at, static_cast<std::int64_t>(n), buffer.data(),
                                           channels, tpf);
                if (active) {
                    graph->sendPost(slot, buffer.data(), mixer);
                    if (const std::span<float> to = graph->outputOf(slot); !to.empty()) {
                        mixer.addBusTo(slot, buffer, channels, to);
                        continue;
                    }
                }
            }
            mixer.addBus(static_cast<int>(i), buffer, channels);
        }
        if (active) {
            graph->finishBlock(mixer, out);
        }
        mixer.endBlock(&master);
    }

    void renderBlock() {
        const auto ch = static_cast<std::size_t>(channels);
        master.beginRender();
        if (graph) {
            graph->beginRender();
        }
        if (!primed) {
            // Like the player at a start or seek: the master chain (and the submix paths, p02) restart at `start` and
            // are primed with their latency's worth of mix from there.
            const int latency = master.latencyFrames() + (graph ? graph->latencyFrames() : 0);
            master.restart(start, tpf);
            if (graph) {
                graph->restart(start, tpf);
            }
            for (std::int64_t done = 0; done < latency;) {
                const std::int64_t n = std::min<std::int64_t>(latency - done, kBlockFrames);
                mixAt(start + Ticks{done * tpf}, std::span<float>(scratch.data(), static_cast<std::size_t>(n) * ch));
                done += n;
            }
            readCursor = start + Ticks{latency * tpf};
            primed = true;
        }
        mixAt(readCursor, block);
        readCursor += Ticks{kBlockFrames * tpf};
        if (graph) {
            graph->endRender();
        }
        master.endRender();
        blockPos = 0;
        ++blocksRendered;
    }
};

OfflineMixdown::OfflineMixdown(std::shared_ptr<const Sequence> sequence, int channels, int sampleRate, Ticks start,
                               AudioFeed& feed, OfflineMixdownOptions options)
    : m_channels(std::max(channels, 1)) {
    const OfflineScope offline; // the master's processors are created here
    m_impl = std::make_unique<Impl>(std::move(sequence), m_channels, sampleRate, start, feed, std::move(options));
}

OfflineMixdown::~OfflineMixdown() = default;

Ticks OfflineMixdown::position() const noexcept {
    const Impl& d = *m_impl;
    const std::int64_t frames = d.blocksRendered * kBlockFrames - (kBlockFrames - static_cast<std::int64_t>(d.blockPos));
    return d.start + Ticks{std::max<std::int64_t>(frames, 0) * d.tpf};
}

void OfflineMixdown::render(std::span<float> interleaved) {
    const OfflineScope offline;                     // clip and track processors are created while rendering
    const audio::rt::ScopedDenormalsOff denormals; // review A5-P2-9
    Impl& d = *m_impl;
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto mix = static_cast<std::size_t>(d.channels);
    std::size_t done = 0;
    const std::size_t frames = interleaved.size() / ch;
    while (done < frames) {
        if (d.blockPos == static_cast<std::size_t>(kBlockFrames)) {
            d.renderBlock();
        }
        const std::size_t n = std::min(frames - done, static_cast<std::size_t>(kBlockFrames) - d.blockPos);
        if (mix == ch) {
            std::memcpy(interleaved.data() + done * ch, d.block.data() + d.blockPos * ch, n * ch * sizeof(float));
        } else {
            float* out = interleaved.data() + done * ch;
            d.downmix.apply(d.block.data() + d.blockPos * mix, out, n);
            if (d.guardAfterDownmix) {
                for (std::size_t s = 0; s < n * ch; ++s) {
                    out[s] = audio::softClip(out[s]);
                }
            }
        }
        d.blockPos += n;
        done += n;
    }
}

const TrackRenderStats& OfflineMixdown::stats() const noexcept { return m_impl->stats; }

} // namespace wabcut::playback
