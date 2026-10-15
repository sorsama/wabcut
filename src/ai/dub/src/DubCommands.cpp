#include <wabcut/ai/dub/DubCommands.h>

#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/ai/dub/Dub.h>
#include <wabcut/ai/dub/Voices.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/translate/CaptionTranslation.h>
#include <wabcut/ai/translate/Languages.h>
#include <wabcut/command/Schema.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace wabcut::ai::dub {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

constexpr std::size_t kMaxScriptBytes = 64 * 1024;
constexpr std::size_t kMaxLines = 2000;
const Ticks kMinCloneSpeech = Ticks::fromSeconds(5);
const Ticks kMaxCloneSpeech = Ticks::fromSeconds(90);

double usd(MicroUsd micro) { return std::round(static_cast<double>(micro) / 1e6 * 10000.0) / 10000.0; }
double secondsOf(Ticks t) { return std::round(static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond) * 1000.0) / 1000.0; }

std::unexpected<PlanError> refuse(ErrorCode code, std::string message) { return std::unexpected(PlanError{code, std::move(message)}); }

bool speaks(const ModelSpec& model) {
    return std::ranges::find(model.caps.capabilities, Capability::Speak) != model.caps.capabilities.end();
}

/// Languages ElevenLabs Flash v2.5 does not speak, of those Wabcut names (elevenlabs.io language list, 2026-10-12).
bool flashLacks(std::string_view language) { return language == "th" || language == "lo" || language == "km" || language == "my"; }

std::expected<CloneSource, PlanError> cloneSourceOf(const ProjectState& state, const Sequence& sequence, SpeakerId speaker,
                                                const ai::edit::TranscriptProvider& transcripts) {
    const auto who = std::ranges::find_if(state.speakers, [&](const Speaker& s) { return s.id == speaker; });
    if (!speaker.isValid() || who == state.speakers.end()) {
        return refuse(ErrorCode::NotFound, "no speaker " + std::to_string(speaker.value) + " to clone");
    }
    const ai::edit::SequenceTranscript transcript = ai::edit::SequenceTranscript::build(state, sequence.id, transcripts);
    // Media item and stream -> their speech ranges (media time).
    std::map<std::pair<std::uint64_t, int>, std::vector<TimeRange>> speech;
    bool any = false;
    for (const ai::edit::TimelineWord& word : transcript.words()) {
        if (word.speakerId != speaker) {
            continue;
        }
        any = true;
        if (word.redacted) {
            return refuse(ErrorCode::PermissionDenied,
                          who->name + " has redacted speech in this sequence: a redacted speaker's voice is never cloned");
        }
        const auto clip = sequence.findClip(word.clip);
        if (!clip) {
            continue;
        }
        speech[{clip->clip().media.value, clip->clip().stream.index}].push_back(word.source);
    }
    if (!any) {
        return refuse(ErrorCode::NotFound, "no transcribed speech of " + who->name + " in this sequence (transcribe it and name the speakers)");
    }
    CloneSource best;
    for (auto& [key, ranges] : speech) {
        std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
        // Words less than 0.35 s apart are one run of speech.
        std::vector<TimeRange> runs;
        for (const TimeRange& r : ranges) {
            if (!runs.empty() && r.start - runs.back().end() <= Ticks{kTicksPerSecond * 35 / 100}) {
                runs.back() = TimeRange::fromStartEnd(runs.back().start, std::max(runs.back().end(), r.end()));
            } else {
                runs.push_back(r);
            }
        }
        // A run never spans a redaction: the gap between two of the speaker's words can hold someone else's redacted word,
        // which would be uploaded in the sample (review wave 5, A5-P2-15).
        const std::vector<TimeRange> redacted = ai::audioRedactionRanges(state, MediaId{key.first});
        std::vector<TimeRange> clear;
        for (const TimeRange& run : runs) {
            Ticks from = run.start;
            for (const TimeRange& r : redacted) {
                if (r.end() <= from || r.start >= run.end()) {
                    continue;
                }
                if (r.start > from) {
                    clear.push_back(TimeRange::fromStartEnd(from, r.start));
                }
                from = std::max(from, r.end());
            }
            if (from < run.end()) {
                clear.push_back(TimeRange::fromStartEnd(from, run.end()));
            }
        }
        CloneSource source;
        source.media = MediaId{key.first};
        source.stream = key.second;
        for (const TimeRange& r : clear) {
            if (source.total >= kMaxCloneSpeech) {
                break;
            }
            source.ranges.push_back(r);
            source.total = source.total + r.duration;
        }
        if (source.total > best.total) {
            best = std::move(source);
        }
    }
    if (best.total < kMinCloneSpeech) {
        return refuse(ErrorCode::InvalidParams, "cloning needs at least 5 s of " + who->name + "'s speech in one recording (there is " +
                                                    std::to_string(secondsOf(best.total)) + " s)");
    }
    return best;
}

