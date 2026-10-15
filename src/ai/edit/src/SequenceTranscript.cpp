#include <wabcut/ai/edit/SequenceTranscript.h>

#include <wabcut/ai/Text.h>
#include <wabcut/ai/WordBuilder.h>
#include <wabcut/ai/edit/TranscriptEditing.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/RedactionMedia.h>

#include <algorithm>
#include <tuple>
#include <unordered_set>

namespace wabcut::ai::edit {

namespace {

std::tuple<std::uint64_t, std::uint64_t, int> dedupeKey(LinkGroupId link, MediaId media, int stream) {
    return {link.value, media.value, stream};
}

/// How deep nested sequences are read through (validate() refuses cycles; this only bounds the work).
constexpr int kMaxReadDepth = 8;

/// The timeline range of source (or nested-sequence) range [s0, s1) through `clip`, when its midpoint is heard there:
/// inside the clip's source window, or a remapped clip's 100 % forward segments; clamped to where it plays.
std::optional<TimeRange> throughClip(const Clip& clip, Ticks s0, Ticks s1) {
    const Ticks mid = s0 + Ticks{(s1 - s0).value / 2};
    if (clip.isRemapped()) {
        for (const TimeRange r : clip.unitySpeedRanges()) {
            const Ticks base = clip.sourceTimeAt(r.start);
            if (mid >= base && mid < base + r.duration) {
                const Ticks a = std::clamp(r.start + (s0 - base), r.start, r.end());
                const Ticks b = std::clamp(r.start + (s1 - base), a, r.end());
                return TimeRange::fromStartEnd(a, b);
            }
        }
        return std::nullopt;
    }
    const Ticks sourceIn = clip.sourceIn;
    const Ticks sourceOut = clip.sourceOut();
    if (clip.isFreeze() || mid < sourceIn || mid >= sourceOut) {
        return std::nullopt;
    }
    const auto toTimeline = [&](Ticks s) {
        const Ticks offset = clip.reversed ? sourceOut - s : s - sourceIn;
        return clip.start() + Ticks{mulDiv(offset.value, clip.speed.den, clip.speed.num, Rounding::Nearest).value_or(0)};
    };
    Ticks a = toTimeline(s0);
    Ticks b = toTimeline(s1);
    if (b < a) {
        std::swap(a, b);
    }
    a = std::clamp(a, clip.start(), clip.end());
    b = std::clamp(b, a, clip.end());
    return TimeRange::fromStartEnd(a, b);
}

bool isAsciiAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// A redacted media range `r` covers word range `w` (an empty word: its instant).
bool covers(TimeRange r, TimeRange w) {
    if (r.isEmpty()) {
        return false;
    }
    return w.isEmpty() ? r.contains(w.start) : w.start < r.end() && r.start < w.end();
}

/// The enabled audio redactions of `state`, without their (unused here) areas, anchored at their redaction roots (a
/// subclip's or stem's marks its source: model/RedactionMedia.h).
std::vector<Redaction> audioRedactions(const ProjectState& state) {
    std::vector<Redaction> out;
    for (const Redaction& r : state.redactions) {
        if (r.enabled && r.audio != RedactionAudio::None) {
            out.push_back(r);
            out.back().region = Mask{};
            out.back().media = redactionRoot(state, r.media);
        }
    }
    return out;
}

bool hears(const Redaction& r, TimeRange source, std::optional<ClipId> clip) {
    if (!r.enabled || r.audio == RedactionAudio::None || (clip && !r.appliesTo(*clip))) {
        return false;
    }
    return r.audioRanges.empty() ? covers(r.source, source)
                                 : std::ranges::any_of(r.audioRanges, [&](TimeRange a) { return covers(a, source); });
}

} // namespace

bool isAudioRedacted(std::span<const Redaction> redactions, MediaId media, TimeRange source, std::optional<ClipId> clip) {
    return std::ranges::any_of(redactions, [&](const Redaction& r) { return r.media == media && hears(r, source, clip); });
}

bool isAudioRedacted(const ProjectState& state, MediaId media, TimeRange source, std::optional<ClipId> clip) {
    return std::ranges::any_of(redactionsCovering(state, media), [&](const Redaction* r) { return hears(*r, source, clip); });
}

// --- MapTranscriptProvider ---------------------------------------------------------------------------------

void MapTranscriptProvider::add(MediaId media, int audioStream, std::shared_ptr<const Transcript> transcript) {
    m_transcripts[{media.value, audioStream}] = std::move(transcript);
}

std::shared_ptr<const Transcript> MapTranscriptProvider::transcript(const MediaItem& media, int audioStream) const {
    const auto it = m_transcripts.find({media.id.value, audioStream});
    return it == m_transcripts.end() ? nullptr : it->second;
}

// --- Building ----------------------------------------------------------------------------------------------

std::optional<std::uint32_t> SequenceTranscript::transcriptFor(const MediaItem& media, int audioStream,
                                                              const ProjectState& state,
                                                              const TranscriptProvider& provider) {
    const auto key = std::pair{media.id.value, audioStream};
    if (const auto it = m_transcriptIndex.find(key); it != m_transcriptIndex.end()) {
        return it->second;
    }
    std::optional<std::uint32_t> index;
    auto t = provider.transcript(media, audioStream);
    // The project's corrections on top of the cached words (C.7, TranscriptEditing.h).
    const TranscriptEdits* edits = state.findTranscriptEdits(media.id, audioStream);
    if (t && edits && !edits->corrections.empty()) {
        t = std::make_shared<const Transcript>(applyCorrections(*t, *edits));
    }
    if (t && !t->words.empty()) {
        Ticks longest{};
        for (const TranscriptWord& w : t->words) {
            longest = std::max(longest, w.t1 - w.t0);
        }
        index = static_cast<std::uint32_t>(m_transcripts.size());
        m_transcripts.push_back(std::move(t));
        m_longestWord.push_back(longest);
        m_edits.push_back(edits ? std::optional<TranscriptEdits>(*edits) : std::nullopt);
    }
    m_transcriptIndex.emplace(key, index);
    return index;
}

void SequenceTranscript::appendWords(const Clip& clip, const ClipEntry& e) {
    const Transcript& t = *m_transcripts[e.transcript];
    // The words whose middle lies in media [sourceIn, sourceOut), each at the timeline range `toTimeline` gives it
    // (clamped to [lo, hi]).
    const auto append = [&](Ticks sourceIn, Ticks sourceOut, Ticks lo, Ticks hi, const auto& toTimeline) {
        auto it = std::lower_bound(t.words.begin(), t.words.end(), sourceIn - m_longestWord[e.transcript],
                                   [](const TranscriptWord& w, Ticks v) { return w.t0 < v; });
        for (; it != t.words.end() && it->t0 < sourceOut; ++it) {
            const Ticks mid = it->t0 + Ticks{(it->t1 - it->t0).value / 2};
            if (mid < sourceIn || mid >= sourceOut) {
                continue; // trimmed away (or mostly)
            }
            Ticks a = toTimeline(it->t0);
            Ticks b = toTimeline(it->t1);
            if (b < a) {
                std::swap(a, b);
            }
            a = std::clamp(a, lo, hi);
            b = std::clamp(b, a, hi);
            appendWord(clip, e, static_cast<std::uint32_t>(it - t.words.begin()), TimeRange::fromStartEnd(a, b));
        }
    };
    if (clip.isRemapped()) {
        // A remapped clip is heard only where it plays its media forwards at exactly 100 % (Clip::unitySpeedRanges):
        // its words map there one to one; holds, ramps and reverse segments are silent and list no words.
        for (const TimeRange r : clip.unitySpeedRanges()) {
            const Ticks s0 = clip.sourceTimeAt(r.start);
            append(s0, s0 + r.duration, r.start, r.end(), [&](Ticks s) { return r.start + (s - s0); });
        }
        return;
    }
    const Ticks sourceIn = clip.sourceIn;
    const Ticks sourceOut = clip.sourceOut();
    // Source time -> timeline time through the clip (speed and direction; the nearest tick).
    append(sourceIn, sourceOut, clip.start(), clip.end(), [&](Ticks s) {
        const Ticks offset = e.reversed ? sourceOut - s : s - sourceIn;
        return clip.start() + Ticks{mulDiv(offset.value, e.speed.den, e.speed.num, Rounding::Nearest).value_or(0)};
    });
}

void SequenceTranscript::appendWord(const Clip& clip, const ClipEntry& e, std::uint32_t word, TimeRange timeline) {
    const TranscriptWord& tw = m_transcripts[e.transcript]->words[word];
    TimelineWord w;
    w.timeline = timeline;
    w.source = TimeRange::fromStartEnd(tw.t0, tw.t1);
    w.clip = clip.id;
    w.track = e.track;
    w.linkGroup = e.linkGroup;
    w.trackOrder = e.trackOrder;
    w.transcript = e.transcript;
    w.word = word;
    w.speaker = tw.speakerId;
    w.speakerId = speakerOf(tw, m_edits[e.transcript] ? &*m_edits[e.transcript] : nullptr);
    w.confidence = tw.confidence;
    w.redacted = !m_redactions.empty() && isAudioRedacted(m_redactions, e.redactionRoot, w.source, clip.id);
    m_words.push_back(w);
}

void SequenceTranscript::addNestedClip(const Clip& clip, const Track& track, std::uint16_t order, const ProjectState& state,
                                       const TranscriptProvider& provider) {
    const Sequence* inner = state.findSequence(clip.sequence);
    if (!inner || m_depth >= kMaxReadDepth) {
        return;
    }
    // The tracks the clip mixes: every unmuted audio track of a nest; a multicam clip only its active angles'.
    std::vector<TrackId> heard;
    for (const auto& t : inner->audioTracks) {
        if (t->muted) {
            continue;
        }
        if (inner->multicam) {
            const auto angle = multicamAngleOfTrack(*inner->multicam, t->id);
            if (!angle || !clip.angles.contains(*angle)) {
                continue;
            }
        }
        heard.push_back(t->id);
    }
    SequenceTranscript nested = buildTracks(state, clip.sequence, provider, &heard, m_depth + 1);
    ClipEntry e;
    e.nested = true;
    e.timeline = clip.timelineRange;
    e.sourceIn = clip.sourceIn;
    e.speed = clip.speed;
    e.reversed = clip.reversed;
    e.remapped = clip.isRemapped();
    e.track = track.id;
    e.trackOrder = order;
    e.linkGroup = clip.linkGroup;
    e.contributes = true;
    m_clips[clip.id.value] = e;
    m_readsNested = true;
    // The nested words' transcripts join this one's list (the same transcript once).
    std::vector<std::uint32_t> index(nested.m_transcripts.size());
    for (std::size_t i = 0; i < nested.m_transcripts.size(); ++i) {
        const auto same = std::find(m_transcripts.begin(), m_transcripts.end(), nested.m_transcripts[i]);
        if (same != m_transcripts.end()) {
            index[i] = static_cast<std::uint32_t>(same - m_transcripts.begin());
            continue;
        }
        index[i] = static_cast<std::uint32_t>(m_transcripts.size());
        m_transcripts.push_back(nested.m_transcripts[i]);
        m_longestWord.push_back(nested.m_longestWord[i]);
        m_edits.push_back(nested.m_edits[i]);
    }
    for (const TimelineWord& w : nested.m_words) {
        const auto at = throughClip(clip, w.timeline.start, w.timeline.end());
        if (!at) {
            continue; // trimmed away
        }
        TimelineWord out = w;
        out.timeline = *at;
        out.clip = clip.id;
        out.track = track.id;
        out.linkGroup = clip.linkGroup;
        out.trackOrder = order;
        out.transcript = index[w.transcript];
        m_words.push_back(out);
    }
}

void SequenceTranscript::addClip(const Clip& clip, const Track& track, std::uint16_t order, const ProjectState& state,
                                 const TranscriptProvider& provider) {
    if (clip.isFreeze() || clip.stream.kind != StreamKind::Audio) {
        return;
    }
    if (clip.isNested()) {
        addNestedClip(clip, track, order, state, provider); // C.17: read through nested and multicam clips
        return;
    }
    const MediaItem* media = state.findMedia(clip.media);
    if (!media) {
        return;
    }
    const auto index = transcriptFor(*media, clip.stream.index, state, provider);
    if (!index) {
        return;
    }
    ClipEntry e;
    e.timeline = clip.timelineRange;
    e.sourceIn = clip.sourceIn;
    e.speed = clip.speed;
    e.reversed = clip.reversed;
    e.remapped = clip.isRemapped();
    e.media = clip.media;
    e.redactionRoot = redactionRoot(state, clip.media);
    e.audioStream = clip.stream.index;
    e.track = track.id;
    e.trackOrder = order;
    e.linkGroup = clip.linkGroup;
    e.transcript = *index;
    e.contributes = true;
    if (clip.linkGroup.isValid()) {
        // One link group hears a media stream once (a stereo pair split over two tracks).
        const auto key = dedupeKey(clip.linkGroup, clip.media, clip.stream.index);
        e.contributes = std::none_of(m_clips.begin(), m_clips.end(), [&](const auto& item) {
            const ClipEntry& o = item.second;
            return o.contributes && dedupeKey(o.linkGroup, o.media, o.audioStream) == key;
        });
    }
    m_clips[clip.id.value] = e;
    if (e.contributes) {
        appendWords(clip, e);
    }
}

void SequenceTranscript::sortWords() {
    std::sort(m_words.begin(), m_words.end(), [](const TimelineWord& a, const TimelineWord& b) {
        return std::tie(a.timeline.start, a.trackOrder, a.timeline.duration, a.transcript, a.word) <
               std::tie(b.timeline.start, b.trackOrder, b.timeline.duration, b.transcript, b.word);
    });
}

SequenceTranscript SequenceTranscript::build(const ProjectState& state, SequenceId sequence,
                                             const TranscriptProvider& provider) {
    return buildTracks(state, sequence, provider, nullptr, 0);
}

SequenceTranscript SequenceTranscript::buildTracks(const ProjectState& state, SequenceId sequence,
                                                   const TranscriptProvider& provider, const std::vector<TrackId>* only,
                                                   int depth) {
    SequenceTranscript st;
    st.m_sequence = sequence;
    st.m_depth = depth;
    st.m_redactions = audioRedactions(state);
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return st;
    }
    for (std::size_t order = 0; order < seq->audioTracks.size(); ++order) {
        const Track& track = *seq->audioTracks[order];
        if (only && std::find(only->begin(), only->end(), track.id) == only->end()) {
            continue;
        }
        for (const Clip& clip : track.clips) {
            st.addClip(clip, track, static_cast<std::uint16_t>(order), state, provider);
        }
    }
    st.sortWords();
    return st;
}

