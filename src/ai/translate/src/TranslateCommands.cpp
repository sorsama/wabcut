#include <wabcut/ai/translate/TranslateCommands.h>

#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/FinishingOps.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <usp10.h>
#endif

namespace wabcut::ai::translate {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

double rounded(double v, double scale = 100.0) { return std::round(v * scale) / scale; }

#ifdef _WIN32
/// Uniscribe's line breaks (ScriptBreak's soft breaks): words in Thai, Lao, Khmer and Burmese, spaces and punctuation
/// elsewhere.
class UniscribeLineBreaker final : public LineBreaker {
public:
    std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view) const override {
        std::vector<std::size_t> out;
        if (text.empty() || text.size() > 1 << 20) {
            return out;
        }
        const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (wide <= 0) {
            return out;
        }
        std::wstring w(static_cast<std::size_t>(wide), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w.data(), wide);
        // UTF-16 index -> UTF-8 byte offset.
        std::vector<std::size_t> byteAt(static_cast<std::size_t>(wide) + 1, text.size());
        {
            std::size_t b = 0;
            std::size_t u = 0;
            while (b < text.size() && u < static_cast<std::size_t>(wide)) {
                byteAt[u] = b;
                const auto lead = static_cast<unsigned char>(text[b]);
                const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                u += length == 4 ? 2 : 1;
                if (length == 4 && u - 1 < byteAt.size()) {
                    byteAt[u - 1] = b; // the low surrogate maps to the same character
                }
                b += length;
            }
        }
        std::vector<SCRIPT_ITEM> items(static_cast<std::size_t>(wide) + 2);
        int count = 0;
        if (FAILED(ScriptItemize(w.data(), wide, static_cast<int>(items.size()) - 1, nullptr, nullptr, items.data(), &count))) {
            return out;
        }
        std::vector<SCRIPT_LOGATTR> attrs(static_cast<std::size_t>(wide));
        for (int i = 0; i < count; ++i) {
            const int begin = items[static_cast<std::size_t>(i)].iCharPos;
            const int end = items[static_cast<std::size_t>(i) + 1].iCharPos;
            if (end > begin) {
                ScriptBreak(w.data() + begin, end - begin, &items[static_cast<std::size_t>(i)].a, attrs.data() + begin);
            }
        }
        for (int i = 1; i < wide; ++i) {
            if (attrs[static_cast<std::size_t>(i)].fSoftBreak || (w[static_cast<std::size_t>(i - 1)] == L' ' && w[static_cast<std::size_t>(i)] != L' ')) {
                const std::size_t at = byteAt[static_cast<std::size_t>(i)];
                if (at > 0 && at < text.size() && (out.empty() || out.back() < at)) {
                    out.push_back(at);
                }
            }
        }
        return out;
    }
};
#endif

TranslateServices* servicesOf(ExecutionContext& c) { return command::findService<TranslateServices>(c.services()); }

const CaptionTrack& trackOf(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    if (seq.captionTracks.empty()) {
        fail(ErrorCode::NotFound, "sequence '" + seq.name + "' has no caption track to translate");
    }
    if (!c.has("captionTrack")) {
        if (seq.captionTracks.size() > 1) {
            fail(ErrorCode::InvalidParams, "the sequence has several caption tracks: pass \"captionTrack\"", Json{{"path", "/captionTrack"}});
        }
        return seq.captionTracks.front();
    }
    const auto id = static_cast<std::uint64_t>(c.integer("captionTrack"));
    for (const CaptionTrack& t : seq.captionTracks) {
        if (t.id.value == id) {
            return t;
        }
    }
    fail(ErrorCode::NotFound, "no caption track " + std::to_string(id) + " in '" + seq.name + "'", Json{{"path", "/captionTrack"}});
}