const CaptionTrack* captionTrackOf(const Sequence& sequence, CaptionTrackId id) {
    if (!id.isValid()) {
        return sequence.captionTracks.size() == 1 ? &sequence.captionTracks.front() : nullptr;
    }
    for (const CaptionTrack& t : sequence.captionTracks) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

ErrorCode commandCode(AiErrorCode code) {
    switch (code) {
    case AiErrorCode::Cancelled:
        return ErrorCode::Cancelled;
    case AiErrorCode::ConsentRequired:
    case AiErrorCode::SpendCapExceeded:
    case AiErrorCode::Offline:
    case AiErrorCode::NoCredentials:
        return ErrorCode::PermissionDenied;
    default:
        return ErrorCode::EditRejected;
    }
}

} // namespace

std::expected<DubPlan, PlanError> planDub(const ProjectState& state, SequenceId sequenceId, const DubRequest& request,
                                      const ModelRegistry& registry, const ai::edit::TranscriptProvider* transcripts,
                                      std::chrono::year_month_day today) {
    const Sequence* sequence = state.findSequence(sequenceId);
    if (sequence == nullptr) {
        return refuse(ErrorCode::NotFound, "no sequence " + std::to_string(sequenceId.value));
    }
    if (state.aiNoCloud) {
        return refuse(ErrorCode::PermissionDenied, "this project is no-cloud: voice-over needs the user's cloud voice provider (no local "
                                                   "voice model has a licence Wabcut can ship)");
    }
    DubPlan plan;
    plan.request = request;
    plan.sequence = sequenceId;

    // The model and the voice.
    const std::string modelId = request.model.empty() ? std::string("eleven_v3") : request.model;
    auto model = registry.usableModel(modelId, today);
    if (!model) {
        return refuse(ErrorCode::InvalidParams, "voice model \"" + modelId + "\": " + model.error().message);
    }
    if (!speaks(**model)) {
        return refuse(ErrorCode::InvalidParams, "\"" + modelId + "\" is not a text-to-speech model");
    }
    plan.model = **model;
    std::string api = plan.model.api;
    if (api.empty()) {
        if (const ProviderSpec* provider = registry.provider(plan.model.provider)) {
            api = provider->api;
        }
    }
    plan.model.api = api;
    if (request.cloneVoice) {
        if (api != "elevenlabs-tts") {
            return refuse(ErrorCode::InvalidParams, "voices can only be cloned with ElevenLabs models");
        }
        if (!request.rightsConfirmed) {
            return refuse(ErrorCode::PermissionDenied, "cloning a voice needs the speaker's permission: confirm \"I have the speaker's "
                                                       "permission\" (rightsConfirmed)");
        }
        if (transcripts == nullptr) {
            return refuse(ErrorCode::InvalidParams, "cloning needs the sequence's transcripts, and this host has none");
        }
        auto clone = cloneSourceOf(state, *sequence, request.cloneSpeaker, *transcripts);
        if (!clone) {
            return std::unexpected(clone.error());
        }
        plan.clone = std::move(*clone);
        plan.voice = "(cloned)";
    } else {
        plan.voice = request.voice.empty() ? defaultVoice(api) : request.voice;
        if (!isValidVoice(api, plan.voice)) {
            return refuse(ErrorCode::InvalidParams, "\"" + plan.voice + "\" is not a voice of " + plan.model.name);
        }
    }
    if (!(request.maxStretch >= 0.0 && request.maxStretch <= 0.5)) {
        return refuse(ErrorCode::InvalidParams, "maxStretch must be between 0 and 0.5");
    }

    // The lines.
    if (request.kind == DubKind::Dub) {
        const CaptionTrack* track = captionTrackOf(*sequence, request.captionTrack);
        if (track == nullptr) {
            return refuse(request.captionTrack.isValid() ? ErrorCode::NotFound : ErrorCode::InvalidParams,
                          request.captionTrack.isValid() ? "no caption track " + std::to_string(request.captionTrack.value) + " in '" + sequence->name + "'"
                          : sequence->captionTracks.empty() ? "the sequence has no captions to dub (captions.fromTranscript, then captions.translate)"
                                                             : "the sequence has several caption tracks: pass \"captionTrack\"");
        }
        plan.request.captionTrack = track->id;
        plan.language = translate::normalizeLanguage(request.language.empty() ? track->language : request.language);
        // What may be spoken (and uploaded to the voice provider) is what may be translated: corrections applied, and
        // redacted speech masked whatever made the cue - a typed or imported cue over a bleep says nothing at all
        // (review wave 5, A5-P1-4). The voice preview speaks the plan's first line, so it is masked too.
        const translate::SourceTexts spoken = translate::sourceTexts(state, sequenceId, *track, transcripts);
        CaptionTrack safe = *track;
        std::size_t withheld = 0;
        for (std::size_t i = 0; i < safe.cues.size() && i < spoken.texts.size(); ++i) {
            if (spoken.texts[i] == ai::kRedactedWord && !speakableText(safe.cues[i].text).empty()) {
                ++withheld;
            }
            safe.cues[i].text = spoken.texts[i];
        }
        plan.lines = linesFromCaptions(safe);
        if (withheld > 0) {
            plan.warnings.push_back(std::to_string(withheld) + (withheld == 1 ? " caption is" : " captions are") +
                                    " not spoken: redacted speech is under it and its words cannot be told apart");
        }
        if (plan.lines.empty()) {
            return refuse(ErrorCode::InvalidParams, "caption track '" + track->name + "' has no cue with words to speak");
        }
    } else {
        if (request.text.size() > kMaxScriptBytes) {
            return refuse(ErrorCode::InvalidParams, "the script is longer than 64 KB");
        }
        plan.language = translate::normalizeLanguage(request.language);
        plan.lines = narrationLines(request.text);
        if (plan.lines.empty()) {
            return refuse(ErrorCode::InvalidParams, "the script has nothing to say");
        }
        if (request.onBeats) {
            plan.beats = roughCutBeats(*sequence);
            if (!plan.beats.empty() && plan.beats.size() != plan.lines.size()) {
                plan.warnings.push_back("the script has " + std::to_string(plan.lines.size()) + " lines and the rough cut " +
                                        std::to_string(plan.beats.size()) + " beats: the narration runs line after line instead");
            }
        }
    }
    if (plan.lines.size() > kMaxLines) {
        return refuse(ErrorCode::InvalidParams, "more than " + std::to_string(kMaxLines) + " lines in one run");
    }
    if (plan.model.id == "eleven_flash_v2_5" && flashLacks(plan.language)) {
        return refuse(ErrorCode::InvalidParams, "ElevenLabs Flash v2.5 does not speak \"" + plan.language + "\": choose eleven_v3 or eleven_v4");
    }

    plan.duckDb = request.duckDb.value_or(request.kind == DubKind::Dub ? -18.0 : -12.0);
    if (!(plan.duckDb <= 0.0 && plan.duckDb >= -60.0)) {
        return refuse(ErrorCode::InvalidParams, "duck must be between -60 and 0 dB");
    }
    if (!request.trackName.empty()) {
        plan.trackName = request.trackName;
    } else if (request.kind == DubKind::Narration) {
        plan.trackName = "Narration";
    } else {
        plan.trackName = plan.language.empty() ? std::string("Dub") : "Dub (" + std::string(translate::rulesFor(plan.language).nativeName) + ")";
    }
    for (const DubLine& line : plan.lines) {
        plan.characters += characterCount(line.text);
    }
    plan.cost = estimateDubCost(plan.model, plan.lines, today);
    if (!plan.cost.priced) {
        plan.warnings.push_back(plan.model.name + " has no price in the registry: the run's cost is unknown");
    }
    return plan;
}

