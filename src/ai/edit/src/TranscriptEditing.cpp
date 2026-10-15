// Transcript edits applied on top of cached transcripts (TranscriptEditing.h).

#include <wabcut/ai/edit/TranscriptEditing.h>

#include <wabcut/ai/edit/CaptionBuilder.h>
#include <wabcut/model/CaptionText.h>

#include <algorithm>

namespace wabcut::ai::edit {

namespace {

Ticks middle(const TranscriptWord& w) { return w.t0 + Ticks{(w.t1 - w.t0).value / 2}; }

/// The correction's text as words: split at spaces (an unspaced language without spaces stays one word).
std::vector<std::string> tokens(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
    return out;
}

/// New words spanning [t0, t1), sharing it by display length.
std::vector<TranscriptWord> spread(const std::vector<std::string>& texts, Ticks t0, Ticks t1, const TranscriptWord* model) {
    std::vector<TranscriptWord> out;
    std::size_t total = 0;
    for (const std::string& t : texts) {
        total += std::max<std::size_t>(1, captionDisplayLength(t));
    }
    std::size_t done = 0;
    for (const std::string& text : texts) {
        TranscriptWord w;
        if (model) {
            w.speakerId = model->speakerId;
        }
        w.text = text;
        w.confidence = 1.0f;
        w.edited = true;
        w.approved = true;
        const std::size_t length = std::max<std::size_t>(1, captionDisplayLength(text));
        const auto at = [&](std::size_t share) {
            return t0 + Ticks{mulDiv((t1 - t0).value, static_cast<std::int64_t>(share), static_cast<std::int64_t>(total),
                                     Rounding::Floor)
                                  .value_or(0)};
        };
        w.t0 = at(done);
        done += length;
        w.t1 = at(done);
        out.push_back(std::move(w));
    }
    return out;
}

/// Segments with their times kept and their word ranges recomputed: each word belongs to the last segment that
/// starts at or before it (the first segment for words before it).
void rebuildSegments(Transcript& t) {
    if (t.segments.empty()) {
        return;
    }
    std::vector<std::size_t> counts(t.segments.size(), 0);
    std::size_t s = 0;
    for (const TranscriptWord& w : t.words) {
        while (s + 1 < t.segments.size() && t.segments[s + 1].t0 <= w.t0) {
            ++s;
        }
        ++counts[s];
    }
    std::vector<TranscriptSegment> out;
    std::size_t first = 0;
    for (std::size_t k = 0; k < t.segments.size(); ++k) {
        if (counts[k] == 0) {
            continue;
        }
        TranscriptSegment seg = t.segments[k];
        seg.firstWord = first;
        seg.wordCount = counts[k];
        std::vector<std::string> words;
        for (std::size_t i = first; i < first + counts[k]; ++i) {
            words.push_back(t.words[i].text);
            seg.t0 = std::min(seg.t0, t.words[i].t0);
            seg.t1 = std::max(seg.t1, t.words[i].t1);
        }
        seg.text = joinWords(words, t.language);
        out.push_back(std::move(seg));
        first += counts[k];
    }
    t.segments = std::move(out);
}

} // namespace

std::string_view toString(CorrectionOutcome outcome) {
    switch (outcome) {
    case CorrectionOutcome::Applied:
        return "applied";
    case CorrectionOutcome::Conflict:
        return "conflict";
    case CorrectionOutcome::Restored:
        return "restored";
    }
    return "applied";
}

Transcript applyCorrections(const Transcript& transcript, const TranscriptEdits& edits,
                            std::vector<CorrectionReport>* report) {
    if (edits.corrections.empty()) {
        return transcript;
    }
    Transcript out = transcript;
    std::vector<TranscriptWord> words;
    words.reserve(out.words.size());
    std::size_t i = 0;
    for (std::size_t k = 0; k < edits.corrections.size(); ++k) {
        const WordCorrection& c = edits.corrections[k];
        // Words before the correction stay.
        while (i < out.words.size() && middle(out.words[i]) < c.source.start) {
            words.push_back(out.words[i++]);
        }
        std::vector<const TranscriptWord*> matched;
        while (i < out.words.size() && c.source.contains(middle(out.words[i]))) {
            matched.push_back(&out.words[i++]);
        }
        const std::vector<std::string> fixed = tokens(c.text);
        CorrectionReport r{k, CorrectionOutcome::Applied, {}};
        if (matched.empty()) {
            r.outcome = CorrectionOutcome::Restored;
            const auto inserted = spread(fixed, c.source.start, c.source.end(), nullptr);
            words.insert(words.end(), inserted.begin(), inserted.end());
        } else {
            std::vector<std::string> found;
            for (const TranscriptWord* w : matched) {
                found.push_back(w->text);
            }
            r.found = joinWords(found, out.language);
            const bool same = flattenCaptionText(r.found, out.language) == flattenCaptionText(c.original, out.language);
            r.outcome = same ? CorrectionOutcome::Applied : CorrectionOutcome::Conflict;
            if (fixed.size() == matched.size()) {
                for (std::size_t m = 0; m < matched.size(); ++m) {
                    TranscriptWord w = *matched[m];
                    w.text = fixed[m];
                    w.edited = true;
                    w.approved = true;
                    words.push_back(std::move(w));
                }
            } else if (!fixed.empty()) {
                const auto replaced = spread(fixed, matched.front()->t0, matched.back()->t1, matched.front());
                words.insert(words.end(), replaced.begin(), replaced.end());
            }
        }
        if (report) {
            report->push_back(std::move(r));
        }
    }
    while (i < out.words.size()) {
        words.push_back(out.words[i++]);
    }
    std::stable_sort(words.begin(), words.end(), [](const TranscriptWord& a, const TranscriptWord& b) { return a.t0 < b.t0; });
    out.words = std::move(words);
    rebuildSegments(out);
    return out;
}

SpeakerId speakerOf(const TranscriptWord& word, const TranscriptEdits* edits) {
    if (!edits) {
        return {};
    }
    if (const SpeakerId assigned = edits->speakerAt(middle(word)); assigned.isValid()) {
        return assigned;
    }
    return word.speakerId ? edits->speakerOfLabel(*word.speakerId) : SpeakerId{};
}

} // namespace wabcut::ai::edit
