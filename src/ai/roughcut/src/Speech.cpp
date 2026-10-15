#include <wabcut/ai/roughcut/Speech.h>

#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace wabcut::ai::roughcut {

namespace {

double seconds(Ticks t) { return t.toSeconds(); }

bool endsWithTerminal(std::string_view word) {
    for (const std::string_view mark : {std::string_view("."), std::string_view("?"), std::string_view("!"), std::string_view("\xE2\x80\xA6"),
                                        std::string_view("\xE3\x80\x82"), std::string_view("\xEF\xBC\x9F"), std::string_view("\xEF\xBC\x81")}) {
        std::string_view w = word;
        while (!w.empty() && (w.back() == '"' || w.back() == '\'' || w.back() == ')')) {
            w.remove_suffix(1);
        }
        if (w.ends_with(mark)) {
            return true;
        }
    }
    return false;
}

/// The words' units, normalised to one string, for false-start checks.
std::string unitKey(const SpeechSource& source, std::size_t word) {
    std::string key;
    for (std::uint32_t u = source.unitStart[word]; u < source.unitStart[word + 1]; ++u) {
        key += source.units[u].text;
        key += ' ';
    }
    return key;
}

std::string plural(std::size_t n, std::string_view one, std::string_view many) {
    return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

} // namespace

AudioStats EnergyTakeProbe::stats(const MediaItem& media, int audioStream, TimeRange source) const {
    AudioStats out;
    if (m_energy) {
        out.rmsDb = m_energy->rmsDb(media, audioStream, source);
    }
    return out;
}

void EnergyTakeProbe::prefetch(std::span<const edit::AudioEnergyProbe::Range> ranges) const {
    if (m_energy) {
        m_energy->prefetch(ranges);
    }
}

void prepareSource(SpeechSource& source) {
    const edit::SequenceTranscript& st = source.transcript;
    const std::size_t n = st.size();
    source.filler.assign(n, 0);
    for (const edit::ReviewItem& item : source.review) {
        if (item.kind == edit::ReviewKind::Filler) {
            for (std::size_t w = item.firstWord; w <= item.lastWord && w < n; ++w) {
                source.filler[w] = 1;
            }
        }
    }
    source.units.clear();
    source.unitStart.assign(n + 1, 0);
    for (std::size_t w = 0; w < n; ++w) {
        source.unitStart[w] = static_cast<std::uint32_t>(source.units.size());
        appendUnits(source.units, st.text(w), static_cast<std::uint32_t>(w), source.filler[w] != 0);
    }
    source.unitStart[n] = static_cast<std::uint32_t>(source.units.size());
    source.sentences = shorts::splitSentences(st);
    std::vector<double> paces;
    for (const shorts::Sentence& s : source.sentences) {
        const double length = seconds(s.range.duration);
        const std::size_t words = s.last - s.first + 1;
        if (words >= 3 && length > 0.2) {
            paces.push_back(static_cast<double>(words) / length);
        }
    }
    if (!paces.empty()) {
        std::ranges::sort(paces);
        source.wordsPerSecond = paces[paces.size() / 2];
    }
    if (n > 0) {
        source.language = std::string(st.language(0));
    }
}

SpeechCorpus mediaCorpus(const ProjectState& state, std::span<const MediaId> media, const edit::TranscriptProvider& transcripts,
                         const edit::AudioEnergyProbe* energy, const CorpusOptions& options) {
    SpeechCorpus corpus;
    ProjectState scratch = state;
    std::vector<MediaId> ids(media.begin(), media.end());
    if (ids.empty()) {
        for (const MediaPtr& m : state.media) {
            if (m->info.hasAudio() && !m->isGenerator()) {
                ids.push_back(m->id);
            }
        }
    }
    std::set<std::uint64_t> seen;
    std::vector<std::string> missing;
    for (const MediaId id : ids) {
        if (!seen.insert(id.value).second) {
            continue;
        }
        const MediaItem* item = state.findMedia(id);
        if (!item) {
            corpus.warnings.push_back("media " + std::to_string(id.value) + " is not in the project");
            continue;
        }
        std::shared_ptr<const Transcript> transcript;
        int stream = 0;
        for (int s = 0; s < static_cast<int>(item->info.audio.size()); ++s) {
            if ((transcript = transcripts.transcript(*item, s))) {
                stream = s;
                break;
            }
        }
        if (!transcript || transcript->words.empty()) {
            missing.push_back(item->name);
            continue;
        }
        // A scratch sequence holding the whole stream at 0, so timeline time is source time.
        wabcut::edit::SequenceSettings settings;
        settings.name = "speech:" + item->name;
        settings.videoTracks = 0;
        settings.audioTracks = 1;
        auto created = wabcut::edit::addSequence(scratch, settings);
        if (!created) {
            corpus.warnings.push_back("'" + item->name + "': " + created.error().message);
            continue;
        }
        scratch = std::move(created->state);
        auto seq = std::make_shared<Sequence>(*scratch.findSequence(created->id));
        Ticks extent = item->sourceExtent(StreamRef{StreamKind::Audio, stream}).value_or(Ticks{});
        extent = std::max(extent, transcript->words.back().t1 + Ticks::fromSeconds(1));
        Clip clip;
        clip.id = scratch.ids.allocate<ClipId>();
        clip.media = item->id;
        clip.stream = StreamRef{StreamKind::Audio, stream};
        clip.timelineRange = TimeRange{Ticks{}, extent};
        clip.sourceIn = Ticks{};
        clip.setContent(item->defaultClipContent());
        auto track = std::make_shared<Track>(*seq->audioTracks.front());
        track->clips = ClipList(std::vector<Clip>{clip});
        seq->audioTracks.front() = std::move(track);
        seq->duration = extent;
        scratch = scratch.withSequence(std::move(seq));

        SpeechSource source;
        source.kind = SpeechSource::Kind::Media;
        source.media = item->id;
        source.audioStream = stream;
        source.sequence = created->id;
        source.name = item->name;
        source.transcript = edit::SequenceTranscript::build(scratch, created->id, transcripts);
        source.review = edit::review(source.transcript, scratch, options.review, energy);
        prepareSource(source);
        corpus.words += source.transcript.size();
        corpus.sources.push_back(std::move(source));
    }
    if (!missing.empty()) {
        std::string list;
        for (std::size_t i = 0; i < missing.size() && i < 5; ++i) {
            list += (i ? ", '" : "'") + missing[i] + "'";
        }
        corpus.warnings.push_back(plural(missing.size(), "media item has", "media items have") + " no transcript (" + list +
                                  (missing.size() > 5 ? ", ..." : "") + "): transcribe them to use their takes");
    }
    corpus.state = std::make_shared<const ProjectState>(std::move(scratch));
    return corpus;
}

void addSequenceSource(SpeechCorpus& corpus, const ProjectState& state, SequenceId sequence, const edit::TranscriptProvider& transcripts,
                       const edit::AudioEnergyProbe* energy, const CorpusOptions& options) {
    if (!corpus.state) {
        corpus.state = std::make_shared<const ProjectState>(state);
    }
    const Sequence* seq = corpus.state->findSequence(sequence);
    if (!seq) {
        corpus.warnings.push_back("sequence " + std::to_string(sequence.value) + " is not in the project");
        return;
    }
    SpeechSource source;
    source.kind = SpeechSource::Kind::Sequence;
    source.sequence = sequence;
    source.name = seq->name;
    source.transcript = edit::SequenceTranscript::build(*corpus.state, sequence, transcripts);
    if (source.transcript.empty()) {
        corpus.warnings.push_back("sequence '" + seq->name + "' has no transcript: transcribe its media first");
        return;
    }
    source.review = edit::review(source.transcript, *corpus.state, options.review, energy);
    prepareSource(source);
    corpus.words += source.transcript.size();
    corpus.sources.push_back(std::move(source));
}

TimeRange spanRange(const SpeechCorpus& corpus, const WordSpan& span) {
    const edit::SequenceTranscript& st = corpus.sources[span.source].transcript;
    const Ticks start = st[span.first].timeline.start;
    return TimeRange::fromStartEnd(start, std::max(start, st[span.last].timeline.end()));
}

std::string spanText(const SpeechCorpus& corpus, const WordSpan& span) {
    return corpus.sources[span.source].transcript.textOf(span.first, span.last);
}

std::optional<MediaRange> mediaRangeOf(const SpeechCorpus& corpus, const WordSpan& span) {
    const SpeechSource& source = corpus.sources[span.source];
    const edit::SequenceTranscript& st = source.transcript;
    if (source.kind == SpeechSource::Kind::Media) {
        const Ticks start = st[span.first].source.start;
        return MediaRange{source.media, source.audioStream, TimeRange::fromStartEnd(start, std::max(start, st[span.last].source.end()))};
    }
    const ClipId clip = st[span.first].clip;
    for (std::size_t w = span.first; w <= span.last; ++w) {
        if (st[w].clip != clip) {
            return std::nullopt;
        }
    }
    const Sequence* seq = corpus.state->findSequence(source.sequence);
    const auto location = seq ? seq->findClip(clip) : std::nullopt;
    if (!location) {
        return std::nullopt;
    }
    const Clip& c = location->clip();
    if (!c.speed.isUnity() || c.reversed || c.isRemapped()) {
        return std::nullopt;
    }
    const Ticks start = st[span.first].source.start;
    return MediaRange{c.media, c.stream.index, TimeRange::fromStartEnd(start, std::max(start, st[span.last].source.end()))};
}

std::optional<MediaRange> mediaRangeWithHandles(const SpeechCorpus& corpus, const WordSpan& span, Ticks handle) {
    std::optional<MediaRange> range = mediaRangeOf(corpus, span);
    if (!range) {
        return range;
    }
    const edit::SequenceTranscript& st = corpus.sources[span.source].transcript;
    // Neighbouring words of the same clip (Media sources: the same stream) bound the handles.
    const auto sameClip = [&](std::size_t a, std::size_t b) { return st[a].clip == st[b].clip; };
    Ticks before = std::min(handle, range->source.start);
    Ticks after = handle;
    if (span.first > 0 && sameClip(span.first - 1, span.first)) {
        const Ticks gap = st[span.first].source.start - st[span.first - 1].source.end();
        before = std::clamp(Ticks{gap.value / 2}, Ticks{}, before);
    }
    if (span.last + 1 < st.size() && sameClip(span.last, span.last + 1)) {
        const Ticks gap = st[span.last + 1].source.start - st[span.last].source.end();
        after = std::clamp(Ticks{gap.value / 2}, Ticks{}, handle);
    }
    range->source = TimeRange::fromStartEnd(range->source.start - before, range->source.end() + after);
    return range;
}

WordSpan wordsOfUnits(const SpeechSource& source, std::size_t sourceIndex, std::size_t beginUnit, std::size_t endUnit) {
    WordSpan span;
    span.source = sourceIndex;
    if (source.units.empty() || endUnit <= beginUnit) {
        return span;
    }
    span.first = source.units[std::min(beginUnit, source.units.size() - 1)].source;
    span.last = source.units[std::min(endUnit, source.units.size()) - 1].source;
    return span;
}

TakeQuality assessSpan(const SpeechCorpus& corpus, const WordSpan& span, double match, const CorpusOptions& options) {
    TakeQuality q;
    q.match = std::clamp(match, 0.0, 1.0);
    const SpeechSource& source = corpus.sources[span.source];
    const edit::SequenceTranscript& st = source.transcript;
    for (const edit::ReviewItem& item : source.review) {
        if (item.kind == edit::ReviewKind::Filler && item.firstWord >= span.first && item.lastWord <= span.last) {
            ++q.fillers;
        }
    }
    // A filler said right before or after the words (trimmed from the clip) still marks a hesitant take: "um, today
    // we...". Not one that starts the next sentence.
    const Ticks edge{kTicksPerSecond * 35 / 100};
    if (span.first > 0 && source.filler[span.first - 1] && st[span.first].timeline.start - st[span.first - 1].timeline.end() < edge) {
        ++q.fillers;
    }
    if (span.last + 1 < st.size() && source.filler[span.last + 1] && !endsWithTerminal(st.text(span.last)) &&
        st[span.last + 1].timeline.start - st[span.last].timeline.end() < edge) {
        ++q.fillers;
    }
    double confidence = 0.0;
    std::size_t spoken = 0;
    for (std::size_t w = span.first; w <= span.last; ++w) {
        confidence += st[w].confidence;
        if (!source.filler[w]) {
            ++spoken;
        }
        if (w > span.first && st[w].timeline.start - st[w - 1].timeline.end() >= options.hesitation) {
            ++q.hesitations;
        }
    }
    const std::size_t count = span.last - span.first + 1;
    q.confidence = confidence / static_cast<double>(count);
    // False starts: one to three words said twice at the start ("so so", "I think I think", "it cuts a, it cuts a"), or
    // a cut-off word ("I-").
    std::vector<std::string> keys;
    for (std::size_t w = span.first; w <= span.last && keys.size() < 8; ++w) {
        if (!source.filler[w]) {
            keys.push_back(unitKey(source, w));
            const std::string_view text = st.text(w);
            if (text.size() > 1 && text.back() == '-') {
                ++q.falseStarts;
            }
        }
    }
    for (std::size_t k = 0; k + 1 < keys.size();) {
        std::size_t repeated = 0;
        for (std::size_t n = 3; n >= 1 && repeated == 0; --n) {
            if (k + 2 * n <= keys.size() && std::equal(keys.begin() + static_cast<std::ptrdiff_t>(k), keys.begin() + static_cast<std::ptrdiff_t>(k + n),
                                                       keys.begin() + static_cast<std::ptrdiff_t>(k + n))) {
                repeated = n;
            }
        }
        if (repeated > 0) {
            ++q.falseStarts;
            k += repeated;
        } else {
            ++k;
        }
    }
    // A restart right before the words: "it cuts a, it cuts a whole video" (the take starts at the second attempt).
    for (std::size_t n = 1; n <= 3 && span.first >= n && span.first + n - 1 <= span.last; ++n) {
        bool same = st[span.first].timeline.start - st[span.first - 1].timeline.end() < edge;
        for (std::size_t k = 0; same && k < n; ++k) {
            same = unitKey(source, span.first - n + k) == unitKey(source, span.first + k);
        }
        if (same) {
            ++q.falseStarts;
            break;
        }
    }
    const Ticks sentenceGap{kTicksPerSecond / 2};
    q.startsSentence = span.first == 0 || endsWithTerminal(st.text(span.first - 1)) ||
                       st[span.first].timeline.start - st[span.first - 1].timeline.end() >= sentenceGap ||
                       st[span.first].speaker != st[span.first - 1].speaker;
    q.endsSentence = endsWithTerminal(st.text(span.last)) || span.last + 1 == st.size() ||
                     st[span.last + 1].timeline.start - st[span.last].timeline.end() >= Ticks{kTicksPerSecond * 7 / 10} ||
                     st[span.last + 1].speaker != st[span.last].speaker;
    const double length = seconds(spanRange(corpus, span).duration);
    q.wordsPerSecond = length > 0.05 ? static_cast<double>(spoken) / length : 0.0;
    return q;
}

std::vector<edit::AudioEnergyProbe::Range> audioRanges(const SpeechCorpus& corpus, const WordSpan& span) {
    std::vector<edit::AudioEnergyProbe::Range> out;
    const auto range = mediaRangeOf(corpus, span);
    if (!range) {
        return out;
    }
    const MediaItem* media = corpus.state->findMedia(range->media);
    if (!media) {
        return out;
    }
    out.push_back({media, range->audioStream, range->source});
    // The pauses near it, for the noise floor: at least 0.25 s, within 10 s, the six nearest.
    const SpeechSource& source = corpus.sources[span.source];
    const edit::SequenceTranscript& st = source.transcript;
    std::vector<std::pair<Ticks, TimeRange>> pauses;
    const Ticks minPause{kTicksPerSecond / 4};
    const Ticks reach = Ticks::fromSeconds(10);
    const TimeRange around = spanRange(corpus, span);
    for (std::size_t w = 1; w < st.size(); ++w) {
        const Ticks gapStart = st[w - 1].timeline.end();
        const Ticks gapEnd = st[w].timeline.start;
        if (gapEnd - gapStart < minPause || st[w].clip != st[w - 1].clip) {
            continue;
        }
        if (gapEnd < around.start - reach || gapStart > around.end() + reach) {
            continue;
        }
        // Inner 60 % of the pause (away from word edges), in media time.
        const Ticks srcStart = st[w - 1].source.end();
        const Ticks srcEnd = st[w].source.start;
        if (srcEnd <= srcStart) {
            continue;
        }
        const Ticks margin{(srcEnd - srcStart).value / 5};
        const Ticks distance = gapEnd < around.start ? around.start - gapEnd : (gapStart > around.end() ? gapStart - around.end() : Ticks{});
        pauses.emplace_back(distance, TimeRange::fromStartEnd(srcStart + margin, srcEnd - margin));
    }
    std::ranges::stable_sort(pauses, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < pauses.size() && i < 6; ++i) {
        out.push_back({media, range->audioStream, pauses[i].second});
    }
    return out;
}

void measureAudio(const SpeechCorpus& corpus, const WordSpan& span, const TakeAudioProbe& probe, TakeQuality& quality) {
    const std::vector<edit::AudioEnergyProbe::Range> ranges = audioRanges(corpus, span);
    if (ranges.empty()) {
        return;
    }
    const AudioStats speech = probe.stats(*ranges[0].media, ranges[0].audioStream, ranges[0].source);
    quality.rmsDb = speech.rmsDb;
    quality.peakDb = speech.peakDb;
    quality.clipped = speech.peakDb && *speech.peakDb >= -0.3;
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        const AudioStats pause = probe.stats(*ranges[i].media, ranges[i].audioStream, ranges[i].source);
        if (pause.rmsDb && std::isfinite(*pause.rmsDb) && (!quality.noiseDb || *pause.rmsDb < *quality.noiseDb)) {
            quality.noiseDb = pause.rmsDb;
        }
    }
}