std::expected<DubOutcome, AiError> executeDub(const ProjectState& state, const DubPlan& plan, ISpeechSynth& speech, SpeechAudio& audio,
                                              const std::filesystem::path& directory, std::stop_token stop, const LineProgress& progress) {
    DubJob job;
    job.kind = plan.request.kind;
    job.lines = plan.lines;
    job.voice.voiceId = plan.clone ? std::string() : plan.voice;
    job.voice.language = plan.language;
    job.voice.style = plan.request.style;
    job.fit.maxStretch = plan.request.maxStretch;
    job.at = plan.request.at.value_or(Ticks{});
    job.gap = plan.request.gap;
    job.beats = plan.beats.size() == plan.lines.size() ? plan.beats : std::vector<TimeRange>{};
    job.directory = directory;
    job.name = plan.trackName;
    job.provider = plan.model.provider;
    job.budget = plan.request.maxCost;

    // A clone sample is the speaker's own recording: it is written for the upload and removed right after the run.
    std::filesystem::path sample;
    struct RemoveSample {
        std::filesystem::path& file;
        ~RemoveSample() {
            if (!file.empty()) {
                std::error_code ec;
                std::filesystem::remove(file, ec);
            }
        }
    } removeSample{sample};
    if (plan.clone) {
        const MediaItem* media = state.findMedia(plan.clone->media);
        if (media == nullptr) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "the speaker's recording is no longer in the project"));
        }
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        auto written = audio.extractSample(*media, plan.clone->stream, plan.clone->ranges,
                                           directory / ("clone-sample-" + std::to_string(plan.request.cloneSpeaker.value) + ".wav"));
        if (!written) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "the speaker's recording cannot be read: " + written.error().message));
        }
        sample = *written;
        job.cloneSamples.push_back(sample);
    }

    auto run = runDub(job, speech, audio, stop, progress);
    if (!run) {
        return std::unexpected(run.error());
    }
    AssemblyOptions options;
    options.trackName = plan.trackName;
    options.duckDb = plan.duckDb;
    if (plan.request.kind == DubKind::Narration) {
        options.duckRoles = {TrackRole::Dialogue, TrackRole::Music, TrackRole::None};
    }
    auto assembly = assembleDub(state, plan.sequence, run->audio, run->fitted, options);
    if (!assembly) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "the voice-over cannot be placed: " + assembly.error().message));
    }
    return DubOutcome{std::move(*run), std::move(*assembly)};
}

