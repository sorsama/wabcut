#include <wabcut/playback/video/CaptionRedaction.h>

#include <wabcut/model/Masks.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/Caption.h>

#include <QString>

#include <algorithm>

namespace wabcut::playback {

namespace {

bool overlapsAny(std::span<const TimeRange> ranges, Ticks start, Ticks end) { return overlapsRedactedSpeech(ranges, start, end); }

} // namespace

std::string redactedCueText(CaptionTrackId track, const CaptionCue& cue, std::span<const TimeRange> ranges,
                            const CaptionWordTimings* words, std::string_view token) {
    if (ranges.empty() || !overlapsAny(ranges, cue.start, cue.end())) {
        return cue.text;
    }
    const std::vector<std::pair<int, int>> spans = render::captionWordSpans(cue.text);
    const std::vector<CaptionWordTiming>* timing = nullptr;
    if (words) {
        if (const auto it = words->cues.find({track.value, cue.start.value});
            it != words->cues.end() && it->second.size() == spans.size()) {
            timing = &it->second;
        }
    }
    if (!timing) {
        return std::string(token);
    }
    const QString replacement = QString::fromUtf8(token.data(), static_cast<qsizetype>(token.size()));
    QString text = QString::fromStdString(cue.text);
    for (std::size_t i = spans.size(); i-- > 0;) { // back to front: earlier offsets stay valid
        const CaptionWordTiming& w = (*timing)[i];
        if (overlapsAny(ranges, w.start, std::max(w.end, w.start + Ticks{1}))) {
            text.replace(spans[i].first, spans[i].second - spans[i].first, replacement);
        }
    }
    return text.toStdString();
}

CaptionTrack redactCaptionTrack(const CaptionTrack& track, std::span<const TimeRange> ranges,
                                const CaptionWordTimings* words, std::string_view token) {
    CaptionTrack out = track;
    for (CaptionCue& cue : out.cues) {
        cue.text = redactedCueText(track.id, cue, ranges, words, token);
    }
    return out;
}

} // namespace wabcut::playback
