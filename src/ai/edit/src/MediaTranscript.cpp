#include <wabcut/ai/edit/MediaTranscript.h>

#include <wabcut/ai/Text.h>
#include <wabcut/ai/WordBuilder.h>
#include <wabcut/ai/edit/TranscriptEditing.h>

#include <algorithm>

namespace wabcut::ai::edit {

namespace {

bool isAsciiAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

} // namespace

MediaTranscript MediaTranscript::build(const ProjectState& state, MediaId media, int audioStream,
                                       const TranscriptProvider& provider) {
    MediaTranscript out;
    out.m_media = media;
    out.m_audioStream = audioStream;
    const MediaItem* item = state.findMedia(media);
    if (!item || item->isGenerator() || audioStream < 0 || audioStream >= static_cast<int>(item->info.audio.size())) {
        return out;
    }
    auto t = provider.transcript(*item, audioStream);
    // The project's corrections on top of the cached words (C.7), as the sequence transcript reads them.
    if (const TranscriptEdits* edits = state.findTranscriptEdits(media, audioStream); t && edits && !edits->corrections.empty()) {
        t = std::make_shared<const Transcript>(applyCorrections(*t, *edits));
    }
    if (!t || t->words.empty()) {
        return out;
    }
    out.m_words.reserve(t->words.size());
    for (std::size_t i = 0; i < t->words.size(); ++i) {
        const TranscriptWord& w = t->words[i];
        MediaWord mw;
        mw.source = TimeRange::fromStartEnd(w.t0, std::max(w.t0, w.t1));
        mw.word = static_cast<std::uint32_t>(i);
        mw.speaker = w.speakerId;
        mw.confidence = w.confidence;
        // Any redaction of the media (no clip to except it).
        mw.redacted = isAudioRedacted(state, media, mw.source);
        out.m_words.push_back(mw);
    }
    // Transcripts are in time order; keep it so for a hand-made one.
    std::stable_sort(out.m_words.begin(), out.m_words.end(),
                     [](const MediaWord& a, const MediaWord& b) { return a.source.start < b.source.start; });
    out.m_transcript = std::move(t);
    return out;
}

std::string_view MediaTranscript::text(std::size_t i) const {
    const MediaWord& w = m_words[i];
    if (w.redacted) {
        return kRedactedWordText;
    }
    return m_transcript->words[w.word].text;
}

std::string_view MediaTranscript::language() const {
    return m_transcript ? std::string_view(m_transcript->language) : std::string_view();
}

std::string MediaTranscript::textOf(std::size_t first, std::size_t last) const {
    std::string out;
    const bool spaces = wordSplitFor(language()) == WordSplit::Whitespace;
    for (std::size_t i = first; i <= last && i < m_words.size(); ++i) {
        const std::string_view word = text(i);
        if (!out.empty() && !word.empty() && !text::isTrailingPunctuation(word) &&
            (spaces || (isAsciiAlnum(out.back()) && isAsciiAlnum(word.front())))) {
            out += ' ';
        }
        out += word;
    }
    return out;
}

std::pair<std::size_t, std::size_t> MediaTranscript::wordsIn(TimeRange range) const {
    const auto end = std::lower_bound(m_words.begin(), m_words.end(), range.end(),
                                      [](const MediaWord& w, Ticks t) { return w.source.start < t; });
    // Fix round 1: a zero-length word is in the range when its instant is (not dropped at the range's start).
    auto begin = m_words.begin();
    while (begin != end && (begin->source.isEmpty() ? begin->source.start < range.start : begin->source.end() <= range.start)) {
        ++begin;
    }
    return {static_cast<std::size_t>(begin - m_words.begin()), static_cast<std::size_t>(end - m_words.begin())};
}

std::optional<std::size_t> MediaTranscript::wordAt(Ticks t) const {
    // As SequenceTranscript::wordAt: the word containing t, else the nearest one; words are in start order.
    std::optional<std::size_t> best;
    Ticks bestDistance = Ticks::max();
    for (std::size_t i = 0; i < m_words.size(); ++i) {
        const TimeRange r = m_words[i].source;
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

bool MediaTranscript::sameAs(const MediaTranscript& other) const {
    if (m_media != other.m_media || m_audioStream != other.m_audioStream || m_words != other.m_words) {
        return false;
    }
    for (std::size_t i = 0; i < m_words.size(); ++i) {
        if (text(i) != other.text(i)) {
            return false;
        }
    }
    return language() == other.language();
}

} // namespace wabcut::ai::edit