Json planToJson(const DubPlan& plan) {
    Json lines = Json::array();
    for (std::size_t i = 0; i < plan.lines.size() && i < 200; ++i) {
        const DubLine& line = plan.lines[i];
        Json row{{"line", i + 1}, {"text", line.text}};
        if (plan.request.kind == DubKind::Dub) {
            row["cue"] = line.source;
            row["start"] = secondsOf(line.start);
            row["seconds"] = secondsOf(line.duration);
        }
        lines.push_back(std::move(row));
    }
    Json out{{"kind", plan.request.kind == DubKind::Dub ? "dub" : "narration"},
             {"sequence", plan.sequence.value},
             {"provider", plan.model.provider},
             {"model", plan.model.id},
             {"voice", plan.voice},
             {"language", plan.language},
             {"track", plan.trackName},
             {"lines", plan.lines.size()},
             {"characters", plan.characters},
             {"duckDb", plan.duckDb},
             {"maxStretch", plan.request.maxStretch},
             {"estimate", Json{{"usd", usd(plan.cost.expected)}, {"ceilingUsd", usd(plan.cost.ceiling)}, {"priced", plan.cost.priced},
                               {"audioSeconds", static_cast<double>(plan.cost.audioMilliseconds) / 1000.0}}},
             {"script", std::move(lines)},
             {"warnings", plan.warnings}};
    if (plan.request.kind == DubKind::Dub) {
        out["captionTrack"] = plan.request.captionTrack.value;
    } else {
        out["onBeats"] = plan.beats.size() == plan.lines.size() && !plan.beats.empty();
        out["beats"] = plan.beats.size();
    }
    if (plan.clone) {
        out["clone"] = Json{{"speaker", plan.request.cloneSpeaker.value}, {"media", plan.clone->media.value},
                            {"seconds", secondsOf(plan.clone->total)}};
    }
    if (plan.request.maxCost) {
        out["maxCostUsd"] = usd(*plan.request.maxCost);
    }
    return out;
}

