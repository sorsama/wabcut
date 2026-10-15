#include <wabcut/audio/edit/MediaAnalysis.h>

#include <wabcut/audio/dsp/Loudness.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/MediaAudioFeed.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace wabcut::audio::edit {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kModelVersion = "ebur128-1.2.6+wabcut-dsp-1";

// ---- Feeds ---------------------------------------------------------------------------------------------------

/// MediaAudioFeed reads locators through an EditSession; this keeps one for the state being analysed.
class SessionFeed final : public playback::AudioFeed {
public:
    explicit SessionFeed(const ProjectState& state) : m_session(state), m_feed(m_session) {}
    std::expected<std::size_t, Error> read(MediaId media, int streamOrdinal, Ticks sourceTime, std::span<float> interleaved,
                                           int channels, Rate rate) override {
        return m_feed.read(media, streamOrdinal, sourceTime, interleaved, channels, rate);
    }

private:
    EditSession m_session;
    playback::MediaAudioFeed m_feed; // after m_session: it reads the session
};

// ---- What is rendered ------------------------------------------------------------------------------------------

struct Scoped {
    std::shared_ptr<const Sequence> sequence;
    TimeRange range;
    playback::OfflineMixdownOptions options;
};

/// A track heard on its own: mute, solo and output do not apply.
std::shared_ptr<Track> alone(const Track& track) {
    auto t = std::make_shared<Track>(track);
    t->muted = false;
    t->solo = false;
    t->enabled = true;
    return t;
}

std::expected<Scoped, Error> scope(const ProjectState& state, SequenceId id, const LoudnessTarget& target,
                                   std::optional<TimeRange> range) {
    const SequencePtr seq = state.sequencePtr(id);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    Scoped out;
    auto copy = std::make_shared<Sequence>(*seq);
    copy->videoTracks.clear();
    TimeRange extent;
    switch (target.scope) {
    case LoudnessTarget::Scope::Clip: {
        const auto location = seq->findClip(target.clip);
        if (!location || location->track->kind != TrackKind::Audio) {
            return std::unexpected(Error{ErrorCode::NotFound, "clip " + std::to_string(target.clip.value) + " is not an audio clip of the sequence"});
        }
        Track only;
        only.id = location->track->id;
        only.kind = TrackKind::Audio;
        only.clips.push_back(location->clip());
        copy->audioTracks = {std::make_shared<const Track>(std::move(only))};
        copy->audioEffects.clear();
        extent = location->clip().timelineRange;
        out.options.honourMuteSolo = false;
        out.options.masterEffects = false;
        break;
    }
    case LoudnessTarget::Scope::Track: {
        const Track* track = seq->findTrack(target.track);
        if (track == nullptr || track->kind != TrackKind::Audio) {
            return std::unexpected(Error{ErrorCode::NotFound, "track " + std::to_string(target.track.value) + " is not an audio track"});
        }
        copy->audioTracks = {alone(*track)};
        copy->audioEffects.clear();
        extent = track->clips.empty() ? TimeRange{}
                                      : TimeRange::fromStartEnd(track->clips.front().start(), track->clips.back().end());
        out.options.honourMuteSolo = false;
        out.options.masterEffects = false;
        break;
    }
    case LoudnessTarget::Scope::Mix:
        extent = TimeRange::fromStartEnd(Ticks{}, seq->contentEnd());
        break;
    }
    if (range) {
        const Ticks start = std::max(extent.start, range->start);
        const Ticks end = std::min(extent.end(), range->end());
        extent = end > start ? TimeRange::fromStartEnd(start, end) : TimeRange{start, Ticks{}};
    }
    out.sequence = std::move(copy);
    out.range = extent;
    // Review P2-10: measure what export renders, the project's redaction mutes and bleeps included.
    out.options.redactions = playback::AudioRedactions::of(state);
    // A.4: and the enhanced takes clips with Enhance Speech play.
    out.options.takes = playback::AudioTakes::of(state);
    // C.8: and the mixes of nested and multicam clips. Borrowed: a scope is rendered while `state` is alive.
    out.options.project = std::shared_ptr<const ProjectState>(std::shared_ptr<const ProjectState>{}, &state);
    return out;
}

// ---- Cache fingerprint -------------------------------------------------------------------------------------------