TranslateOptions optionsOf(ExecutionContext& c, const CaptionTrack& track) {
    TranslateOptions options;
    options.sourceLanguage = normalizeLanguage(c.string("sourceLanguage", track.language));
    if (c.has("tone")) {
        options.tone = toneFromString(c.string("tone")).value_or(Tone::Neutral);
    }
    options.notes = c.string("notes", "");
    if (c.has("readingSpeed")) {
        options.charsPerSecond = c.number("readingSpeed");
    }
    if (c.has("glossary")) {
        for (const Json& term : c.params()["glossary"]) {
            options.glossary.push_back(GlossaryTerm{term.value("source", std::string()), term.value("target", std::string())});
        }
    }
    const Sequence& seq = c.sequence();
    if (seq.frameRate.isValid()) {
        options.timing.minGap = Ticks{seq.frameRate.ticksPerUnit() * 2};
    }
    return options;
}

std::chrono::year_month_day today() {
    return std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
}

CommandOutput translateCommand(ExecutionContext& c) {
    const CaptionTrack& source = trackOf(c);
    if (source.cues.empty()) {
        fail(ErrorCode::EditRejected, "caption track '" + source.name + "' has no cues");
    }
    TranslateOptions options = optionsOf(c, source);
    std::vector<std::string> languages;
    for (const Json& l : c.params()["languages"]) {
        const std::string code = l.get<std::string>();
        if (!isLanguageTag(code)) {
            fail(ErrorCode::InvalidParams, "\"" + code + "\" is not a language code (ISO 639-1, e.g. \"th\")", Json{{"path", "/languages"}});
        }
        const std::string normalized = normalizeLanguage(code);
        if (!options.sourceLanguage.empty() && normalized == options.sourceLanguage) {
            fail(ErrorCode::InvalidParams, "the captions are already in \"" + normalized + "\"", Json{{"path", "/languages"}});
        }
        if (std::ranges::find(languages, normalized) == languages.end()) {
            languages.push_back(normalized);
        }
    }
    const std::string provider = c.string("provider", "auto");
    TranslateServices* services = servicesOf(c);
    const bool cloudAllowed = !c.state().aiNoCloud;
    if (provider == "cloud" && !cloudAllowed) {
        fail(ErrorCode::EditRejected, "this project is no-cloud: captions can only be translated by a local model");
    }

    // Which translator does each language get?
    struct Choice {
        std::string language;
        std::unique_ptr<ITranslator> translator;
        bool local = false;
        std::string model;
    };
    std::vector<Choice> choices;
    std::unique_ptr<IChatModel> estimateModel;
    for (const std::string& language : languages) {
        Choice choice;
        choice.language = language;
        std::string localProblem = "this host has no local translation models";
        if (provider != "cloud" && services && services->local()) {
            auto local = services->local()(options.sourceLanguage, language);
            if (local) {
                choice.translator = std::move(*local);
                choice.local = true;
                choice.model = "local";
            } else {
                localProblem = local.error().message;
            }
        }
        if (!choice.translator && provider == "local") {
            fail(ErrorCode::EditRejected, "no local translation into \"" + language + "\": " + localProblem);
        }
        if (!choice.translator) {
            if (!cloudAllowed) {
                fail(ErrorCode::EditRejected, "no local translation into \"" + language + "\" (" + localProblem +
                                                  "), and this project is no-cloud");
            }
            if (!services || !services->chat()) {
                fail(ErrorCode::EditRejected, "no local translation into \"" + language + "\" (" + localProblem +
                                                  "), and no cloud model is available here");
            }
            auto model = services->chat()();
            if (!model) {
                fail(ErrorCode::EditRejected, "no local translation into \"" + language + "\" (" + localProblem + "); the cloud model: " +
                                                  model.error().message);
            }
            choice.model = (*model)->spec().id;
            if (c.boolean("estimate", false) && !estimateModel) {
                estimateModel = std::move(*model);
            } else {
                choice.translator = std::make_unique<LlmTranslator>(std::move(*model));
            }
        }
        choices.push_back(std::move(choice));
    }

    auto* transcripts = command::findService<edit::TranscriptServices>(c.services());
    const SourceTexts texts = sourceTexts(c.state(), c.sequence().id, source, transcripts ? transcripts->transcripts.get() : nullptr);

    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"sourceTrack", source.id.value}, {"sourceLanguage", options.sourceLanguage}, {"cues", source.cues.size()},
                    {"corrected", texts.corrected}, {"redacted", texts.redacted}};

    if (c.boolean("estimate", false)) {
        // The cloud cost, request by request, without sending anything.
        Json estimates = Json::array();
        double expected = 0.0;
        double ceiling = 0.0;
        for (const Choice& choice : choices) {
            if (choice.local) {
                estimates.push_back(Json{{"language", choice.language}, {"provider", "local"}, {"usd", 0.0}});
                continue;
            }
            const ModelSpec& spec = estimateModel ? estimateModel->spec() : static_cast<LlmTranslator*>(choice.translator.get())->spec();
            CostEstimate total;
            for (std::size_t from = 0; from < texts.texts.size(); from += options.batch) {
                TranslationRequest request;
                request.sourceLanguage = options.sourceLanguage;
                request.targetLanguage = choice.language;
                request.context = translationContext(options.sourceLanguage, choice.language, options);
                for (std::size_t i = from; i < std::min(texts.texts.size(), from + options.batch); ++i) {
                    request.segments.push_back(TranslationSegment{std::to_string(i), texts.texts[i]});
                }
                const CostEstimate e = estimateChatCost(spec, translationChatRequest(request, 8192), today());
                total.expected += e.expected;
                total.ceiling += e.ceiling;
                total.priced = e.priced;
            }
            const double usdExpected = static_cast<double>(total.expected) / 1e6;
            const double usdCeiling = static_cast<double>(total.ceiling) / 1e6;
            expected += usdExpected;
            ceiling += usdCeiling;
            estimates.push_back(Json{{"language", choice.language}, {"provider", "cloud"}, {"model", choice.model},
                                     {"usd", rounded(usdExpected, 10000.0)}, {"ceilingUsd", rounded(usdCeiling, 10000.0)},
                                     {"priced", total.priced}});
        }
        out.data["estimate"] = Json{{"languages", std::move(estimates)}, {"usd", rounded(expected, 10000.0)},
                                    {"ceilingUsd", rounded(ceiling, 10000.0)}};
        return out;
    }

    std::shared_ptr<const LineBreaker> system;
    const LineBreaker* breaker = nullptr;
    if (auto* captions = command::findService<command::CaptionServices>(c.services())) {
        breaker = &captions->lineBreaker();
    } else {
        system = systemLineBreaker();
        breaker = system.get();
    }

    Json results = Json::array();
    const bool replace = c.boolean("replace", true);
    for (std::size_t k = 0; k < choices.size(); ++k) {
        Choice& choice = choices[k];
        TranslateOptions languageOptions = options;
        languageOptions.markersKept = !choice.local;
        const auto progress = [&](std::size_t done, std::size_t total) {
            c.checkpoint("translating into " + choice.language + " (" + std::to_string(done) + "/" + std::to_string(total) + ")");
            return true;
        };
        auto translated = translateTrack(source, texts.texts, choice.language, *choice.translator, languageOptions, *breaker, progress, {});
        if (!translated) {
            const AiError& e = translated.error();
            const ErrorCode code = e.code == AiErrorCode::Cancelled            ? ErrorCode::Cancelled
                                   : e.code == AiErrorCode::ConsentRequired ||
                                           e.code == AiErrorCode::SpendCapExceeded || e.code == AiErrorCode::Offline
                                       ? ErrorCode::PermissionDenied
                                       : ErrorCode::EditRejected;
            fail(code, "translating into \"" + choice.language + "\" failed: " + e.message);
        }
        CaptionTrack track = std::move(translated->track);
        if (replace) {
            for (const CaptionTrack& existing : out.state.findSequence(c.sequence().id)->captionTracks) {
                if (existing.id != source.id && existing.name == track.name && normalizeLanguage(existing.language) == choice.language) {
                    track.id = existing.id;
                }
            }
        }
        const bool replaced = track.id.isValid();
        auto edited = wabcut::edit::setCaptionTrack(out.state, c.sequence().id, track);
        if (!edited) {
            fail(ErrorCode::EditRejected, edited.error().message);
        }
        out.state = std::move(edited->state);
        const CaptionTrackId id = replaced ? track.id : edited->createdCaptionTrack;
        if (!replaced) {
            out.createdIds.push_back(id.value);
        }
        const SegmentationReport& s = translated->segmentation;
        Json warnings = Json::array();
        for (const std::string& w : translated->warnings) {
            warnings.push_back(w);
            out.warnings.push_back(choice.language + ": " + w);
        }
        results.push_back(Json{{"language", choice.language},
                               {"captionTrack", id.value},
                               {"name", track.name},
                               {"replaced", replaced},
                               {"provider", choice.local ? "local" : "cloud"},
                               {"model", choice.model},
                               {"cues", track.cues.size()},
                               {"kept", s.kept},
                               {"extended", s.extended},
                               {"merged", s.merged},
                               {"rebalanced", s.rebalanced},
                               {"split", s.split},
                               {"tooFast", s.tooFast},
                               {"maxCharsPerSecond", rounded(s.maxCharsPerSecond, 10.0)},
                               {"readingSpeed", rounded(languageOptions.charsPerSecond.value_or(rulesFor(choice.language).charsPerSecond), 10.0)},
                               {"retried", translated->retried},
                               {"untranslated", translated->untranslated},
                               {"characters", translated->characters},
                               {"warnings", std::move(warnings)}});
    }
    out.data["languages"] = std::move(results);
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