Json outcomeToJson(const DubPlan& plan, const DubOutcome& outcome) {
    Json out = planToJson(plan);
    out.erase("script");
    const DubRun& run = outcome.run;
    Json flagged = Json::array();
    Json lines = Json::array();
    for (std::size_t i = 0; i < run.fitted.size(); ++i) {
        const FittedLine& f = run.fitted[i];
        Json row{{"line", i + 1},          {"start", secondsOf(f.start)},  {"seconds", secondsOf(f.duration)},
                 {"generatedSeconds", secondsOf(run.generated[i])},          {"speed", std::round(f.speed * 1000.0) / 1000.0},
                 {"stretched", f.stretched}, {"shifted", f.shifted},         {"flagged", f.flagged}};
        if (!f.note.empty()) {
            row["note"] = f.note;
        }
        if (f.flagged) {
            flagged.push_back(Json{{"line", i + 1}, {"text", run.lines[i].text}, {"note", f.note}});
        }
        if (i < 200) {
            lines.push_back(std::move(row));
        }
    }
    out["media"] = outcome.assembly.media.value;
    out["audioTrack"] = outcome.assembly.track.value;
    out["clips"] = outcome.assembly.clips.size();
    out["duckedClips"] = outcome.assembly.duckedClips;
    out["fit"] = Json{{"fitted", run.report.fitted}, {"stretched", run.report.stretched}, {"shifted", run.report.shifted},
                      {"flagged", run.report.flagged}, {"maxSpeed", std::round(run.report.maxSpeed * 1000.0) / 1000.0}};
    out["flagged"] = std::move(flagged);
    out["placed"] = std::move(lines);
    out["estimatedUsd"] = usd(run.estimated);
    out["cloned"] = run.cloned;
    out["file"] = run.audio.item.locator.path;
    Json warnings = plan.warnings;
    for (const std::string& w : run.warnings) {
        warnings.push_back(w);
    }
    out["warnings"] = std::move(warnings);
    return out;
}

CommandOutput DirectDubServices::dub(const DubPlan& plan, ExecutionContext& c) {
    if (!m_speech || !m_audio) {
        fail(ErrorCode::InvalidRequest, "this host cannot generate speech");
    }
    auto speech = m_speech(plan.model, plan.request.maxCost);
    if (!speech) {
        fail(commandCode(speech.error().code), plan.model.name + ": " + speech.error().message);
    }
    std::stop_source stop;
    std::exception_ptr stopped;
    const std::string what = plan.request.kind == DubKind::Dub ? "dubbing" : "narrating";
    auto outcome = executeDub(c.state(), plan, **speech, *m_audio, m_directory, stop.get_token(), [&](std::size_t done, std::size_t total) {
        if (stopped) {
            return;
        }
        try {
            c.checkpoint(what + " (" + std::to_string(done) + "/" + std::to_string(total) + " lines)");
        } catch (...) {
            stopped = std::current_exception();
            stop.request_stop();
        }
    });
    if (stopped) {
        std::rethrow_exception(stopped);
    }
    if (!outcome) {
        fail(commandCode(outcome.error().code), what + " failed: " + outcome.error().message);
    }
    CommandOutput out;
    out.data = outcomeToJson(plan, *outcome);
    out.createdIds = {outcome->assembly.media.value, outcome->assembly.track.value};
    for (const ClipId id : outcome->assembly.clips) {
        out.createdIds.push_back(id.value);
    }
    out.warnings = out.data["warnings"].get<std::vector<std::string>>();
    out.state = std::move(outcome->assembly.state);
    return out;
}