/// FNV-1a over a canonical description of everything that changes the rendered audio (never ids).
class Hasher {
public:
    void bytes(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            m_h = (m_h ^ p[i]) * 0x100000001b3ULL;
        }
    }
    void add(std::string_view s) {
        add(static_cast<std::int64_t>(s.size()));
        bytes(s.data(), s.size());
    }
    void add(std::int64_t v) { bytes(&v, sizeof v); }
    void add(double v) { add(static_cast<std::int64_t>(std::bit_cast<std::uint64_t>(v))); }
    void add(Ticks t) { add(t.value); }
    void add(const ParamValue& v) {
        if (const Curve* c = v.curve()) {
            add(std::int64_t{1});
            for (const Keyframe& k : c->keys) {
                add(k.time);
                add(k.value);
                add(static_cast<std::int64_t>(k.interpolation));
                add(k.in.slope);
                add(k.in.influence);
                add(k.out.slope);
                add(k.out.influence);
            }
        } else {
            add(std::int64_t{0});
            add(v.constant());
        }
    }
    void add(const EffectStack& stack) {
        add(static_cast<std::int64_t>(stack.size()));
        for (const EffectInstance& e : stack) {
            add(e.effect);
            add(std::int64_t{e.version});
            add(std::int64_t{e.enabled ? 1 : 0});
            for (const EffectParam& p : e.params) {
                add(p.key);
                add(p.value);
            }
            // A.6: a plugin effect's state lives in its data. Hashed only when there is some, so other keys stay as
            // they were.
            if (!e.data.empty()) {
                add(std::int64_t{0xDA7A});
                for (const EffectData& d : e.data) {
                    add(d.key);
                    if (const std::string* text = d.text()) {
                        add(*text);
                    } else if (const auto* points = d.points()) {
                        for (const EffectPoint& pt : *points) {
                            add(pt.x);
                            add(pt.y);
                        }
                    }
                }
            }
        }
    }
    void add(const ProjectState& state, const Clip& c) {
        if (const MediaItem* m = state.findMedia(c.media); m && m->locator.fingerprint.isKnown()) {
            add(m->locator.fingerprint.size);
            add(m->locator.fingerprint.sampleHash);
        } else if (m) {
            add(m->locator.path);
        }
        add(std::int64_t{c.stream.index});
        add(c.timelineRange.start);
        add(c.timelineRange.duration);
        add(c.sourceIn);
        add(std::int64_t{c.speed.num});
        add(std::int64_t{c.speed.den});
        add(std::int64_t{(c.reversed ? 2 : 0) + (c.enabled ? 1 : 0)});
        add(c.properties()[ParamId::VolumeDb]);
        add(c.properties()[ParamId::Pan]);
        add(c.effects());
        // Review P2-23: a remap curve decides which source plays and where the clip is silent. Hashed only when there
        // is one, so the keys of other clips (and their cached results) stay as they were.
        if (const Curve* remap = c.timeRemap()) {
            add(std::int64_t{0x7E3A9});
            add(ParamValue(*remap));
        }
        // C.8: a nested or multicam clip plays its sequence's mix, so what that mix is made of: the tracks it reads, the
        // nested master stage, the length. Hashed only for such clips, so other keys stay as they were.
        if (c.isNested()) {
            add(std::int64_t{0xC8});
            add(static_cast<std::int64_t>(c.angles.bits));
            const Sequence* nested = m_depth < kMaxNestingDepth ? state.findSequence(c.sequence) : nullptr;
            if (nested == nullptr) {
                add(std::int64_t{-1});
                return;
            }
            ++m_depth;
            add(nested->duration);
            add(std::int64_t{nested->multicam ? 1 : 0});
            for (const std::size_t i : nestedTracksRead(*nested, c)) {
                add(static_cast<std::int64_t>(i));
                add(state, *nested->audioTracks[i]);
            }
            if (!nested->multicam) {
                add(nested->audioEffects);
                add(nested->masterVolume);
            }
            --m_depth;
        }
    }
    void add(const ProjectState& state, const Track& t) {
        add(std::int64_t{(t.muted ? 4 : 0) + (t.solo ? 2 : 0) + (t.enabled ? 1 : 0)});
        add(static_cast<std::int64_t>(t.clips.size()));
        for (const Clip& c : t.clips) {
            add(state, c);
        }
        for (const Transition& tr : t.transitions) {
            add(tr.outgoing.isValid() ? static_cast<std::int64_t>(t.indexOf(tr.outgoing).value_or(9999)) : -1);
            add(tr.incoming.isValid() ? static_cast<std::int64_t>(t.indexOf(tr.incoming).value_or(9999)) : -1);
            add(tr.before);
            add(tr.after);
            add(tr.effect);
        }
        add(t.effects);
        if (!(t.volume == ParamValue()) || !(t.pan == ParamValue())) { // A.3 fader; unchanged keys at 0 dB / centre
            add(std::int64_t{0xFADE});
            add(t.volume);
            add(t.pan);
        }
    }
    std::uint64_t value() const { return m_h; }