// --- Incremental update ----------------------------------------------------------------------------------

void SequenceTranscript::update(const ProjectState& next, const ChangeSet& changes, const TranscriptProvider& provider) {
    const Sequence* seq = next.findSequence(m_sequence);
    const auto rebuild = [&] { *this = build(next, m_sequence, provider); };
    if (!seq || !changes.modifiedMedia.empty() || !changes.removedMedia.empty() || changes.transcriptEditsChanged ||
        changes.speakersChanged || changes.redactionsChanged) {
        rebuild();
        return;
    }
    if (m_readsNested && (!changes.addedSequences.empty() || !changes.removedSequences.empty() ||
                          std::any_of(changes.sequences.begin(), changes.sequences.end(),
                                      [&](const SequenceChange& s) { return s.sequence != m_sequence; }))) {
        rebuild(); // C.17: a sequence it reads through may have changed
        return;
    }
    const SequenceChange* sc = changes.sequence(m_sequence);
    if (!sc) {
        return;
    }
    if (sc->trackListChanged || sc->roles.has(PropertyRole::Link)) {
        rebuild();
        return;
    }
    std::unordered_set<std::uint64_t> changed;
    for (const auto* list : {&sc->addedClips, &sc->removedClips, &sc->modifiedClips}) {
        for (const ClipId id : *list) {
            changed.insert(id.value);
        }
    }
    if (changed.empty()) {
        return;
    }
    // Where the changed clips are now (only the audio tracks the edit touched can hold them).
    struct Now {
        const Clip* clip;
        const Track* track;
        std::uint16_t order;
    };
    std::unordered_map<std::uint64_t, Now> now;
    for (const TrackChange& tc : sc->tracks) {
        const auto pos = seq->trackPosition(tc.track);
        if (!pos || pos->kind != TrackKind::Audio) {
            continue;
        }
        const Track& track = *seq->audioTracks[pos->index];
        for (const Clip& clip : track.clips) {
            if (changed.contains(clip.id.value)) {
                now.emplace(clip.id.value, Now{&clip, &track, static_cast<std::uint16_t>(pos->index)});
            }
        }
    }

    std::unordered_map<std::uint64_t, Ticks> shift; // clips that only moved
    std::unordered_set<std::uint64_t> drop;         // clips whose old words go
    std::vector<Now> add;                           // clips whose words are mapped (again)
    bool promote = false;
    for (const std::uint64_t id : changed) {
        const auto old = m_clips.find(id);
        const auto cur = now.find(id);
        if (old == m_clips.end()) {
            if (cur != now.end()) {
                add.push_back(cur->second); // new, or newly on an audio track
            }
            continue;
        }
        ClipEntry& e = old->second;
        if (cur == now.end()) {
            drop.insert(id);
            promote = promote || (e.contributes && e.linkGroup.isValid());
            m_clips.erase(old);
            continue;
        }
        const Clip& c = *cur->second.clip;
        // A remapped clip's words follow its curve: mapped again on any change (the entry does not keep the curve).
        const bool sameWindow = c.sourceIn == e.sourceIn && c.duration() == e.timeline.duration && c.speed == e.speed &&
                                c.reversed == e.reversed && !e.remapped && !c.isRemapped() && !e.nested && !c.isNested() &&
                                c.media == e.media &&
                                c.stream.index == e.audioStream && cur->second.track->id == e.track;
        if (sameWindow) {
            if (const Ticks delta = c.start() - e.timeline.start; delta.value != 0) {
                shift.emplace(id, delta);
                e.timeline = c.timelineRange;
            }
            continue;
        }
        drop.insert(id);
        const bool contributed = e.contributes;
        m_clips.erase(old);
        if (contributed) {
            add.push_back(cur->second);
        } else {
            promote = true; // re-evaluated below with the rest of its group
        }
    }
    if (!drop.empty() || !shift.empty()) {
        std::erase_if(m_words, [&](const TimelineWord& w) { return drop.contains(w.clip.value); });
        for (TimelineWord& w : m_words) {
            if (const auto it = shift.find(w.clip.value); it != shift.end()) {
                w.timeline.start += it->second;
            }
        }
    }
    for (const Now& n : add) {
        addClip(*n.clip, *n.track, n.order, next, provider);
    }
    if (promote) {
        // A group lost the clip it was heard through: another clip of the same stream takes over.
        for (auto& [id, e] : m_clips) {
            if (e.contributes || !e.linkGroup.isValid()) {
                continue;
            }
            const auto key = dedupeKey(e.linkGroup, e.media, e.audioStream);
            const bool covered = std::any_of(m_clips.begin(), m_clips.end(), [&](const auto& item) {
                return item.second.contributes && dedupeKey(item.second.linkGroup, item.second.media,
                                                            item.second.audioStream) == key;
            });
            if (!covered) {
                const Track* track = seq->findTrack(e.track);
                const Clip* clip = track ? track->findClip(ClipId{id}) : nullptr;
                if (clip) {
                    e.contributes = true;
                    appendWords(*clip, e);
                }
            }
        }
    }
    sortWords();
}