DubRequest requestFromParams(ExecutionContext& c, DubKind kind) {
    DubRequest r;
    r.kind = kind;
    if (kind == DubKind::Dub) {
        if (c.has("captionTrack")) {
            r.captionTrack = CaptionTrackId{static_cast<std::uint64_t>(c.integer("captionTrack"))};
        }
    } else {
        r.text = c.string("text");
        r.onBeats = c.boolean("onBeats", true);
        r.at = c.optionalTime("at");
        if (c.has("gap")) {
            r.gap = Ticks{static_cast<std::int64_t>(std::llround(c.number("gap") * static_cast<double>(kTicksPerSecond)))};
        }
    }
    r.language = c.string("language", "");
    r.model = c.string("model", "");
    r.voice = c.string("voice", "");
    r.style = c.string("style", "");
    if (c.has("maxStretch")) {
        r.maxStretch = c.number("maxStretch");
    }
    if (c.has("duck")) {
        r.duckDb = c.number("duck");
    }
    r.trackName = c.string("name", "");
    r.cloneVoice = c.boolean("cloneVoice", false);
    r.rightsConfirmed = c.boolean("rightsConfirmed", false);
    if (c.has("speaker")) {
        r.cloneSpeaker = SpeakerId{static_cast<std::uint64_t>(c.integer("speaker"))};
    }
    if (c.has("maxCost")) {
        r.maxCost = static_cast<MicroUsd>(std::llround(c.number("maxCost") * 1e6));
    }
    return r;
}

namespace {

CommandOutput dubCommand(ExecutionContext& c, DubKind kind) {
    DubRequest request = requestFromParams(c, kind);
    auto* services = command::findService<DubServices>(c.services());
    if (request.model.empty() && services != nullptr) {
        request.model = services->defaultModel(request.language);
    }
    auto* transcripts = command::findService<ai::edit::TranscriptServices>(c.services());
    auto plan = planDub(c.state(), c.sequenceId(), request, ModelRegistry::builtin(), transcripts ? transcripts->transcripts.get() : nullptr,
                        todayUtc());
    if (!plan) {
        fail(plan.error().code, plan.error().message);
    }
    if (c.boolean("estimate", false)) {
        CommandOutput out;
        out.state = c.state();
        out.data = planToJson(*plan);
        out.warnings = plan->warnings;
        return out;
    }
    if (services == nullptr) {
        fail(ErrorCode::InvalidRequest, "a voice-over calls the user's paid voice provider: open Dub in the app and approve the voice, cost "
                                        "and consent (\"estimate\": true prices it here)");
    }
    if (!services->proposes()) {
        if (!plan->request.maxCost) {
            fail(ErrorCode::InvalidParams, "a voice-over is paid: pass \"maxCost\", the run's hard cap in USD (\"estimate\": true prices it first)",
                 Json{{"path", "/maxCost"}});
        }
        if (plan->cost.priced && plan->cost.ceiling > *plan->request.maxCost) {
            fail(ErrorCode::PermissionDenied, std::format("this run could cost up to ${:.2f}, more than the cap of ${:.2f}", usd(plan->cost.ceiling),
                                                          usd(*plan->request.maxCost)));
        }
    }
    return services->dub(*plan, c);
}

Json commonParams(Json props) {
    props["language"] = Json{{"type", "string"}, {"minLength", 2}, {"maxLength", 16},
                             {"description", "The language spoken (ISO 639-1); dub default: the caption track's."}};
    props["model"] = Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                          {"description", "Voice model (registry id): eleven_v3 / eleven_v4 (Thai and 70+ languages), eleven_flash_v2_5, "
                                          "gpt-4o-mini-tts, gemini-3.8-flash-tts. Default: the host's, else eleven_v3."}};
    props["voice"] = Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                          {"description", "The model's voice (an ElevenLabs voice id, an OpenAI voice such as \"marin\", a Gemini voice "
                                          "such as \"Kore\"); default: the model's first."}};
    props["style"] = Json{{"type", "string"}, {"maxLength", 300}, {"description", "Delivery instructions (\"warm, unhurried\"), where the model takes them."}};
    props["maxStretch"] = schema::number("How much faster a line may be played to fit (0.15 = 15 %, the default; pitch kept).", 0.0, 0.5);
    props["name"] = Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"description", "The new audio track's name."}};
    props["cloneVoice"] = schema::boolean("Clone the speaker's own voice (ElevenLabs instant cloning) instead of a stock voice. Opt-in; "
                                          "needs rightsConfirmed and speaker; never for a speaker with redacted speech. The clone is "
                                          "deleted after the run.",
                                          false);
    props["rightsConfirmed"] = schema::boolean("The user confirmed: \"I have the speaker's permission\" to clone their voice.", false);
    props["speaker"] = schema::integer("The project speaker whose voice is cloned.", 1);
    props["maxCost"] = schema::number("The run's hard cap in USD: refused before anything is sent when it could cost more, stopped "
                                      "before a line that could pass it. Required for a run.",
                                      0.0, 1000.0);
    props["estimate"] = schema::boolean("Only plan and price the run; nothing is sent or changed (default false).", false);
    props["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    return props;
}

} // namespace