private:
    std::uint64_t m_h = 0xcbf29ce484222325ULL;
    std::size_t m_depth = 0; ///< nested sequences being hashed
};

std::uint64_t fingerprintOf(const ProjectState& state, const Scoped& s, const LoudnessTarget& target, int channels,
                            int rate) {
    Hasher h;
    h.add(std::int64_t{static_cast<int>(target.scope)});
    h.add(std::int64_t{channels});
    h.add(std::int64_t{rate});
    h.add(s.range.start);
    h.add(s.range.duration);
    h.add(std::int64_t{(s.options.honourMuteSolo ? 2 : 0) + (s.options.masterEffects ? 1 : 0)});
    for (const TrackPtr& t : s.sequence->audioTracks) {
        h.add(state, *t);
    }
    if (s.options.masterEffects) {
        h.add(s.sequence->audioEffects);
        if (!(s.sequence->masterVolume == ParamValue())) { // A.3 master fader
            h.add(std::int64_t{0xFADE});
            h.add(s.sequence->masterVolume);
        }
    }
    // The media the scope plays, nested ones included (C.8).
    std::vector<MediaId> heard;
    for (const TrackPtr& t : s.sequence->audioTracks) {
        const std::vector<MediaId> media = playback::mediaHeardOn(state, *t);
        heard.insert(heard.end(), media.begin(), media.end());
    }
    std::sort(heard.begin(), heard.end());
    // The redactions of the media the scope plays (review P2-10): a new bleep is new audio. Ranges are anchored at
    // redaction roots: a subclip or stem heard hears its master's or source's (review wave 5).
    if (s.options.redactions) {
        std::vector<MediaId> heardRoots;
        for (const MediaId m : heard) {
            heardRoots.push_back(s.options.redactions->rootOf(m));
        }
        std::sort(heardRoots.begin(), heardRoots.end());
        for (const playback::AudioRedactions::Range& r : s.options.redactions->ranges()) {
            const bool used = std::binary_search(heardRoots.begin(), heardRoots.end(), r.media);
            if (!used) {
                continue;
            }
            h.add(static_cast<std::int64_t>(r.media.value));
            h.add(r.source.start);
            h.add(r.source.duration);
            h.add(std::int64_t{static_cast<int>(r.mode)});
            for (const ClipId c : r.exceptions) {
                h.add(static_cast<std::int64_t>(c.value));
            }
        }
    }
    // A.4: the takes of the media the scope plays (a new render is new audio). Hashed only when there are any, so
    // existing cache entries stay valid.
    if (s.options.takes) {
        for (const playback::AudioTakes::Take& t : s.options.takes->takes()) {
            const bool used = std::binary_search(heard.begin(), heard.end(), t.media);
            if (!used) {
                continue;
            }
            h.add(std::int64_t{0x7A4E});
            h.add(static_cast<std::int64_t>(t.media.value));
            h.add(std::int64_t{t.stream});
            h.add(t.kind);
            h.add(t.path);
            h.add(static_cast<std::int64_t>(t.generation));
        }
    }
    return h.value();
}

Json level(double v) { return std::isfinite(v) ? Json(v) : Json(nullptr); }
double level(const Json& j) { return j.is_number() ? j.get<double>() : -std::numeric_limits<double>::infinity(); }

