#include <wabcut/ai/translate/CaptionTranslation.h>

#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/model/RedactedSpeech.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/CaptionText.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace wabcut::ai::translate {

using Json = nlohmann::json;

namespace {

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// Letters and digits only, lower-cased ASCII, for comparing a cue with the words under it.
std::string comparable(std::string_view s) {
    std::string out;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u)) {
            out.push_back(static_cast<char>(u >= 0x80 ? c : std::tolower(u)));
        }
    }
    return out;
}

std::size_t findText(std::string_view haystack, std::string_view needle, std::size_t from) {
    if (needle.empty()) {
        return std::string_view::npos;
    }
    if (const auto at = haystack.find(needle, from); at != std::string_view::npos) {
        return at;
    }
    const std::string h = lowerAscii(haystack);
    const std::string n = lowerAscii(needle);
    return h.find(n, from);
}

std::string truncatedUtf8(std::string_view text, std::size_t bytes) {
    if (text.size() <= bytes) {
        return std::string(text);
    }
    std::size_t cut = bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(text.substr(0, cut));
}

bool phraseSpaced(std::string_view language) {
    const std::string l = normalizeLanguage(language);
    return l == "th" || l == "lo" || l == "km" || l == "my";
}

} // namespace

std::string_view toString(Tone tone) {
    switch (tone) {
    case Tone::Casual:
        return "casual";
    case Tone::Formal:
        return "formal";
    case Tone::Playful:
        return "playful";
    case Tone::Neutral:
        break;
    }
    return "neutral";
}

std::optional<Tone> toneFromString(std::string_view text) {
    for (const Tone t : {Tone::Neutral, Tone::Casual, Tone::Formal, Tone::Playful}) {
        if (text == toString(t)) {
            return t;
        }
    }
    return std::nullopt;
}

// --- Source text ----------------------------------------------------------------------------------------------------

