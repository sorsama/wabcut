#include "NestedMix.h"

#include "Fader.h"

#include <wabcut/audio/TimeMath.h>

#include <wabcut/model/Resolve.h>

#include <algorithm>

namespace wabcut::playback::detail {

using audio::timemath::mulDivSaturate;

namespace {

bool hasRunnableEffects(const EffectStack& stack) {
    return std::any_of(stack.begin(), stack.end(), [](const EffectInstance& e) {
        return e.enabled && audio::isImplementedAudioEffect(e.effect) && isEvaluable(e, EffectKind::Audio);
    });
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

const EffectStack& noEffects() {
    static const EffectStack kNone;
    return kNone;
}

} // namespace

NestedMix::NestedMix(int channels, Rate rate, TrackRendererOptions options)
    : m_channels(channels), m_rate(rate), m_tpf(rate.ticksPerUnit()), m_options([&] {
          options.trackFader = true; // a nested sequence's tracks always get their faders (only the top level's are live)
          return options;
      }()),
      m_master(channels, static_cast<int>(rate.num / std::max(rate.den, 1))) {}

NestedMix::~NestedMix() = default;

NestedMix::Inner& NestedMix::innerFor(TrackId track) {
    for (Inner& inner : m_tracks) {
        if (inner.track == track) {
            return inner;
        }
    }
    Inner inner;
    inner.track = track;
    inner.renderer = std::make_unique<TrackRenderer>(m_channels, m_rate, m_options);
    m_tracks.push_back(std::move(inner));
    return m_tracks.back();
}

void NestedMix::configure(Inner& inner, const NestContext& context) {
    // What the level above set, handed down when it changes (the renderers copy shared pointers).
    if (inner.project != context.project->get()) {
        inner.project = context.project->get();
        inner.renderer->setProject(*context.project);
    }
    if (inner.redactions != context.redactions->get()) {
        inner.redactions = context.redactions->get();
        inner.renderer->setRedactions(*context.redactions);
    }
    if (inner.takes != context.takes->get()) {
        inner.takes = context.takes->get();
        inner.renderer->setAudioTakes(*context.takes);
    }
    if (!std::equal(inner.path.begin(), inner.path.end(), context.path.begin(), context.path.end())) {
        inner.path.assign(context.path.begin(), context.path.end());
        inner.renderer->setNesting(inner.path);
    }
}

void NestedMix::sumTracks(const NestContext& context, const Sequence& nested, const Clip& clip, std::int64_t g0,
                          std::int64_t n, float* dst, AudioFeed& feed, TrackRenderStats& stats) {
    for (Inner& inner : m_tracks) {
        inner.seen = false;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto samples = static_cast<std::size_t>(n) * ch;
    for (const std::size_t index : nestedTracksRead(nested, clip)) {
        const Track& track = *nested.audioTracks[index];
        Inner& inner = innerFor(track.id);
        inner.seen = true;
        configure(inner, context);
        m_track.resize(samples);
        inner.renderer->render(track, timeOf(g0), std::span<float>(m_track.data(), samples), feed, stats);
        for (std::size_t k = 0; k < samples; ++k) {
            dst[k] += m_track[k];
        }
    }
    // A track that stopped playing (muted, unsoloed, deleted, another angle) restarts if it plays again.
    std::erase_if(m_tracks, [](const Inner& inner) { return !inner.seen; });
}

void NestedMix::renderBlock(const NestContext& context, const Sequence& nested, const Clip& clip, std::int64_t f0,
                            std::int64_t n, float* out, AudioFeed& feed, TrackRenderStats& stats) {
    const auto ch = static_cast<std::size_t>(m_channels);
    std::fill(out, out + static_cast<std::size_t>(n) * ch, 0.0f);
    // The nested sequence's master stage; a multicam source has none (Nesting.h).
    const bool multicam = nested.multicam.has_value();
    if (m_master.bind(multicam ? noEffects() : nested.audioEffects) && m_masterInput != 0) {
        m_master.reset();
        m_masterInput = f0;
    }
    if (m_master.empty()) {
        sumTracks(context, nested, clip, f0, n, out, feed, stats);
        m_masterInput = f0 + n;
    } else {
        // Read the tracks ahead by the stack's latency, so what comes out for nested time t is the mix at t.
        const std::int64_t latency = m_master.latencyFrames();
        const std::int64_t inFrom = std::min(m_masterInput, f0 + latency);
        const std::int64_t inTo = f0 + n + latency;
        const auto count = static_cast<std::size_t>(inTo - inFrom);
        m_mix.assign(count * ch, 0.0f);
        sumTracks(context, nested, clip, inFrom, static_cast<std::int64_t>(count), m_mix.data(), feed, stats);
        m_master.process(m_mix.data(), static_cast<int>(count), inFrom, audio::CurveClock{m_origin, m_tpf, nullptr});
        m_masterInput = inTo;
        std::copy_n(m_mix.data() + (count - static_cast<std::size_t>(n)) * ch, static_cast<std::size_t>(n) * ch, out);
    }
    if (!multicam) {
        applyFader(nested.masterVolume, nullptr, f0, f0 + n, out, m_channels, [&](std::int64_t f) { return timeOf(f); });
    }
}

void NestedMix::restart(const NestContext& context, const Sequence& nested, const Clip& clip, Ticks from, AudioFeed& feed,
                        TrackRenderStats& stats) {
    for (Inner& inner : m_tracks) {
        inner.renderer->reset();
    }
    m_master.reset();
    m_masterInput = 0;
    m_started = true;
    m_sequence = nested.id;
    const bool settle = !nested.multicam && hasRunnableEffects(nested.audioEffects) && m_options.maxPreroll.value > 0;
    const std::int64_t preroll = settle ? mulDivSaturate(m_options.maxPreroll.value, 1, m_tpf, Rounding::Ceil) : 0;
    m_origin = from - Ticks{preroll * m_tpf};
    m_cursor = 0;
    constexpr std::int64_t kPiece = 4096;
    while (m_cursor < preroll) {
        const std::int64_t n = std::min(kPiece, preroll - m_cursor);
        m_discard.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(m_channels));
        renderBlock(context, nested, clip, m_cursor, n, m_discard.data(), feed, stats);
        m_cursor += n;
    }
}

void NestedMix::read(const NestContext& context, const Clip& clip, Ticks from, std::span<float> out, AudioFeed& feed,
                     TrackRenderStats& stats) {
    std::fill(out.begin(), out.end(), 0.0f);
    const std::int64_t frames = frameCount(out);
    const ProjectState* project = context.project != nullptr ? context.project->get() : nullptr;
    const Sequence* nested = project != nullptr ? project->findSequence(clip.sequence) : nullptr;
    if (frames <= 0 || nested == nullptr || context.path.size() > kMaxNestingDepth) {
        m_started = false;
        return;
    }
    if (!m_started || from != m_next || nested->id != m_sequence) {
        restart(context, *nested, clip, from, feed, stats);
    }
    renderBlock(context, *nested, clip, m_cursor, frames, out.data(), feed, stats);
    m_cursor += frames;
    m_next = from + Ticks{frames * m_tpf};
    // Nothing before the nested sequence's start or past its end (the clip may outlast a shortened sequence).
    const auto ch = static_cast<std::size_t>(m_channels);
    for (std::int64_t k = 0; k < frames; ++k) {
        const Ticks t = from + Ticks{k * m_tpf};
        if (t.value < 0 || t >= nested->duration) {
            std::fill_n(out.data() + static_cast<std::size_t>(k) * ch, ch, 0.0f);
        }
    }
}

void NestedMix::readReversed(const NestContext& context, const Clip& clip, Ticks anchor, Ticks from, std::span<float> out,
                             AudioFeed& feed, TrackRenderStats& stats) {
    std::fill(out.begin(), out.end(), 0.0f);
    const std::int64_t frames = frameCount(out);
    if (frames <= 0) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    const std::int64_t pieceFrames = std::max<std::int64_t>(mulDivSaturate(kReversePiece.value, 1, m_tpf, Rounding::Ceil), 1);
    const std::int64_t pieceTicks = pieceFrames * m_tpf;
    // The pieces lie on the reads' own frame grid (they are all a whole number of frames apart).
    const Ticks base = anchor + Ticks{(from - anchor).value - floorDiv((from - anchor).value, m_tpf) * m_tpf};
    for (std::int64_t k = 0; k < frames;) {
        const Ticks t = from + Ticks{k * m_tpf};
        const std::int64_t piece = floorDiv((t - base).value, pieceTicks);
        const Ticks pieceStart = base + Ticks{piece * pieceTicks};
        if (piece != m_piece || base != m_pieceBase) {
            m_pieceSamples.assign(static_cast<std::size_t>(pieceFrames) * ch, 0.0f);
            m_started = false; // every piece from its own restart
            read(context, clip, pieceStart, std::span<float>(m_pieceSamples), feed, stats);
            m_piece = piece;
            m_pieceBase = base;
        }
        const std::int64_t offset = (t - pieceStart).value / m_tpf;
        const std::int64_t n = std::min(frames - k, pieceFrames - offset);
        std::copy_n(m_pieceSamples.data() + static_cast<std::size_t>(offset) * ch, static_cast<std::size_t>(n) * ch,
                    out.data() + static_cast<std::size_t>(k) * ch);
        k += n;
    }
}

} // namespace wabcut::playback::detail