Json toJson(const LoudnessStats& s) {
    return Json{{"integratedLufs", level(s.integratedLufs)}, {"rangeLu", s.rangeLu}, {"truePeakDbtp", level(s.truePeakDbtp)},
                {"samplePeakDbfs", level(s.samplePeakDbfs)}, {"maxMomentaryLufs", level(s.maxMomentaryLufs)},
                {"maxShortTermLufs", level(s.maxShortTermLufs)}, {"durationTicks", s.duration.value}};
}

std::optional<LoudnessStats> fromJson(std::string_view text) {
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::nullopt;
    }
    LoudnessStats s;
    s.integratedLufs = level(j.value("integratedLufs", Json()));
    s.rangeLu = j.value("rangeLu", 0.0);
    s.truePeakDbtp = level(j.value("truePeakDbtp", Json()));
    s.samplePeakDbfs = level(j.value("samplePeakDbfs", Json()));
    s.maxMomentaryLufs = level(j.value("maxMomentaryLufs", Json()));
    s.maxShortTermLufs = level(j.value("maxShortTermLufs", Json()));
    s.duration = Ticks{j.value("durationTicks", std::int64_t{0})};
    return s;
}

std::optional<Error> stopRequested(command::ExecutionControl* control, std::string_view what) {
    if (control == nullptr) {
        return std::nullopt;
    }
    if (auto stop = control->checkpoint(what)) {
        return Error{ErrorCode::Cancelled, stop->message};
    }
    return std::nullopt;
}

int rateOf(const Sequence& seq, const RenderAnalysisOptions& o) {
    if (o.sampleRate > 0) {
        return o.sampleRate;
    }
    return seq.audioRate.isExact() && seq.audioRate.den == 1 ? seq.audioRate.num : 48000;
}

/// Renders `s` over its range in blocks, calling `sink` with each block. Stops (an Error) when asked to.
template <class Sink>
std::optional<Error> renderScoped(const Scoped& s, playback::AudioFeed& feed, int channels, int rate,
                                  command::ExecutionControl* control, std::string_view what, Sink&& sink,
                                  const std::function<void(double)>& progress = {}) {
    playback::OfflineMixdown mix(s.sequence, channels, rate, s.range.start, feed, s.options);
    const std::int64_t tpf = Rate{rate, 1}.ticksPerUnit();
    const std::int64_t frames = s.range.duration.value / tpf;
    constexpr std::int64_t kBlock = 4800;
    std::vector<float> block(static_cast<std::size_t>(kBlock) * static_cast<std::size_t>(channels));
    for (std::int64_t done = 0; done < frames;) {
        const std::int64_t n = std::min(kBlock, frames - done);
        const std::span<float> view(block.data(), static_cast<std::size_t>(n) * static_cast<std::size_t>(channels));
        mix.render(view);
        sink(view, n);
        done += n;
        if (progress) {
            progress(static_cast<double>(done) / static_cast<double>(frames));
        }
        if (done % (kBlock * 10) == 0) {
            if (auto stop = stopRequested(control, what)) {
                return stop;
            }
        }
    }
    return std::nullopt;
}

} // namespace

FeedFactory mediaFeedFactory() {
    return [](const ProjectState& state) { return std::make_shared<SessionFeed>(state); };
}

// ---- Loudness ----------------------------------------------------------------------------------------------------

RenderLoudnessAnalyzer::RenderLoudnessAnalyzer(FeedFactory feeds, RenderAnalysisOptions options)
    : m_feeds(std::move(feeds)), m_options(std::move(options)) {}

std::expected<LoudnessStats, Error> RenderLoudnessAnalyzer::measure(const ProjectState& state, SequenceId sequence,
                                                                    const LoudnessTarget& target,
                                                                    std::optional<TimeRange> range,
                                                                    command::ExecutionControl* control) {
    return measure(state, sequence, target, range, control, {});
}

