#pragma once

// Transcripts for Timeline pictures (tests/model/TimelineFixture.h): words are written in SOURCE frames of a media
// letter at the fixture's 25 fps, and a SequenceTranscript is printed back as "text@start-end" in timeline frames,
// so a mapping test reads like the picture it maps.

#include "TimelineFixture.h"

#include <wabcut/ai/edit/SequenceTranscript.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::ai::edit::test {

using namespace wabcut::test;

struct W {
    std::string text;
    double from = 0; ///< source frames (fractions allowed)
    double to = 0;
    std::optional<int> speaker = std::nullopt;
    float confidence = 0.9f;
};

inline Ticks frames(double n) {
    return Ticks{static_cast<std::int64_t>(n * static_cast<double>(f(1).value) + (n < 0 ? -0.5 : 0.5))};
}

/// A transcript in `language` with these words (source frames).
inline std::shared_ptr<const Transcript> transcript(std::string language, std::vector<W> words) {
    auto t = std::make_shared<Transcript>();
    t->language = std::move(language);
    for (const W& w : words) {
        TranscriptWord tw;
        tw.text = w.text;
        tw.t0 = frames(w.from);
        tw.t1 = frames(w.to);
        tw.confidence = w.confidence;
        tw.speakerId = w.speaker;
        t->words.push_back(std::move(tw));
    }
    return t;
}

/// Transcripts of media letters (audio stream 0).
inline std::shared_ptr<MapTranscriptProvider>
provider(const Timeline& tl, std::initializer_list<std::pair<char, std::shared_ptr<const Transcript>>> transcripts) {
    auto p = std::make_shared<MapTranscriptProvider>();
    for (const auto& [letter, t] : transcripts) {
        p->add(tl.media(letter), 0, t);
    }
    return p;
}

/// "12" for a whole frame, "12.5" otherwise.
inline std::string frameText(Ticks t) {
    const std::int64_t frame = f(1).value;
    if (t.value % frame == 0) {
        return std::to_string(t.value / frame);
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", static_cast<double>(t.value) / static_cast<double>(frame));
    return text;
}

/// "Hello@0-4 world@8-12": every word with its timeline range in frames.
inline std::string layout(const SequenceTranscript& st) {
    std::string out;
    for (std::size_t i = 0; i < st.size(); ++i) {
        out += (out.empty() ? "" : " ") + std::string(st.text(i)) + "@" + frameText(st[i].timeline.start) + "-" +
               frameText(st[i].timeline.end());
    }
    return out;
}

/// Same words, ranges, clips and tracks.
inline bool sameWords(const SequenceTranscript& a, const SequenceTranscript& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const TimelineWord& x = a[i];
        const TimelineWord& y = b[i];
        if (x.timeline != y.timeline || x.source != y.source || x.clip != y.clip || x.track != y.track ||
            x.linkGroup != y.linkGroup || a.text(i) != b.text(i)) {
            return false;
        }
    }
    return true;
}

} // namespace wabcut::ai::edit::test
