#include <wabcut/ai/TranscriptRedaction.h>

#include <wabcut/ai/Captions.h>
#include <wabcut/model/RedactionMedia.h>

#include <algorithm>

namespace wabcut::ai {

std::vector<TimeRange> audioRedactionRanges(const ProjectState& project, MediaId media) {
    std::vector<TimeRange> out;
    // A subclip or a stem shares its master's or source's redactions (model/RedactionMedia.h; review wave 5 A5-P1-3).
    for (const Redaction* covering : redactionsCovering(project, media)) {
        const Redaction& r = *covering;
        if (!r.enabled || r.audio == RedactionAudio::None) {
            continue;
        }
        if (r.audioRanges.empty()) {
            out.push_back(r.source);
        } else {
            out.insert(out.end(), r.audioRanges.begin(), r.audioRanges.end());
        }
    }
    std::erase_if(out, [](const TimeRange& t) { return t.duration.value <= 0; });
    std::sort(out.begin(), out.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
    std::vector<TimeRange> merged;
    for (const TimeRange& t : out) {
        if (!merged.empty() && t.start <= merged.back().end()) {
            const Ticks end = std::max(merged.back().end(), t.end());
            merged.back().duration = end - merged.back().start;
        } else {
            merged.push_back(t);
        }
    }
    return merged;
}

std::size_t redactTranscript(Transcript& transcript, std::span<const TimeRange> ranges) {
    if (ranges.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::vector<bool> changed(transcript.words.size(), false);
    for (std::size_t i = 0; i < transcript.words.size(); ++i) {
        TranscriptWord& w = transcript.words[i];
        // A word overlaps a range when they share time; a zero-length word counts when it lies inside one.
        const bool hit = std::any_of(ranges.begin(), ranges.end(), [&](const TimeRange& r) {
            return w.t1 > w.t0 ? (w.t0 < r.end() && r.start < w.t1) : r.contains(w.t0);
        });
        if (hit && w.text != kRedactedWord) {
            w.text = std::string(kRedactedWord);
            w.confidence = 0.0f;
            changed[i] = true;
            ++count;
        }
    }
    if (count > 0) {
        for (TranscriptSegment& s : transcript.segments) {
            const std::size_t end = std::min(s.firstWord + s.wordCount, transcript.words.size());
            if (std::any_of(changed.begin() + static_cast<std::ptrdiff_t>(std::min(s.firstWord, end)),
                            changed.begin() + static_cast<std::ptrdiff_t>(end), [](bool b) { return b; })) {
                s.text = segmentWordsText(transcript, s);
            }
        }
        transcript.provenance.params["redactedWords"] = std::to_string(
            std::count_if(transcript.words.begin(), transcript.words.end(), [](const TranscriptWord& w) { return w.text == kRedactedWord; }));
    }
    return count;
}

} // namespace wabcut::ai