std::expected<LoudnessStats, Error> RenderLoudnessAnalyzer::measure(const ProjectState& state, SequenceId sequence,
                                                                    const LoudnessTarget& target,
                                                                    std::optional<TimeRange> range,
                                                                    command::ExecutionControl* control,
                                                                    const std::function<void(double)>& progress) {
    auto scoped = scope(state, sequence, target, range);
    if (!scoped) {
        return std::unexpected(scoped.error());
    }
    // A.9: a 5.1 master is measured in 5.1 (libebur128 weighs the surround channels, ignores the LFE), not downmixed.
    const int channels = scoped->sequence->masterFormat == ChannelFormat::Surround51 ? 6 : std::max(m_options.channels, 1);
    const int rate = rateOf(*scoped->sequence, m_options);
    std::optional<ai::AnalysisKey> key;
    if (m_options.cache) {
        static constexpr std::string_view kScopes[] = {"loudness.clip", "loudness.track", "loudness.mix"};
        key = ai::AnalysisKey{fingerprintOf(state, *scoped, target, channels, rate), 0,
                              std::string(kScopes[static_cast<int>(target.scope)]), std::string(kModelVersion), "v1"};
        if (const auto hit = m_options.cache->get(*key); hit && *hit) {
            if (const auto stats = fromJson(**hit)) {
                m_hits.fetch_add(1);
                return *stats;
            }
        }
    }
    LoudnessStats stats;
    stats.duration = scoped->range.duration;
    if (scoped->range.duration.value > 0) {
        const auto feed = m_feeds(state);
        dsp::LoudnessMeter meter(channels, rate);
        if (auto stop = renderScoped(
                *scoped, *feed, channels, rate, control, "measuring loudness",
                [&](std::span<const float> block, std::int64_t frames) { meter.add(block.data(), static_cast<std::size_t>(frames)); },
                progress)) {
            return std::unexpected(*stop);
        }
        m_renders.fetch_add(1);
        const dsp::LoudnessResult r = meter.result();
        stats.integratedLufs = r.integratedLufs;
        stats.rangeLu = r.rangeLu;
        stats.truePeakDbtp = r.truePeakDbtp;
        stats.samplePeakDbfs = r.samplePeakDbfs;
        stats.maxMomentaryLufs = r.maxMomentaryLufs;
        stats.maxShortTermLufs = r.maxShortTermLufs;
    }
    if (key) {
        [[maybe_unused]] const auto stored = m_options.cache->put(*key, toJson(stats).dump());
    }
    return stats;
}

// ---- Speech ------------------------------------------------------------------------------------------------------

std::vector<TimeRange> mergeRanges(std::vector<TimeRange> ranges, Ticks bridge) {
    std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> out;
    for (const TimeRange& r : ranges) {
        if (r.duration.value <= 0) {
            continue;
        }
        if (!out.empty() && r.start <= out.back().end() + bridge) {
            out.back() = TimeRange::fromStartEnd(out.back().start, std::max(out.back().end(), r.end()));
        } else {
            out.push_back(r);
        }
    }
    return out;
}

std::vector<TimeRange> speechFromLevels(std::span<const double> levels, Ticks start, Ticks frameTicks,
                                        const EnergySpeechOptions& o) {
    std::vector<double> audible;
    for (const double l : levels) {
        if (l > -100.0) {
            audible.push_back(l);
        }
    }
    if (audible.empty()) {
        return {};
    }
    // The noise floor: the 10th percentile of the frames that are not digital silence.
    const auto nth = audible.begin() + static_cast<std::ptrdiff_t>(audible.size() / 10);
    std::nth_element(audible.begin(), nth, audible.end());
    const double floor = *nth;
    // Capped at -30 dBFS, so continuous speech (whose "floor" is speech) is still found.
    const double on = std::clamp(floor + o.floorMarginDb, o.minimumDbfs, -30.0);
    const double off = on - o.releaseDb;
    std::vector<TimeRange> out;
    bool speaking = false;
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= levels.size(); ++i) {
        const double l = i < levels.size() ? levels[i] : -1e9;
        if (!speaking && l >= on) {
            speaking = true;
            begin = i;
        } else if (speaking && l < off) {
            speaking = false;
            out.push_back(TimeRange{start + Ticks{static_cast<std::int64_t>(begin) * frameTicks.value},
                                    Ticks{static_cast<std::int64_t>(i - begin) * frameTicks.value}});
        }
    }
    return mergeRanges(std::move(out), o.bridge);
}

RenderSpeechDetector::RenderSpeechDetector(FeedFactory feeds, std::shared_ptr<const ai::edit::TranscriptProvider> transcripts,
                                           EnergySpeechOptions energy, RenderAnalysisOptions options)
    : m_feeds(std::move(feeds)), m_transcripts(std::move(transcripts)), m_energy(energy), m_options(std::move(options)) {}

