// Speakers and transcript edits (CaptionOps.h, Transcripts.h): project-level edits.

#include <wabcut/model/CaptionOps.h>

#include "SequenceEditor.h"

#include <algorithm>

namespace wabcut::edit {

namespace {

std::expected<void, Error> checkName(std::string_view name) {
    if (name.empty() || name.size() > kMaxSpeakerNameBytes || !isValidCaptionText(name) ||
        name.find_first_of("\n\t") != std::string_view::npos) {
        return std::unexpected(detail::invalidArgument("a speaker needs a one-line name of 1.." +
                                                       std::to_string(kMaxSpeakerNameBytes) + " bytes"));
    }
    return {};
}

std::expected<void, Error> checkColor(std::uint32_t color) {
    if (color > 0xFFFFFFu) {
        return std::unexpected(detail::invalidArgument("a speaker colour is 0xRRGGBB"));
    }
    return {};
}

std::expected<void, Error> checkStream(const ProjectState& state, MediaId media, std::int32_t audioStream) {
    const MediaItem* item = state.findMedia(media);
    if (!item) {
        return std::unexpected(detail::notFound("media " + std::to_string(media.value) + " does not exist"));
    }
    if (audioStream < 0 || static_cast<std::size_t>(audioStream) >= item->info.audio.size()) {
        return std::unexpected(detail::notFound("media " + std::to_string(media.value) + " has no audio stream " +
                                                std::to_string(audioStream)));
    }
    return {};
}

/// The edits record of a media stream, created (in order) when missing.
TranscriptEdits& editsFor(ProjectState& state, MediaId media, std::int32_t audioStream) {
    const auto key = std::pair{media, audioStream};
    const auto it = std::lower_bound(state.transcriptEdits.begin(), state.transcriptEdits.end(), key,
                                     [](const TranscriptEdits& e, const std::pair<MediaId, std::int32_t>& k) {
                                         return std::pair{e.media, e.audioStream} < k;
                                     });
    const auto index = static_cast<std::size_t>(it - state.transcriptEdits.begin());
    std::vector<TranscriptEdits>& edits = state.transcriptEdits.edit();
    if (it != state.transcriptEdits.end() && it->media == media && it->audioStream == audioStream) {
        return edits[index];
    }
    TranscriptEdits fresh;
    fresh.media = media;
    fresh.audioStream = audioStream;
    return *edits.insert(edits.begin() + static_cast<std::ptrdiff_t>(index), std::move(fresh));
}

void dropEmpty(ProjectState& state) {
    if (std::any_of(state.transcriptEdits.cbegin(), state.transcriptEdits.cend(), [](const TranscriptEdits& e) { return e.empty(); })) {
        std::erase_if(state.transcriptEdits.edit(), [](const TranscriptEdits& e) { return e.empty(); });
    }
}

/// Removes [range) from spans of the same kind, cutting those that cross its edges.
template <class Span, class Make>
void cutOut(std::vector<Span>& spans, TimeRange range, Make make) {
    std::vector<Span> out;
    for (const Span& s : spans) {
        const TimeRange r = s.source;
        if (r.end() <= range.start || r.start >= range.end()) {
            out.push_back(s);
            continue;
        }
        if (r.start < range.start) {
            out.push_back(make(s, TimeRange::fromStartEnd(r.start, range.start)));
        }
        if (r.end() > range.end()) {
            out.push_back(make(s, TimeRange::fromStartEnd(range.end(), r.end())));
        }
    }
    spans = std::move(out);
}

} // namespace

std::expected<Created<SpeakerId>, Error> addSpeaker(const ProjectState& state, std::string name,
                                                    std::optional<std::uint32_t> color) {
    if (auto ok = checkName(name); !ok) {
        return std::unexpected(ok.error());
    }
    if (state.speakers.size() >= kMaxSpeakers) {
        return std::unexpected(detail::invalidArgument("a project holds at most " + std::to_string(kMaxSpeakers) +
                                                       " speakers"));
    }
    const std::uint32_t c = color.value_or(defaultSpeakerColor(state.speakers.size()));
    if (auto ok = checkColor(c); !ok) {
        return std::unexpected(ok.error());
    }
    Created<SpeakerId> out{state, {}};
    out.id = out.state.ids.allocate<SpeakerId>();
    out.state.speakers.push_back(Speaker{out.id, std::move(name), c}); // ids only grow: still sorted
    return out;
}

std::expected<ProjectState, Error> registerSpeakerLabels(const ProjectState& state, MediaId media,
                                                         std::int32_t audioStream, std::span<const std::int32_t> labels) {
    if (auto ok = checkStream(state, media, audioStream); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<std::int32_t> missing;
    const TranscriptEdits* existing = state.findTranscriptEdits(media, audioStream);
    for (const std::int32_t label : labels) {
        if ((!existing || !existing->speakerOfLabel(label).isValid()) &&
            std::find(missing.begin(), missing.end(), label) == missing.end()) {
            missing.push_back(label);
        }
    }
    if (missing.empty()) {
        return state;
    }
    std::sort(missing.begin(), missing.end());
    ProjectState out = state;
    for (const std::int32_t label : missing) {
        auto added = addSpeaker(out, "Speaker " + std::to_string(out.speakers.size() + 1));
        if (!added) {
            return std::unexpected(added.error());
        }
        out = std::move(added->state);
        TranscriptEdits& e = editsFor(out, media, audioStream);
        const auto at = std::lower_bound(e.labels.begin(), e.labels.end(), label,
                                         [](const SpeakerLabel& l, std::int32_t v) { return l.label < v; });
        e.labels.insert(at, SpeakerLabel{label, added->id});
    }
    return out;
}

std::expected<ProjectState, Error> renameSpeaker(const ProjectState& state, SpeakerId speaker,
                                                 std::optional<std::string> name, std::optional<std::uint32_t> color) {
    if (!state.findSpeaker(speaker)) {
        return std::unexpected(detail::notFound("speaker " + std::to_string(speaker.value) + " does not exist"));
    }
    if (name) {
        if (auto ok = checkName(*name); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (color) {
        if (auto ok = checkColor(*color); !ok) {
            return std::unexpected(ok.error());
        }
    }
    ProjectState out = state;
    for (Speaker& s : out.speakers) {
        if (s.id == speaker) {
            s.name = name.value_or(s.name);
            s.color = color.value_or(s.color);
        }
    }
    return out;
}

std::expected<ProjectState, Error> mergeSpeakers(const ProjectState& state, SpeakerId from, SpeakerId into) {
    if (!state.findSpeaker(from) || !state.findSpeaker(into)) {
        return std::unexpected(detail::notFound("speaker " + std::to_string((state.findSpeaker(from) ? into : from).value) +
                                                " does not exist"));
    }
    if (from == into) {
        return std::unexpected(detail::invalidArgument("a speaker cannot merge into itself"));
    }
    ProjectState out = state;
    for (TranscriptEdits& e : out.transcriptEdits.edit()) {
        for (SpeakerLabel& l : e.labels) {
            l.speaker = l.speaker == from ? into : l.speaker;
        }
        for (SpeakerSpan& s : e.speakerSpans) {
            s.speaker = s.speaker == from ? into : s.speaker;
        }
    }
    // Caption cues of every sequence that names the speaker.
    for (const SequencePtr& seq : state.sequences) {
        bool uses = false;
        for (const CaptionTrack& t : seq->captionTracks) {
            uses = uses || std::any_of(t.cues.begin(), t.cues.end(), [&](const CaptionCue& c) { return c.speaker == from; });
        }
        if (!uses) {
            continue;
        }
        auto next = std::make_shared<Sequence>(*seq);
        for (CaptionTrack& t : next->captionTracks) {
            for (CaptionCue& c : t.cues) {
                c.speaker = c.speaker == from ? into : c.speaker;
            }
        }
        const IdGenerator ids = out.ids;
        out = out.withSequence(std::move(next));
        out.ids = ids;
    }
    std::erase_if(out.speakers, [&](const Speaker& s) { return s.id == from; });
    return out;
}

std::expected<ProjectState, Error> assignSpeaker(const ProjectState& state, MediaId media, std::int32_t audioStream,
                                                 TimeRange source, SpeakerId speaker) {
    if (auto ok = checkStream(state, media, audioStream); !ok) {
        return std::unexpected(ok.error());
    }
    if (source.duration.value <= 0 || source.start.value < 0) {
        return std::unexpected(detail::invalidArgument("a speaker is assigned over a non-empty range"));
    }
    if (speaker.isValid() && !state.findSpeaker(speaker)) {
        return std::unexpected(detail::notFound("speaker " + std::to_string(speaker.value) + " does not exist"));
    }
    ProjectState out = state;
    TranscriptEdits& e = editsFor(out, media, audioStream);
    if (e.speakerSpans.size() >= kMaxTranscriptEditEntries) {
        return std::unexpected(detail::invalidArgument("too many speaker assignments for this media"));
    }
    cutOut(e.speakerSpans, source, [](const SpeakerSpan& s, TimeRange r) { return SpeakerSpan{r, s.speaker}; });
    if (speaker.isValid()) {
        const auto at = std::lower_bound(e.speakerSpans.begin(), e.speakerSpans.end(), source.start,
                                         [](const SpeakerSpan& s, Ticks v) { return s.source.start < v; });
        e.speakerSpans.insert(at, SpeakerSpan{source, speaker});
    }
    dropEmpty(out);
    return out;
}

std::expected<ProjectState, Error> correctTranscript(const ProjectState& state, MediaId media,
                                                     std::int32_t audioStream, WordCorrection correction) {
    if (auto ok = checkStream(state, media, audioStream); !ok) {
        return std::unexpected(ok.error());
    }
    if (correction.source.duration.value <= 0 || correction.source.start.value < 0) {
        return std::unexpected(detail::invalidArgument("a correction covers a non-empty range of words"));
    }
    if (correction.text.size() > kMaxCorrectionTextBytes || correction.original.size() > kMaxCorrectionTextBytes ||
        !isValidCaptionText(correction.text) || !isValidCaptionText(correction.original)) {
        return std::unexpected(detail::invalidArgument("a correction is UTF-8 text of at most " +
                                                       std::to_string(kMaxCorrectionTextBytes) + " bytes"));
    }
    ProjectState out = state;
    TranscriptEdits& e = editsFor(out, media, audioStream);
    if (e.corrections.size() >= kMaxTranscriptEditEntries) {
        return std::unexpected(detail::invalidArgument("too many corrections for this media"));
    }
    std::erase_if(e.corrections, [&](const WordCorrection& c) { return c.source.overlaps(correction.source); });
    const auto at = std::lower_bound(e.corrections.begin(), e.corrections.end(), correction.source.start,
                                     [](const WordCorrection& c, Ticks v) { return c.source.start < v; });
    e.corrections.insert(at, std::move(correction));
    return out;
}

} // namespace wabcut::edit