// --- Queries -------------------------------------------------------------------------------------------------

std::string_view SequenceTranscript::text(std::size_t i) const {
    const TimelineWord& w = m_words[i];
    if (w.redacted) {
        return kRedactedWordText;
    }
    return m_transcripts[w.transcript]->words[w.word].text;
}

std::string_view SequenceTranscript::language(std::size_t i) const {
    return m_transcripts[m_words[i].transcript]->language;
}

std::string SequenceTranscript::textOf(std::size_t first, std::size_t last) const {
    std::string out;
    for (std::size_t i = first; i <= last && i < m_words.size(); ++i) {
        const std::string_view word = text(i);
        const bool spaces = wordSplitFor(language(i)) == WordSplit::Whitespace;
        if (!out.empty() && !word.empty() && !text::isTrailingPunctuation(word) &&
            (spaces || (isAsciiAlnum(out.back()) && isAsciiAlnum(word.front())))) {
            out += ' ';
        }
        out += word;
    }
    return out;
}

std::pair<std::size_t, std::size_t> SequenceTranscript::wordsIn(TimeRange range) const {
    // Starts are sorted; a word that overlaps the range starts before its end.
    const auto end = std::lower_bound(m_words.begin(), m_words.end(), range.end(),
                                      [](const TimelineWord& w, Ticks t) { return w.timeline.start < t; });
    auto begin = m_words.begin();
    while (begin != end && begin->timeline.end() <= range.start) {
        ++begin;
    }
    return {static_cast<std::size_t>(begin - m_words.begin()), static_cast<std::size_t>(end - m_words.begin())};
}