/// `accounted[c]`: every redacted word of cue c is known to be bleeped in texts[c] (the cue was made from the
/// transcript, or each redacted word's recognised spelling was found and replaced). Cues the transcript does not
/// account for are masked whole where redacted speech overlaps them (sourceTexts; review wave 5, A5-P1-4).
static SourceTexts collectSourceTexts(const ProjectState& state, SequenceId sequence, const CaptionTrack& track,
                                      const edit::TranscriptProvider* transcripts, std::vector<bool>& accounted) {
    SourceTexts out;
    accounted.assign(track.cues.size(), false);
    out.texts.reserve(track.cues.size());
    for (const CaptionCue& cue : track.cues) {
        out.texts.push_back(cue.text);
    }
    if (!transcripts || !state.findSequence(sequence)) {
        return out;
    }
    const edit::SequenceTranscript corrected = edit::SequenceTranscript::build(state, sequence, *transcripts);
    if (corrected.empty()) {
        return out;
    }
    // The same words as recognised: no corrections, no redactions.
    ProjectState rawState = state;
    rawState.redactions.clear();
    for (TranscriptEdits& edits : rawState.transcriptEdits) {
        edits.corrections.clear();
    }
    const edit::SequenceTranscript raw = edit::SequenceTranscript::build(rawState, sequence, *transcripts);
    // As recognised but with the redactions: how a cue made after a redaction and before a correction reads.
    ProjectState uncorrectedState = state;
    for (TranscriptEdits& edits : uncorrectedState.transcriptEdits) {
        edits.corrections.clear();
    }
    const edit::SequenceTranscript uncorrected = edit::SequenceTranscript::build(uncorrectedState, sequence, *transcripts);
    const auto midpointIn = [](const edit::TimelineWord& w, const CaptionCue& cue) {
        const Ticks mid = w.timeline.start + Ticks{w.timeline.duration.value / 2};
        return mid >= cue.start && mid < cue.end();
    };
    for (std::size_t c = 0; c < track.cues.size(); ++c) {
        const CaptionCue& cue = track.cues[c];
        const auto [b, e] = corrected.wordsIn(TimeRange{cue.start, cue.duration});
        std::vector<std::size_t> words;
        for (std::size_t i = b; i < e; ++i) {
            if (midpointIn(corrected.words()[i], cue)) {
                words.push_back(i);
            }
        }
        if (words.empty()) {
            continue;
        }
        const auto [rb, re] = raw.wordsIn(TimeRange{cue.start, cue.duration});
        std::string rawText;
        for (std::size_t i = rb; i < re; ++i) {
            if (midpointIn(raw.words()[i], cue)) {
                rawText += std::string(raw.text(i)) + " ";
            }
        }
        const auto [ub, ue] = uncorrected.wordsIn(TimeRange{cue.start, cue.duration});
        std::string uncorrectedText;
        for (std::size_t i = ub; i < ue; ++i) {
            if (midpointIn(uncorrected.words()[i], cue)) {
                uncorrectedText += std::string(uncorrected.text(i)) + " ";
            }
        }
        std::size_t changedCorrected = 0;
        std::size_t changedRedacted = 0;
        for (const std::size_t i : words) {
            if (corrected.words()[i].redacted) {
                ++changedRedacted;
            }
        }
        const std::string correctedText = corrected.textOf(words.front(), words.back());
        // A cue generated from the transcript: the corrected text (with its bleeps) as a whole.
        if (comparable(cue.text) == comparable(rawText) || comparable(cue.text) == comparable(correctedText) ||
            comparable(cue.text) == comparable(uncorrectedText)) {
            accounted[c] = true;
            if (correctedText != cue.text) {
                for (const std::size_t i : words) {
                    const auto [ob, oe] = raw.wordsIn(corrected.words()[i].timeline);
                    if (ob < oe && !corrected.words()[i].redacted && raw.text(ob) != corrected.text(i)) {
                        ++changedCorrected;
                    }
                }
                out.texts[c] = correctedText;
            }
            out.corrected += changedCorrected;
            out.redacted += changedRedacted;
            continue;
        }
        // An edited cue: replace each corrected or redacted word where its recognised spelling appears.
        std::string text = cue.text;
        std::size_t cursor = 0;
        std::size_t redactedWords = 0;
        std::size_t redactedReplaced = 0;
        for (const std::size_t i : words) {
            const edit::TimelineWord& w = corrected.words()[i];
            redactedWords += w.redacted ? 1 : 0;
            const auto [ob, oe] = raw.wordsIn(w.timeline);
            if (ob >= oe) {
                continue;
            }
            const std::string recognised = raw.textOf(ob, oe - 1);
            const std::string wanted = std::string(corrected.text(i));
            if (recognised == wanted) {
                continue;
            }
            const auto at = findText(text, recognised, cursor);
            if (at == std::string::npos) {
                continue;
            }
            text.replace(at, recognised.size(), wanted);
            cursor = at + wanted.size();
            ++(w.redacted ? out.redacted : out.corrected);
            redactedReplaced += w.redacted ? 1 : 0;
        }
        accounted[c] = redactedReplaced == redactedWords; // a redacted word not found: its spelling may still be there
        out.texts[c] = std::move(text);
    }
    return out;
}

SourceTexts sourceTexts(const ProjectState& state, SequenceId sequence, const CaptionTrack& track, const edit::TranscriptProvider* transcripts) {
    std::vector<bool> accounted;
    SourceTexts out = collectSourceTexts(state, sequence, track, transcripts, accounted);
    // Redacted speech under a cue the transcript does not account for (typed, imported, edited beyond recognition, no
    // transcript at all): which word was redacted is unknown, so the whole cue is "[bleep]", as sidecars and burn-in
    // show it (review wave 5, A5-P1-4). Nothing of it is translated, uploaded or spoken.
    if (const Sequence* seq = state.findSequence(sequence)) {
        const std::vector<TimeRange> redacted = redactedSpeechRanges(state, *seq);
        for (std::size_t c = 0; c < track.cues.size(); ++c) {
            if (!accounted[c]) {
                out.texts[c] = outwardCueText(CaptionCue{track.cues[c].start, track.cues[c].duration, out.texts[c], {}}, redacted);
            }
        }
    }
    // What the report says is kept as "[bleep]": every bleep in the text that is translated, however it got there (the
    // cue was made after the redaction, or a redacted word was replaced here).
    out.redacted = 0;
    for (const std::string& t : out.texts) {
        out.redacted += countBleeps(t);
    }
    return out;
}