void registerDubCommands(command::CommandRegistry& registry) {
    Json dubProps = commonParams(Json::object());
    dubProps["captionTrack"] = schema::integer("The caption track to speak (usually a translated one); only needed when the sequence has several.", 1);
    dubProps["duck"] = schema::number("How far the original dialogue goes down under the new lines, dB (default -18; 0 = not at all).", -60.0, 0.0);
    Json dubParams{{"type", "object"}, {"properties", std::move(dubProps)}, {"additionalProperties", false}};
    [[maybe_unused]] const auto dub = registry.add(
        command::runsModel({"dub.create", 1, "Dub",
         "AI voice-over in another language with the user's own voice provider (ElevenLabs, OpenAI or Gemini; BYOK): each caption cue "
         "is spoken and fitted to its slot (sped up at most 15 %, pitch kept; else moved and flagged), on a new \"Dub\" audio track "
         "with the Dialogue role, the original dialogue ducked under it, as one undo step. Bleeped words are never spoken. The "
         "audio is marked AI-generated. Paid: \"estimate\" prices it; a run needs \"maxCost\" and the app's approval (standalone "
         "CLI/MCP and the assistant only estimate or open the Dub dialog).",
         command::Capability::EditProject, std::move(dubParams), Json{{"language", "th"}, {"estimate", true}}, {},
         [](ExecutionContext& c) { return dubCommand(c, DubKind::Dub); }}));

    Json narrationProps = commonParams(Json::object());
    narrationProps["text"] = Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 65536},
                                  {"description", "The script: one line per beat (Markdown headings, bullets and [B-roll] cues are dropped)."}};
    narrationProps["onBeats"] = schema::boolean("On a rough cut (roughcut.fromScript), line i over beat i when the counts match (default true).", true);
    narrationProps["at"] = Json{{"type", Json::array({"integer", "string"})},
                                {"description", "Where the first line starts when not on beats (default the sequence's start). Timecode in "
                                                "the sequence's rate (\"00:01:02:03\") or ticks."}};
    narrationProps["gap"] = schema::number("Seconds between lines when not on beats (default 0.5).", 0.0, 10.0);
    narrationProps["duck"] = schema::number("How far the other audio goes down under the narration, dB (default -12; 0 = not at all).", -60.0, 0.0);
    Json narrationParams{{"type", "object"}, {"properties", std::move(narrationProps)}, {"additionalProperties", false}, {"required", Json::array({"text"})}};
    [[maybe_unused]] const auto narration = registry.add(
        command::runsModel({"narration.create", 1, "Narration",
         "Narration from a script with the user's own voice provider (BYOK): each line spoken, over its rough-cut beat when the "
         "sequence is a rough cut (AI-11) or one after another, on a new \"Narration\" audio track with the Dialogue role and the "
         "other audio ducked under it, as one undo step. The audio is marked AI-generated. Paid: \"estimate\" prices it; a run needs "
         "\"maxCost\" and the app's approval.",
         command::Capability::EditProject, std::move(narrationParams), Json{{"text", "Welcome to Chiang Mai."}, {"estimate", true}}, {},
         [](ExecutionContext& c) { return dubCommand(c, DubKind::Narration); }}));
}

} // namespace wabcut::ai::dub