std::expected<std::vector<TimeRange>, Error> RenderSpeechDetector::speech(const ProjectState& state, SequenceId sequence,
                                                                          std::span<const TrackId> tracks,
                                                                          SpeechSource source, std::optional<TimeRange> range,
                                                                          command::ExecutionControl* control) {
    const SequencePtr seq = state.sequencePtr(sequence);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    const auto onTracks = [&](TrackId id) { return std::find(tracks.begin(), tracks.end(), id) != tracks.end(); };

    // Clips of the dialogue tracks, and which of them a transcript covers.
    std::vector<TimeRange> clipRanges;
    std::vector<TimeRange> covered;
    for (const TrackPtr& t : seq->audioTracks) {
        if (!onTracks(t->id)) {
            continue;
        }
        for (const Clip& c : t->clips) {
            if (!c.enabled) {
                continue;
            }
            clipRanges.push_back(c.timelineRange);
            const MediaItem* media = state.findMedia(c.media);
            if (m_transcripts && media != nullptr && m_transcripts->transcript(*media, c.stream.index)) {
                covered.push_back(c.timelineRange);
            }
        }
    }
    clipRanges = mergeRanges(std::move(clipRanges));
    covered = mergeRanges(std::move(covered));
    const auto overlapsAny = [](const std::vector<TimeRange>& list, const TimeRange& r) {
        return std::any_of(list.begin(), list.end(), [&](const TimeRange& x) { return x.overlaps(r); });
    };

    std::vector<TimeRange> found;
    if (source != SpeechSource::Energy && m_transcripts && !covered.empty()) {
        const ai::edit::SequenceTranscript words = ai::edit::SequenceTranscript::build(state, sequence, *m_transcripts);
        for (const ai::edit::TimelineWord& w : words.words()) {
            // A word counts when it plays where a dialogue track has audio (linked stereo pairs list a word once).
            if ((onTracks(w.track) || overlapsAny(clipRanges, w.timeline)) && (!range || w.timeline.overlaps(*range))) {
                found.push_back(w.timeline);
            }
        }
        found = mergeRanges(std::move(found), Ticks{kTicksPerSecond * 3 / 10});
    }
    if (source == SpeechSource::Transcript) {
        return found;
    }

    // The audio level of each dialogue track, where no transcript covers it.
    const int rate = rateOf(*seq, m_options);
    const int channels = std::max(m_options.channels, 1);
    const std::int64_t frameLength = rate / 100; // 10 ms
    const Ticks frameTicks{frameLength * Rate{rate, 1}.ticksPerUnit()};
    const auto feed = m_feeds(state);
    for (const TrackPtr& t : seq->audioTracks) {
        if (!onTracks(t->id) || t->clips.empty()) {
            continue;
        }
        auto scoped = scope(state, sequence, LoudnessTarget::ofTrack(t->id), range);
        if (!scoped) {
            return std::unexpected(scoped.error());
        }
        if (scoped->range.duration.value <= 0) {
            continue;
        }
        std::vector<double> levels;
        double sum = 0.0;
        std::int64_t counted = 0;
        if (auto stop = renderScoped(*scoped, *feed, channels, rate, control, "finding speech",
                                     [&](std::span<const float> block, std::int64_t frames) {
                                         for (std::int64_t f = 0; f < frames; ++f) {
                                             for (int c = 0; c < channels; ++c) {
                                                 const double v = block[static_cast<std::size_t>(f * channels + c)];
                                                 sum += v * v;
                                             }
                                             if (++counted == frameLength) {
                                                 const double ms = sum / static_cast<double>(frameLength * channels);
                                                 levels.push_back(ms > 1e-20 ? 10.0 * std::log10(ms) : -200.0);
                                                 sum = 0.0;
                                                 counted = 0;
                                             }
                                         }
                                     })) {
            return std::unexpected(*stop);
        }
        for (const TimeRange& r : speechFromLevels(levels, scoped->range.start, frameTicks, m_energy)) {
            if (source == SpeechSource::Auto && overlapsAny(covered, r)) {
                continue; // the transcript is the better judge there
            }
            found.push_back(r);
        }
    }
    return mergeRanges(std::move(found));
}