std::optional<std::size_t> SequenceTranscript::wordAt(Ticks t) const {
    if (m_words.empty()) {
        return std::nullopt;
    }
    std::optional<std::size_t> best;
    Ticks bestDistance = Ticks::max();
    for (std::size_t i = 0; i < m_words.size(); ++i) {
        const TimeRange r = m_words[i].timeline;
        const Ticks distance = r.contains(t) ? Ticks{} : (t < r.start ? r.start - t : t - r.end());
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
        if (r.start > t && distance > bestDistance) {
            break;
        }
    }
    return best;
}

std::vector<ClipId> SequenceTranscript::clipsFor(std::size_t first, std::size_t last, const Sequence& sequence) const {
    std::unordered_set<std::uint64_t> clips;
    std::unordered_set<std::uint64_t> groups;
    for (std::size_t i = first; i <= last && i < m_words.size(); ++i) {
        clips.insert(m_words[i].clip.value);
        if (m_words[i].linkGroup.isValid()) {
            groups.insert(m_words[i].linkGroup.value);
        }
    }
    std::vector<ClipId> out;
    sequence.forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (clips.contains(c.id.value) || (c.linkGroup.isValid() && groups.contains(c.linkGroup.value))) {
                out.push_back(c.id);
            }
        }
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<TimeRange> SequenceTranscript::extent() const {
    std::optional<TimeRange> out;
    for (const auto& [id, e] : m_clips) {
        if (!e.contributes) {
            continue;
        }
        out = out ? TimeRange::fromStartEnd(std::min(out->start, e.timeline.start), std::max(out->end(), e.timeline.end()))
                  : e.timeline;
    }
    return out;
}

} // namespace wabcut::ai::edit