// --- Bleeps ---------------------------------------------------------------------------------------------------------

BleepSplit splitBleeps(std::string_view text) {
    BleepSplit out;
    std::size_t from = 0;
    while (true) {
        const auto at = text.find(kBleep, from);
        std::string piece(text.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from));
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\n')) {
            piece.pop_back();
        }
        std::size_t lead = 0;
        while (lead < piece.size() && (piece[lead] == ' ' || piece[lead] == '\n')) {
            ++lead;
        }
        out.pieces.push_back(piece.substr(lead));
        if (at == std::string_view::npos) {
            break;
        }
        ++out.bleeps;
        from = at + kBleep.size();
    }
    return out;
}

std::string joinBleeps(const std::vector<std::string>& pieces, std::string_view language) {
    const bool spaced = !isUnspacedLanguage(language) || phraseSpaced(language);
    std::string out;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (i > 0) {
            if (spaced && !out.empty()) {
                out += ' ';
            }
            out += kBleep;
            if (spaced && !pieces[i].empty()) {
                out += ' ';
            }
        }
        out += pieces[i];
    }
    return out;
}

std::size_t countBleeps(std::string_view text) {
    std::size_t n = 0;
    for (std::size_t at = text.find(kBleep); at != std::string_view::npos; at = text.find(kBleep, at + kBleep.size())) {
        ++n;
    }
    return n;
}

// --- The model's request --------------------------------------------------------------------------------------------

std::string translationContext(std::string_view sourceLanguage, std::string_view targetLanguage, const TranslateOptions& options) {
    const LanguageRules target = rulesFor(targetLanguage);
    const double cps = options.charsPerSecond.value_or(target.charsPerSecond);
    std::string context = "These are the captions of a video, in order. Translate them for subtitles";
    if (!sourceLanguage.empty()) {
        context += " from " + std::string(rulesFor(sourceLanguage).name.empty() ? sourceLanguage : rulesFor(sourceLanguage).name);
    }
    context += " into " + std::string(target.name.empty() ? targetLanguage : target.name) + ": natural spoken " +
               std::string(target.name.empty() ? targetLanguage : target.name) + ", concise enough to be read at " +
               std::to_string(static_cast<int>(cps)) + " characters per second, each line at most " +
               std::to_string(target.maxCharsPerLine) + " characters. Tone: " + std::string(toString(options.tone)) + ".";
    if (normalizeLanguage(targetLanguage) == "th") {
        context += " Thai: use polite particles (ครับ/ค่ะ) only where the source is polite; prefer everyday spoken Thai.";
    }
    if (!options.glossary.empty()) {
        context += " Glossary (always translate these terms like this; a term mapped to itself stays untranslated):";
        for (const GlossaryTerm& term : options.glossary) {
            context += " \"" + term.source + "\" -> \"" + term.target + "\";";
        }
    }
    if (!options.notes.empty()) {
        context += " Notes: " + truncatedUtf8(options.notes, 500);
    }
    return context;
}

