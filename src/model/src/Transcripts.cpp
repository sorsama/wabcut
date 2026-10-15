// The project's side of transcripts (Transcripts.h): lookups, the speaker palette and validation.

#include <wabcut/model/Transcripts.h>

#include <wabcut/model/CaptionText.h>

#include <algorithm>
#include <array>

namespace wabcut {

SpeakerId TranscriptEdits::speakerOfLabel(std::int32_t label) const {
    const auto it = std::lower_bound(labels.begin(), labels.end(), label,
                                     [](const SpeakerLabel& l, std::int32_t v) { return l.label < v; });
    return it != labels.end() && it->label == label ? it->speaker : SpeakerId{};
}

SpeakerId TranscriptEdits::speakerAt(Ticks t) const {
    const auto it = std::upper_bound(speakerSpans.begin(), speakerSpans.end(), t,
                                     [](Ticks v, const SpeakerSpan& s) { return v < s.source.start; });
    if (it == speakerSpans.begin()) {
        return {};
    }
    const SpeakerSpan& span = *(it - 1);
    return span.source.contains(t) ? span.speaker : SpeakerId{};
}

std::uint32_t defaultSpeakerColor(std::size_t n) {
    // Distinct on dark and light backgrounds, in an order where neighbours differ.
    static constexpr std::array<std::uint32_t, 8> kPalette{0x4F8EF7, 0xF2994A, 0x27AE60, 0xEB5757,
                                                           0x9B51E0, 0xF2C94C, 0x2D9CDB, 0xBB6BD9};
    return kPalette[n % kPalette.size()];
}

std::optional<std::string> transcriptEditsProblem(const TranscriptEdits& e) {
    if (e.audioStream < 0) {
        return "have a negative audio stream";
    }
    if (e.labels.size() > kMaxTranscriptEditEntries || e.speakerSpans.size() > kMaxTranscriptEditEntries ||
        e.corrections.size() > kMaxTranscriptEditEntries) {
        return "have more than " + std::to_string(kMaxTranscriptEditEntries) + " entries of a kind";
    }
    for (std::size_t i = 0; i < e.labels.size(); ++i) {
        if (i > 0 && e.labels[i - 1].label >= e.labels[i].label) {
            return "have speaker labels out of order or twice";
        }
        if (!e.labels[i].speaker.isValid()) {
            return "map a label to no speaker";
        }
    }
    for (std::size_t i = 0; i < e.speakerSpans.size(); ++i) {
        const SpeakerSpan& s = e.speakerSpans[i];
        if (s.source.duration.value <= 0 || s.source.start.value < 0 || !s.speaker.isValid()) {
            return "have an empty or negative speaker span, or one without a speaker";
        }
        if (i > 0 && e.speakerSpans[i - 1].source.end() > s.source.start) {
            return "have speaker spans that overlap or are out of order";
        }
    }
    for (std::size_t i = 0; i < e.corrections.size(); ++i) {
        const WordCorrection& c = e.corrections[i];
        if (c.source.duration.value <= 0 || c.source.start.value < 0) {
            return "have an empty or negative correction";
        }
        if (i > 0 && e.corrections[i - 1].source.end() > c.source.start) {
            return "have corrections that overlap or are out of order";
        }
        if (c.text.size() > kMaxCorrectionTextBytes || c.original.size() > kMaxCorrectionTextBytes ||
            !isValidCaptionText(c.text) || !isValidCaptionText(c.original)) {
            return "have a correction that is not UTF-8 text up to " + std::to_string(kMaxCorrectionTextBytes) + " bytes";
        }
    }
    return std::nullopt;
}

} // namespace wabcut