std::shared_ptr<const LineBreaker> systemLineBreaker() {
#ifdef _WIN32
    static const auto breaker = std::make_shared<const UniscribeLineBreaker>();
    return breaker;
#else
    return std::shared_ptr<const LineBreaker>(std::shared_ptr<const LineBreaker>{}, &simpleLineBreaker());
#endif
}

void registerTranslateCommands(command::CommandRegistry& registry) {
    Json params = schema::object(
        {{"captionTrack", schema::integer("The caption track to translate; only needed when the sequence has several.", 1)},
         {"languages", schema::array(Json{{"type", "string"}, {"minLength", 2}, {"maxLength", 16}},
                                     "Target languages, ISO 639-1 (\"th\", \"ja\", \"es\", \"zh-Hant\" ...): one new caption track each.", 1)},
         {"provider", schema::enumeration({"auto", "local", "cloud"},
                                          "Who translates: a local model on this computer, the user's cloud model, or \"auto\" (local when "
                                          "a model for the pair is installed, else the cloud model unless the project is no-cloud; default).")},
         {"sourceLanguage", Json{{"type", "string"}, {"minLength", 2}, {"maxLength", 16},
                                 {"description", "The captions' language when the track does not say (ISO 639-1)."}}},
         {"glossary", schema::array(schema::object({{"source", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 200}}},
                                                    {"target", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 200}}}},
                                                   {"source", "target"}),
                                    "Terms and how to translate them (a term mapped to itself stays as it is: names, brands).")},
         {"tone", schema::enumeration({"neutral", "casual", "formal", "playful"}, "The register of a cloud translation (default neutral).")},
         {"notes", Json{{"type", "string"}, {"maxLength", 500}, {"description", "Extra instructions for a cloud model (audience, who speaks)."}}},
         {"readingSpeed", schema::number("Characters per second the new captions may ask of a reader (default: the language's, e.g. "
                                         "English 20, Thai 17, Japanese 4).",
                                         2.0, 40.0)},
         {"replace", schema::boolean("Replace an earlier translation of this track into the same language (default true).", true)},
         {"estimate", schema::boolean("Only price the cloud translation; nothing is sent or changed (default false).", false)}},
        {"languages"});
    params["properties"]["glossary"]["maxItems"] = 200;
    params["properties"]["languages"]["maxItems"] = 10;
    params["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    add(registry,
        command::runsModel({"captions.translate", 1, "Translate Captions",
         "Translates a caption track into new caption tracks, one per language, keeping the cue timing: cues that would read too fast "
         "in the target language borrow time from the gaps, merge with a neighbour or move words across, and text that does not fit "
         "two lines is split into more cues (Thai broken between words). The corrections layer applies first, and bleeped words stay "
         "\"[bleep]\" (never sent as content). A local model (OPUS-MT, on this computer) or the user's cloud model (consent and "
         "spend cap; caption text only) with a glossary and a tone. One undo step; \"estimate\" prices a cloud run first.",
         command::Capability::EditSequence, params, Json{{"languages", Json::array({"th", "ja"})}, {"provider", "auto"}}, {},
         translateCommand}));
}

} // namespace wabcut::ai::translate