void scoreTake(TakeQuality& q, std::optional<double> medianRmsDb, double usualWordsPerSecond) {
    q.reasons.clear();
    const double clean = 1.0 / (1.0 + static_cast<double>(q.fillers) + 0.5 * static_cast<double>(q.hesitations) +
                                static_cast<double>(q.falseStarts));
    double audio = 0.5;
    if (q.rmsDb && std::isfinite(*q.rmsDb)) {
        const double level = medianRmsDb ? std::clamp(0.5 + (*q.rmsDb - *medianRmsDb) / 12.0, 0.0, 1.0)
                                         : std::clamp((*q.rmsDb + 40.0) / 25.0, 0.0, 1.0);
        double clarity = level;
        if (q.noiseDb && std::isfinite(*q.noiseDb)) {
            clarity = std::clamp((*q.rmsDb - *q.noiseDb - 6.0) / 24.0, 0.0, 1.0);
        }
        audio = 0.5 * level + 0.5 * clarity;
    }
    if (q.clipped) {
        audio *= 0.3;
    }
    const double complete = (q.startsSentence ? 0.5 : 0.0) + (q.endsSentence ? 0.5 : 0.0);
    double pace = 0.5;
    if (usualWordsPerSecond > 0.0 && q.wordsPerSecond > 0.0) {
        pace = 1.0 - std::min(1.0, std::abs(q.wordsPerSecond - usualWordsPerSecond) / usualWordsPerSecond);
    }
    q.score = 0.40 * q.match + 0.20 * clean + 0.15 * audio + 0.15 * complete + 0.05 * std::clamp(q.confidence, 0.0, 1.0) + 0.05 * pace;

    if (q.fillers == 0 && q.hesitations == 0 && q.falseStarts == 0) {
        q.reasons.emplace_back("clean delivery");
    }
    if (q.fillers > 0) {
        q.reasons.push_back(plural(q.fillers, "filler", "fillers"));
    }
    if (q.falseStarts > 0) {
        q.reasons.push_back(plural(q.falseStarts, "false start", "false starts"));
    }
    if (q.hesitations > 0) {
        q.reasons.push_back(plural(q.hesitations, "long pause", "long pauses"));
    }
    if (!q.startsSentence || !q.endsSentence) {
        q.reasons.emplace_back(!q.startsSentence && !q.endsSentence ? "starts and ends mid-sentence"
                               : !q.startsSentence                  ? "starts mid-sentence"
                                                                    : "cut off at the end");
    }
    if (q.clipped) {
        q.reasons.emplace_back("clipped audio");
    }
    if (q.rmsDb && q.noiseDb && std::isfinite(*q.rmsDb) && std::isfinite(*q.noiseDb)) {
        char text[64];
        std::snprintf(text, sizeof text, "%.0f dB over the noise floor", *q.rmsDb - *q.noiseDb);
        q.reasons.emplace_back(text);
    }
    if (q.match < 0.6) {
        q.reasons.emplace_back("words differ from the script");
    }
}

} // namespace wabcut::ai::roughcut