ChatRequest translationChatRequest(const TranslationRequest& request, int maxOutputTokens) {
    ChatRequest chat;
    chat.system =
        "You translate video captions. Answer with JSON only: {\"translations\":[{\"id\":\"...\",\"text\":\"...\"}]}, one "
        "entry for every input id, in the same order, nothing else. Translate each entry on its own, using the neighbouring "
        "entries only as context. Keep every [bleep] marker exactly as it is, as many times as it appears and where the "
        "sentence has it: it stands for a word that was bleeped out, never guess or translate it. Do not add notes, quotes "
        "or explanations.";
    Json input = Json::object();
    input["source"] = request.sourceLanguage;
    input["target"] = request.targetLanguage;
    if (!request.context.empty()) {
        input["context"] = request.context;
    }
    if (request.maxCharactersPerSegment) {
        input["maxCharactersPerEntry"] = *request.maxCharactersPerSegment;
    }
    Json segments = Json::array();
    for (const TranslationSegment& s : request.segments) {
        segments.push_back(Json{{"id", s.id}, {"text", s.text}});
    }
    input["entries"] = std::move(segments);
    chat.messages.push_back(ChatMessage::user(input.dump()));
    ResponseFormat format;
    format.name = "caption_translations";
    format.schema = Json{
        {"type", "object"},
        {"properties",
         Json{{"translations", Json{{"type", "array"},
                                    {"items", Json{{"type", "object"},
                                                   {"properties", Json{{"id", Json{{"type", "string"}}}, {"text", Json{{"type", "string"}}}}},
                                                   {"required", Json::array({"id", "text"})},
                                                   {"additionalProperties", false}}}}}}},
        {"required", Json::array({"translations"})},
        {"additionalProperties", false}};
    chat.responseFormat = std::move(format);
    chat.maxOutputTokens = maxOutputTokens;
    return chat;
}

LlmTranslator::LlmTranslator(std::unique_ptr<IChatModel> model, int maxOutputTokens)
    : m_model(std::move(model)), m_maxOutputTokens(maxOutputTokens) {}

std::expected<TranslationResult, AiError> LlmTranslator::translate(const TranslationRequest& request, std::stop_token stop) {
    TranslationResult result;
    if (request.segments.empty()) {
        return result;
    }
    auto response = m_model->chat(translationChatRequest(request, m_maxOutputTokens), {}, stop);
    if (!response) {
        return std::unexpected(response.error());
    }
    result.usage = response->usage;
    std::string text = response->message.text();
    if (const auto open = text.find('{'); open != std::string::npos) {
        const auto close = text.rfind('}');
        text = text.substr(open, close == std::string::npos || close < open ? std::string::npos : close - open + 1);
    }
    const Json answer = Json::parse(text, nullptr, false);
    const Json* list = answer.is_object() && answer.contains("translations") && answer["translations"].is_array() ? &answer["translations"] : nullptr;
    if (!list) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "the model's answer is not the requested JSON"));
    }
    std::map<std::string, std::string> byId;
    for (const Json& item : *list) {
        if (item.is_object() && item.contains("id") && item.contains("text") && item["text"].is_string()) {
            const std::string id = item["id"].is_string() ? item["id"].get<std::string>() : item["id"].dump();
            std::string t = item["text"].get<std::string>();
            if (isValidCaptionText(t) && t.size() <= kMaxCaptionTextBytes && !byId.contains(id)) {
                byId.emplace(id, std::move(t));
            }
        }
    }
    // Same ids, same order; a missing or invalid entry comes back empty (the caller retries it on its own).
    for (const TranslationSegment& s : request.segments) {
        const auto it = byId.find(s.id);
        result.segments.push_back(TranslationSegment{s.id, it == byId.end() ? std::string() : it->second});
    }
    return result;
}

// --- A track --------------------------------------------------------------------------------------------------------