namespace {

/// A.9: per-channel peak and RMS of a stream, decoded at its own channel count.
class MediaChannelAnalyzer final : public ChannelAnalyzer {
public:
    std::expected<std::vector<ChannelLevel>, Error> measure(const ProjectState& state, const MediaItem& item, int streamOrdinal,
                                                             TimeRange sourceRange, command::ExecutionControl* control) override {
        if (streamOrdinal < 0 || static_cast<std::size_t>(streamOrdinal) >= item.info.audio.size()) {
            return std::unexpected(Error(ErrorCode::NotFound, "'" + item.name + "' has no audio stream " + std::to_string(streamOrdinal)));
        }
        const int channels = std::clamp(item.info.audio[static_cast<std::size_t>(streamOrdinal)].channels, 1, kMaxSourceChannels);
        constexpr int kRate = 48000;
        const Rate rate{kRate, 1};
        const Ticks length = std::min(sourceRange.duration, Ticks::fromSeconds(static_cast<std::int64_t>(kMaxSeconds)));
        const std::int64_t total = ticksToSamples(length, rate);
        SessionFeed feed(state);
        constexpr std::int64_t kBlock = 4096;
        std::vector<float> block(static_cast<std::size_t>(kBlock * channels));
        std::vector<double> peak(static_cast<std::size_t>(channels), 0.0);
        std::vector<double> squares(static_cast<std::size_t>(channels), 0.0);
        std::int64_t frames = 0;
        for (std::int64_t done = 0; done < total;) {
            const std::int64_t n = std::min(kBlock, total - done);
            const auto read = feed.read(item.id, streamOrdinal, sourceRange.start + samplesToTicks(done, rate),
                                        std::span<float>(block.data(), static_cast<std::size_t>(n * channels)), channels, rate);
            if (!read) {
                return std::unexpected(read.error());
            }
            const auto got = static_cast<std::int64_t>(std::min<std::size_t>(*read, static_cast<std::size_t>(n)));
            for (std::int64_t f = 0; f < got; ++f) {
                for (int c = 0; c < channels; ++c) {
                    const double v = block[static_cast<std::size_t>(f * channels + c)];
                    peak[static_cast<std::size_t>(c)] = std::max(peak[static_cast<std::size_t>(c)], std::fabs(v));
                    squares[static_cast<std::size_t>(c)] += v * v;
                }
            }
            frames += got;
            if (got < n) {
                break; // the media ended
            }
            done += n;
            if (control != nullptr) {
                if (auto stop = control->checkpoint("measuring channels")) {
                    return std::unexpected(Error(ErrorCode::Cancelled, stop->message));
                }
            }
        }
        const auto db = [](double linear) { return linear > 0.0 ? std::max(20.0 * std::log10(linear), -144.0) : -144.0; };
        std::vector<ChannelLevel> out;
        for (int c = 0; c < channels; ++c) {
            const double rms = frames > 0 ? std::sqrt(squares[static_cast<std::size_t>(c)] / static_cast<double>(frames)) : 0.0;
            out.push_back(ChannelLevel{db(peak[static_cast<std::size_t>(c)]), db(rms)});
        }
        return out;
    }
};

} // namespace

std::shared_ptr<ChannelAnalyzer> makeMediaChannelAnalyzer() { return std::make_shared<MediaChannelAnalyzer>(); }

std::shared_ptr<AudioServices> makeMediaAudioServices(std::shared_ptr<const ai::edit::TranscriptProvider> transcripts,
                                                      std::shared_ptr<ai::AnalysisCache> cache) {
    RenderAnalysisOptions options;
    options.cache = cache;
    auto services = std::make_shared<AudioServices>(std::make_shared<RenderLoudnessAnalyzer>(mediaFeedFactory(), options),
                                                    std::make_shared<RenderSpeechDetector>(mediaFeedFactory(), std::move(transcripts),
                                                                                           EnergySpeechOptions{}, options));
    services->music = makeMediaMusicAnalyzer(cache); // A.7
    services->channels = makeMediaChannelAnalyzer(); // A.9
    services->sync = makeMediaAudioSync(std::move(cache)); // A.5
    return services;
}

} // namespace wabcut::audio::edit