std::expected<LanguageResult, AiError> translateTrack(const CaptionTrack& source, const std::vector<std::string>& texts,
                                                      std::string_view language, ITranslator& translator,
                                                      const TranslateOptions& options, const LineBreaker& breaker,
                                                      const std::function<bool(std::size_t done, std::size_t total)>& progress,
                                                      std::stop_token stop) {
    LanguageResult out;
    out.language = normalizeLanguage(language);
    const std::string sourceLanguage = normalizeLanguage(options.sourceLanguage.empty() ? source.language : options.sourceLanguage);
    LanguageRules rules = rulesFor(out.language);
    if (options.charsPerSecond) {
        rules.charsPerSecond = *options.charsPerSecond;
    }
    const std::size_t n = std::min(texts.size(), source.cues.size());
    const std::string context = translationContext(sourceLanguage, out.language, options);

    // What each cue sends: the whole text with its bleeps as markers, or its pieces between bleeps.
    struct Job {
        std::size_t cue = 0;
        std::size_t piece = 0; ///< SIZE_MAX: the whole text
        std::string text;
    };
    std::vector<BleepSplit> splits(n);
    std::vector<std::string> translated(n);
    std::vector<std::vector<std::string>> pieceResults(n);
    std::vector<bool> done(n, false);
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < n; ++i) {
        splits[i] = splitBleeps(texts[i]);
        const bool nothingToSay = std::ranges::all_of(splits[i].pieces, [](const std::string& p) { return p.empty(); });
        if (nothingToSay) {
            translated[i] = splits[i].bleeps > 0 ? joinBleeps(splits[i].pieces, out.language) : std::string();
            done[i] = true;
            continue;
        }
        if (splits[i].bleeps == 0 || options.markersKept) {
            jobs.push_back(Job{i, SIZE_MAX, texts[i]});
        } else {
            pieceResults[i].assign(splits[i].pieces.size(), std::string());
            for (std::size_t p = 0; p < splits[i].pieces.size(); ++p) {
                if (!splits[i].pieces[p].empty()) {
                    jobs.push_back(Job{i, p, splits[i].pieces[p]});
                }
            }
        }
        out.characters += texts[i].size();
    }

    const auto run = [&](std::vector<Job>& batchJobs, std::size_t& finished, std::size_t total) -> std::expected<void, AiError> {
        for (std::size_t from = 0; from < batchJobs.size(); from += std::max<std::size_t>(1, options.batch)) {
            if (stop.stop_requested()) {
                return std::unexpected(AiError(AiErrorCode::Cancelled, "translation cancelled"));
            }
            const std::size_t to = std::min(batchJobs.size(), from + std::max<std::size_t>(1, options.batch));
            TranslationRequest request;
            request.sourceLanguage = sourceLanguage;
            request.targetLanguage = out.language;
            request.context = context;
            request.maxCharactersPerSegment = rules.maxCharsPerLine * rules.maxLines;
            for (std::size_t k = from; k < to; ++k) {
                request.segments.push_back(TranslationSegment{std::to_string(k), batchJobs[k].text});
            }
            auto result = translator.translate(request, stop);
            if (!result) {
                return std::unexpected(result.error());
            }
            for (std::size_t k = from; k < to && k - from < result->segments.size(); ++k) {
                const Job& job = batchJobs[k];
                std::string text = flattenCaptionText(result->segments[k - from].text, out.language);
                if (job.piece == SIZE_MAX) {
                    translated[job.cue] = std::move(text);
                } else {
                    pieceResults[job.cue][job.piece] = std::move(text);
                }
            }
            finished += to - from;
            if (progress && !progress(finished, total)) {
                return std::unexpected(AiError(AiErrorCode::Cancelled, "translation cancelled"));
            }
        }
        return {};
    };

    std::size_t finished = 0;
    if (auto ran = run(jobs, finished, jobs.size()); !ran) {
        return std::unexpected(ran.error());
    }
    // Whole-text translations that lost or added a bleep, or came back empty: again, piece by piece.
    std::vector<Job> retry;
    for (std::size_t i = 0; i < n; ++i) {
        if (done[i]) {
            continue;
        }
        if (!pieceResults[i].empty()) {
            continue; // already by pieces
        }
        if (!translated[i].empty() && countBleeps(translated[i]) == splits[i].bleeps) {
            done[i] = true;
            continue;
        }
        ++out.retried;
        translated[i].clear();
        pieceResults[i].assign(splits[i].pieces.size(), std::string());
        for (std::size_t p = 0; p < splits[i].pieces.size(); ++p) {
            if (!splits[i].pieces[p].empty()) {
                retry.push_back(Job{i, p, splits[i].pieces[p]});
            }
        }
    }
    if (!retry.empty()) {
        std::size_t again = 0;
        if (auto ran = run(retry, again, retry.size()); !ran) {
            return std::unexpected(ran.error());
        }
    }
    std::vector<std::size_t> failed;
    for (std::size_t i = 0; i < n; ++i) {
        if (done[i]) {
            continue;
        }
        bool ok = true;
        for (std::size_t p = 0; p < splits[i].pieces.size(); ++p) {
            ok = ok && (splits[i].pieces[p].empty() || !pieceResults[i][p].empty());
        }
        if (ok && countBleeps(joinBleeps(pieceResults[i], out.language)) == splits[i].bleeps) {
            // A piece that itself came back with a bleep marker would add content-free markers: strip them.
            for (std::string& piece : pieceResults[i]) {
                for (auto at = piece.find(kBleep); at != std::string::npos; at = piece.find(kBleep)) {
                    piece.erase(at, kBleep.size());
                }
            }
            translated[i] = joinBleeps(pieceResults[i], out.language);
            done[i] = true;
        } else {
            translated[i] = texts[i]; // untranslated, kept so nothing goes missing
            failed.push_back(i + 1);
            ++out.untranslated;
        }
    }
    if (!failed.empty()) {
        std::string list;
        for (std::size_t k = 0; k < failed.size() && k < 12; ++k) {
            list += (k ? ", " : "") + std::to_string(failed[k]);
        }
        out.warnings.push_back(std::to_string(failed.size()) + " cue(s) could not be translated and keep the source text: " + list +
                               (failed.size() > 12 ? ", ..." : ""));
    }

    // Glossary: a term in the source whose translation is not in the result.
    for (std::size_t t = 0; t < options.glossary.size(); ++t) {
        const GlossaryTerm& term = options.glossary[t];
        std::size_t missed = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (findText(texts[i], term.source, 0) != std::string::npos && findText(translated[i], term.target, 0) == std::string::npos) {
                ++missed;
            }
        }
        if (missed > 0) {
            out.missedTerms.push_back(t);
            out.warnings.push_back("\"" + term.source + "\" was not translated as \"" + term.target + "\" in " + std::to_string(missed) +
                                   " cue(s)");
        }
    }

    // Re-segment to the target's reading speed.
    std::vector<TranslatedCue> cues;
    cues.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (translated[i].empty()) {
            continue;
        }
        cues.push_back(TranslatedCue{source.cues[i].start, source.cues[i].duration, truncatedUtf8(translated[i], kMaxCaptionTextBytes),
                                     source.cues[i].speaker, i});
    }
    const std::vector<TimedCue> timed = resegment(cues, out.language, rules, options.timing, breaker, &out.segmentation);
    if (out.segmentation.tooFast > 0) {
        out.warnings.push_back(std::to_string(out.segmentation.tooFast) + " cue(s) still read faster than " +
                               std::to_string(static_cast<int>(rules.charsPerSecond)) + " characters per second");
    }

    out.track.name = source.name + " (" + std::string(rules.nativeName.empty() ? out.language : rules.nativeName) + ")";
    out.track.name = truncatedUtf8(out.track.name, kMaxCaptionNameBytes);
    out.track.language = out.language;
    out.track.enabled = true;
    out.track.style = source.style;
    if (phraseSpaced(out.language)) {
        if (const CaptionPreset* thai = findBuiltinCaptionPreset("Thai-friendly")) {
            out.track.style.fontFamily = thai->style.fontFamily;
            out.track.style.fontWeight = thai->style.fontWeight;
            out.track.style.lineSpacing = thai->style.lineSpacing;
        }
    }
    for (const TimedCue& t : timed) {
        CaptionCue cue;
        cue.start = t.start;
        cue.duration = t.duration;
        cue.text = truncatedUtf8(t.text, kMaxCaptionTextBytes);
        cue.speaker = t.speaker;
        out.track.cues.push_back(std::move(cue));
    }
    return out;
}

} // namespace wabcut::ai::translate
