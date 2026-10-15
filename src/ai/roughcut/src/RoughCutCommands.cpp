#include <wabcut/ai/roughcut/RoughCutCommands.h>

#include <wabcut/ai/Text.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/roughcut/Outline.h>
#include <wabcut/ai/search/SearchCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace wabcut::ai::roughcut {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

double rounded(double v, double scale = 1000.0) { return std::round(v * scale) / scale; }
Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

edit::TranscriptServices& transcriptsOf(ExecutionContext& c) {
    auto* services = command::findService<edit::TranscriptServices>(c.services());
    if (!services || !services->transcripts) {
        fail(ErrorCode::InvalidRequest, "transcripts are not available here: the host provides none");
    }
    return *services;
}

RoughCutServices* roughCutServicesOf(ExecutionContext& c) { return command::findService<RoughCutServices>(c.services()); }

/// The text encoder: the host's, else the footage search's (when its model is installed).
RoughCutServices::TextEmbedder embedderOf(ExecutionContext& c) {
    if (auto* services = roughCutServicesOf(c); services && services->embed) {
        return services->embed;
    }
    auto* search = command::findService<search::SearchServices>(c.services());
    if (!search) {
        return {};
    }
    auto opened = search->search();
    if (!opened || !*opened || !(*opened)->embedder()) {
        return {};
    }
    std::shared_ptr<search::Embedder> embedder = (*opened)->embedder();
    return [embedder](const std::string& text) -> std::optional<std::vector<float>> {
        auto v = embedder->embedText(text, {});
        if (!v) {
            return std::nullopt;
        }
        return std::move(*v);
    };
}

/// Take audio: the host's probe, else the transcript services' energy probe (level only).
std::shared_ptr<TakeAudioProbe> audioOf(ExecutionContext& c) {
    if (auto* services = roughCutServicesOf(c); services && services->audio) {
        return services->audio;
    }
    if (auto* transcripts = command::findService<edit::TranscriptServices>(c.services()); transcripts && transcripts->energy) {
        return std::make_shared<EnergyTakeProbe>(transcripts->energy);
    }
    return nullptr;
}

std::string mediaName(const ProjectState& state, MediaId id) {
    const MediaItem* m = state.findMedia(id);
    return m ? m->name : std::string();
}

Json takeJson(const BeatTake& take, std::size_t index, bool chosen, const ProjectState& state) {
    Json j{{"take", index + 1},
           {"chosen", chosen},
           {"media", take.media.media.value},
           {"mediaName", mediaName(state, take.media.media)},
           {"sourceIn", rounded(take.media.source.start.toSeconds())},
           {"sourceOut", rounded(take.media.source.end().toSeconds())},
           {"sourceInTicks", take.media.source.start.value},
           {"sourceOutTicks", take.media.source.end().value},
           {"text", take.text},
           {"match", rounded(take.match)},
           {"score", rounded(take.quality.score)},
           {"fillers", take.quality.fillers},
           {"falseStarts", take.quality.falseStarts},
           {"hesitations", take.quality.hesitations},
           {"complete", take.quality.startsSentence && take.quality.endsSentence},
           {"clipped", take.quality.clipped},
           {"reasons", take.quality.reasons}};
    if (take.quality.rmsDb && std::isfinite(*take.quality.rmsDb)) {
        j["levelDb"] = rounded(*take.quality.rmsDb, 10.0);
    }
    if (take.quality.noiseDb && std::isfinite(*take.quality.noiseDb)) {
        j["noiseDb"] = rounded(*take.quality.noiseDb, 10.0);
    }
    if (take.byMeaning) {
        j["byMeaning"] = true;
    }
    return j;
}

Json diffJson(const std::vector<DiffWord>& diff) {
    Json out = Json::array();
    for (const DiffWord& w : diff) {
        out.push_back(Json{{"op", w.kind == DiffWord::Kind::Equal ? "equal" : w.kind == DiffWord::Kind::Inserted ? "insert" : "delete"},
                           {"text", w.text}});
    }
    return out;
}

/// The media of a sequence's audio clips (the recordings it was cut from).
std::vector<MediaId> mediaOfSequence(const Sequence& seq) {
    std::vector<MediaId> out;
    std::set<std::uint64_t> seen;
    for (const TrackPtr& t : seq.audioTracks) {
        for (const Clip& clip : t->clips) {
            if (seen.insert(clip.media.value).second) {
                out.push_back(clip.media);
            }
        }
    }
    return out;
}

std::string readScriptFile(ExecutionContext& c, const std::string& path) {
    auto* services = roughCutServicesOf(c);
    if (!services || !services->readFiles) {
        fail(ErrorCode::PermissionDenied, "this client may not read files: pass the script as \"text\"");
    }
    const std::filesystem::path file(std::u8string(path.begin(), path.end()));
    std::string extension = pathToUtf8(file.extension());
    std::ranges::transform(extension, extension.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    if (extension != ".txt" && extension != ".md" && extension != ".markdown" && extension != ".fountain") {
        fail(ErrorCode::InvalidParams, "\"file\" must be a .txt, .md or .fountain file", Json{{"path", "/file"}});
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        fail(ErrorCode::NotFound, "cannot read the script file: " + ec.message(), Json{{"path", "/file"}});
    }
    if (size > ScriptOptions{}.maxBytes) {
        fail(ErrorCode::InvalidParams, "the script file is larger than 256 KiB", Json{{"path", "/file"}});
    }
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    if (!in.good() && !in.eof()) {
        fail(ErrorCode::NotFound, "cannot read the script file", Json{{"path", "/file"}});
    }
    return text.str();
}

/// B-roll for the beats: their visual cues, or (with no cue) the beat's words when the footage clearly shows them.
std::vector<BrollSuggestion> findBroll(ExecutionContext& c, const RoughCutPlan& plan, std::vector<std::string>& warnings) {
    std::vector<BrollSuggestion> out;
    auto* services = command::findService<search::SearchServices>(c.services());
    if (!services) {
        warnings.emplace_back("no footage search here: no B-roll was suggested");
        return out;
    }
    auto opened = services->search();
    if (!opened || !*opened) {
        warnings.push_back("the footage search is not available (" + (opened ? std::string("none") : opened.error().message) + "): no B-roll");
        return out;
    }
    search::FootageSearch& footage = **opened;
    if (!footage.embedder()) {
        warnings.emplace_back("visual search is off (its model is not installed): B-roll suggestions need it");
        return out;
    }
    std::set<std::uint64_t> aroll;
    for (const BeatPlan& beat : plan.beats) {
        for (const BeatTake& take : beat.takes) {
            aroll.insert(take.media.media.value);
        }
    }
    search::SearchFilters filters;
    filters.transcript = false;
    for (const MediaPtr& m : c.state().media) {
        if (m->info.hasVideo() && !m->isGenerator() && !aroll.contains(m->id.value)) {
            filters.media.push_back(m->id);
        }
    }
    if (filters.media.empty()) {
        warnings.emplace_back("no other video in the project to use as B-roll");
        return out;
    }
    std::set<std::pair<std::uint64_t, std::int64_t>> used;
    for (std::size_t b = 0; b < plan.beats.size(); ++b) {
        const BeatPlan& beat = plan.beats[b];
        if (!beat.found()) {
            continue;
        }
        const bool cued = !beat.beat.broll.empty();
        const std::string query = cued ? beat.beat.broll.front() : beat.beat.text;
        c.checkpoint("looking for B-roll for beat " + std::to_string(b + 1));
        auto results = footage.search(c.state(), query, filters, 5);
        if (!results) {
            continue;
        }
        // A cue asks for B-roll, so any real match will do; plain words need a confident picture (SigLIP 2: 0.12+).
        const float floor = cued ? 0.06f : 0.12f;
        for (const search::SearchHit& hit : results->hits) {
            if (!hit.similarity || *hit.similarity < floor || used.contains({hit.media.value, hit.range.start.value})) {
                continue;
            }
            used.insert({hit.media.value, hit.range.start.value});
            out.push_back(BrollSuggestion{b, hit.media, hit.range, query, *hit.similarity});
            break;
        }
    }
    return out;
}

/// The timeline place of each beat's chosen take in the made sequence (after any clean-up).
std::vector<std::optional<TimeRange>> beatRangesIn(const ProjectState& state, SequenceId sequence, const RoughCutPlan& plan,
                                                   const edit::TranscriptProvider& transcripts) {
    std::vector<std::optional<TimeRange>> out(plan.beats.size());
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return out;
    }
    const edit::SequenceTranscript words = edit::SequenceTranscript::build(state, sequence, transcripts);
    std::map<std::tuple<std::uint64_t, std::int64_t, std::int64_t>, std::size_t> occurrences;
    for (std::size_t b = 0; b < plan.beats.size(); ++b) {
        if (!plan.beats[b].found()) {
            continue;
        }
        const MediaRange& take = plan.beats[b].takes.front().media;
        const auto key = std::tuple{take.media.value, take.source.start.value, take.source.duration.value};
        const auto found = wordsOfTake(state, *seq, words, take, occurrences[key]++);
        if (found) {
            out[b] = TimeRange::fromStartEnd(words[found->first].timeline.start, words[found->second].timeline.end());
        }
    }
    return out;
}

// --- roughcut.fromScript -----------------------------------------------------------------------------------------------

CommandOutput fromScriptCommand(ExecutionContext& c) {
    const bool hasText = c.has("text");
    const bool hasFile = c.has("file");
    if (hasText == hasFile) {
        fail(ErrorCode::InvalidParams, "pass the script as either \"text\" or \"file\"");
    }
    const std::string input = hasText ? c.string("text") : readScriptFile(c, c.string("file"));
    ScriptOptions scriptOptions;
    const std::string split = c.string("split", "auto");
    scriptOptions.split = split == "lines" ? ScriptSplit::Lines : split == "sentences" ? ScriptSplit::Sentences : ScriptSplit::Auto;
    auto parsed = parseScript(input, scriptOptions);
    if (!parsed) {
        fail(ErrorCode::InvalidParams, parsed.error().message);
    }
    Script script = std::move(*parsed);
    if (script.beats.empty()) {
        fail(ErrorCode::InvalidParams, "the script has no lines to look for");
    }
    edit::TranscriptServices& transcripts = transcriptsOf(c);
    std::vector<MediaId> scope = c.mediaList("media");
    std::optional<SequenceId> settingsFrom;
    if (c.has("fromSequence")) {
        const SequenceId from{static_cast<std::uint64_t>(c.integer("fromSequence"))};
        const Sequence* seq = c.state().findSequence(from);
        if (!seq) {
            fail(ErrorCode::NotFound, "no sequence " + std::to_string(from.value), Json{{"path", "/fromSequence"}});
        }
        settingsFrom = from;
        for (const MediaId m : mediaOfSequence(*seq)) {
            scope.push_back(m);
        }
        if (scope.empty()) {
            fail(ErrorCode::EditRejected, "sequence '" + seq->name + "' has no audio clips to take lines from");
        }
    }
    c.checkpoint("reading the transcripts");
    RoughCutOptions options;
    if (c.has("minMatch")) {
        options.minMatch = c.number("minMatch");
    }
    options.preference = takePreferenceFromString(c.string("prefer", "best")).value_or(TakePreference::Best);
    const SpeechCorpus corpus = mediaCorpus(c.state(), scope, *transcripts.transcripts, transcripts.energy.get(), options.corpus);
    if (corpus.sources.empty()) {
        fail(ErrorCode::EditRejected, "none of the media has a transcript: transcribe the recordings first (transcript.transcribe)");
    }
    std::vector<std::string> warnings = corpus.warnings;
    warnings.insert(warnings.end(), script.warnings.begin(), script.warnings.end());

    // Optional: a loose outline interpreted by the user's model (transcript text only, guarded).
    Json interpreted = nullptr;
    const bool outline = std::ranges::any_of(script.beats, [](const ScriptBeat& b) { return b.outline; });
    if (c.boolean("interpret", false)) {
        auto* services = roughCutServicesOf(c);
        if (c.state().aiNoCloud) {
            warnings.emplace_back("this project is locked to no-cloud AI: the outline was matched on this computer only");
        } else if (!services || !services->chat) {
            warnings.emplace_back("no cloud chat model here: the outline was matched on this computer only");
        } else if (!outline) {
            warnings.emplace_back("the script has no outline bullets: it was matched as written");
        } else if (auto chat = services->chat(); !chat) {
            warnings.push_back("the outline was not interpreted (" + chat.error().message + "): matched as written");
        } else {
            c.checkpoint("asking " + (*chat)->spec().id + " to place the outline's bullets");
            auto outcome = interpretOutline(**chat, script, corpus);
            if (outcome) {
                interpreted = Json{{"model", outcome->model}, {"bullets", outcome->bullets}, {"beats", outcome->beats}, {"charsSent", outcome->charsSent}};
                warnings.insert(warnings.end(), outcome->script.warnings.begin(), outcome->script.warnings.end());
                script = std::move(outcome->script);
            } else {
                warnings.push_back("the outline was not interpreted (" + outcome.error().message + "): matched as written");
            }
        }
    }

    c.checkpoint("finding the beats in " + std::to_string(corpus.words) + " words");
    const std::shared_ptr<TakeAudioProbe> audio = audioOf(c);
    RoughCutSignals inputs;
    inputs.audio = audio.get();
    if (outline) {
        inputs.embed = embedderOf(c);
    }
    RoughCutPlan plan = planRoughCut(corpus, script, inputs, options);
    warnings.insert(warnings.end(), plan.warnings.begin(), plan.warnings.end());
    if (plan.found() == 0) {
        fail(ErrorCode::EditRejected, "no line of the script was found in the transcripts of " + std::to_string(corpus.sources.size()) +
                                          " media items: check the scope or transcribe the right recordings",
             Json{{"report", planToJson(plan, c.state())}});
    }
    AssemblyOptions assembly;
    assembly.name = c.string("name");
    assembly.settingsFrom = settingsFrom;
    assembly.sectionMarkers = c.boolean("markers", true);
    if (c.boolean("broll", false)) {
        c.checkpoint("looking for B-roll");
        assembly.broll = findBroll(c, plan, warnings);
    }
    auto assembled = assembleRoughCut(c.state(), plan, assembly);
    if (!assembled) {
        fail(ErrorCode::EditRejected, assembled.error().message);
    }
    warnings.insert(warnings.end(), assembled->warnings.begin(), assembled->warnings.end());
    ProjectState state = std::move(assembled->state);
    const SequenceId made = assembled->sequence;
    Json cleanup = nullptr;
    if (c.boolean("cleanup", false)) {
        c.checkpoint("removing fillers and pauses");
        EditSession session(state);
        const std::shared_ptr<command::CommandServices> borrowed(c.services(), [](command::CommandServices*) {});
        command::CommandEngine engine(session, edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 16, borrowed});
        command::CommandRequest request;
        request.command = "transcript.cleanup";
        request.params = Json{{"sequence", made.value}, {"fillers", true},
                              {"silences", c.has("cleanupSilences") ? c.number("cleanupSilences") : 0.8}};
        const command::CommandResult r = engine.execute(request);
        if (r.ok()) {
            state = *session.snapshot();
            cleanup = r.data.is_null() ? Json{{"done", true}} : r.data;
        } else {
            warnings.push_back("clean-up: " + r.error->message);
        }
    }
    const auto ranges = beatRangesIn(state, made, plan, *transcripts.transcripts);
    CommandOutput out;
    out.state = std::move(state);
    out.createdIds.push_back(made.value);
    out.data = planToJson(plan, out.state, &ranges);
    out.data["sequence"] = made.value;
    out.data["name"] = out.state.findSequence(made)->name;
    out.data["clips"] = assembled->clips;
    if (!assembly.broll.empty() || c.boolean("broll", false)) {
        Json broll = Json::array();
        for (const BrollSuggestion& b : assembly.broll) {
            broll.push_back(Json{{"beat", b.beat + 1},
                                 {"media", b.media.value},
                                 {"mediaName", mediaName(out.state, b.media)},
                                 {"sourceIn", rounded(b.source.start.toSeconds())},
                                 {"sourceOut", rounded(b.source.end().toSeconds())},
                                 {"query", b.query},
                                 {"similarity", rounded(b.score)}});
        }
        out.data["broll"] = std::move(broll);
        out.data["brollPlaced"] = assembled->brollPlaced;
    }
    if (!interpreted.is_null()) {
        out.data["interpreted"] = std::move(interpreted);
    }
    if (!cleanup.is_null()) {
        out.data["cleanup"] = std::move(cleanup);
    }
    out.warnings = std::move(warnings);
    return out;
}

// --- roughcut.swapTake -----------------------------------------------------------------------------------------------

CommandOutput swapTakeCommand(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    auto record = recordOfSequence(seq);
    if (!record) {
        fail(ErrorCode::EditRejected, record.error().message);
    }
    const std::size_t beat = c.indexValue(c.params()["beat"], "/beat");
    const std::size_t take = c.indexValue(c.params()["take"], "/take");
    if (beat < 1 || beat > record->beats.size()) {
        fail(ErrorCode::InvalidParams, "the rough cut has beats 1 to " + std::to_string(record->beats.size()), Json{{"path", "/beat"}});
    }
    RecordedBeat& recorded = record->beats[beat - 1];
    if (recorded.takes.empty()) {
        fail(ErrorCode::EditRejected, "beat " + std::to_string(beat) + " was not found in the media: it has no takes");
    }
    if (take < 1 || take > recorded.takes.size()) {
        fail(ErrorCode::InvalidParams, "beat " + std::to_string(beat) + " has takes 1 to " + std::to_string(recorded.takes.size()),
             Json{{"path", "/take"}});
    }
    CommandOutput out;
    if (take - 1 == recorded.chosen) {
        out.state = c.state();
        out.warnings.push_back("take " + std::to_string(take) + " is already the one in the cut");
        out.data = Json{{"beat", beat}, {"take", take}, {"changed", false}};
        return out;
    }
    edit::TranscriptServices& transcripts = transcriptsOf(c);
    const edit::SequenceTranscript words = edit::SequenceTranscript::build(c.state(), seq.id, *transcripts.transcripts);
    const RecordedTake& current = recorded.takes[recorded.chosen];
    // Earlier beats playing the very same media range come first on the timeline.
    std::size_t occurrence = 0;
    for (std::size_t b = 0; b + 1 < beat; ++b) {
        const RecordedBeat& other = record->beats[b];
        if (!other.takes.empty() && other.takes[other.chosen].media == current.media) {
            ++occurrence;
        }
    }
    const auto found = wordsOfTake(c.state(), seq, words, current.media, occurrence);
    if (!found) {
        fail(ErrorCode::EditRejected, "beat " + std::to_string(beat) +
                                          "'s take is no longer in the sequence (it was deleted or replaced by hand)");
    }
    TakeEditOptions options;
    TakeEdit edit{found->first, found->second, recorded.takes[take - 1].media,
                  "beat " + std::to_string(beat) + ": take " + std::to_string(take)};
    ProjectState state = applyTakeEdits(c.state(), seq.id, words, {edit}, options, edit::editingCommands(), c.control(), c.services());
    const std::size_t previous = recorded.chosen;
    recorded.chosen = take - 1;
    auto next = std::make_shared<Sequence>(*state.findSequence(seq.id));
    next->derivedFrom->params = recordToJson(*record);
    state = state.withSequence(std::move(next));
    out.state = std::move(state);
    out.data = Json{{"beat", beat}, {"take", take}, {"previous", previous + 1}, {"changed", true}};
    return out;
}

// --- takes.find / takes.use -------------------------------------------------------------------------------------------

TakesOptions takesOptionsOf(ExecutionContext& c) {
    TakesOptions options;
    if (c.has("minSimilarity")) {
        options.minSimilarity = c.number("minSimilarity");
    }
    if (c.has("maxGap")) {
        options.maxGap = fromSeconds(c.number("maxGap"));
    }
    options.acrossSources = c.boolean("acrossRecordings", true);
    return options;
}

struct TakesScope {
    SpeechCorpus corpus;
    std::size_t sequenceSource = 0;
};

TakesScope takesScope(ExecutionContext& c, const TakesOptions& options) {
    const Sequence& seq = c.sequence();
    edit::TranscriptServices& transcripts = transcriptsOf(c);
    std::vector<MediaId> media = c.mediaList("media");
    if (c.boolean("includeSourceMedia", true)) {
        for (const MediaId m : mediaOfSequence(seq)) {
            media.push_back(m);
        }
    }
    TakesScope scope;
    if (!media.empty()) {
        scope.corpus = mediaCorpus(c.state(), media, *transcripts.transcripts, transcripts.energy.get(), options.corpus);
    }
    addSequenceSource(scope.corpus, c.state(), seq.id, *transcripts.transcripts, transcripts.energy.get(), options.corpus);
    const auto it = std::ranges::find_if(scope.corpus.sources, [](const SpeechSource& s) { return s.kind == SpeechSource::Kind::Sequence; });
    if (it == scope.corpus.sources.end()) {
        fail(ErrorCode::EditRejected, "sequence '" + seq.name + "' has no transcript: transcribe its media first");
    }
    scope.sequenceSource = static_cast<std::size_t>(it - scope.corpus.sources.begin());
    return scope;
}

TakesReport runFind(ExecutionContext& c, const TakesScope& scope, const TakesOptions& options) {
    c.checkpoint("comparing " + std::to_string(scope.corpus.words) + " words for repeated takes");
    const std::shared_ptr<TakeAudioProbe> audio = audioOf(c);
    TakesSignals inputs;
    inputs.audio = audio.get();
    if (c.boolean("meaning", false)) {
        inputs.embed = embedderOf(c);
    }
    return findTakes(scope.corpus, inputs, options);
}

CommandOutput findTakesCommand(ExecutionContext& c) {
    const TakesOptions options = takesOptionsOf(c);
    const TakesScope scope = takesScope(c, options);
    const TakesReport report = runFind(c, scope, options);
    CommandOutput out;
    out.state = c.state();
    out.data = takesToJson(report, static_cast<std::size_t>(c.integer("limit", 100)));
    out.data["sequence"] = c.sequence().id.value;
    out.warnings = scope.corpus.warnings;
    out.warnings.insert(out.warnings.end(), report.warnings.begin(), report.warnings.end());
    return out;
}

CommandOutput useTakeCommand(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const bool best = c.boolean("best", false);
    if (best == c.has("group")) {
        fail(ErrorCode::InvalidParams, "pass either \"group\" (and \"take\") or \"best\": true");
    }
    const TakesOptions options = takesOptionsOf(c);
    const TakesScope scope = takesScope(c, options);
    const TakesReport report = runFind(c, scope, options);
    TakeEditOptions editOptions;
    std::vector<TakeEdit> edits;
    Json review = Json::array();
    const auto plan = [&](const TakeGroup& group, std::size_t take) {
        auto planned = planUseTake(scope.corpus, group, take, editOptions);
        if (!planned) {
            return planned.error().message;
        }
        for (TakeEdit& e : *planned) {
            // Sequence-source word indices are the sequence's own transcript's.
            edits.push_back(std::move(e));
        }
        review.push_back(Json{{"group", group.id + 1},
                              {"text", group.text},
                              {"take", take + 1},
                              {"source", group.takes[take].sourceName},
                              {"score", rounded(group.takes[take].quality.score)},
                              {"replaced", !group.takes[take].onTimeline},
                              {"removed", group.onTimeline - 1},
                              {"reasons", group.takes[take].quality.reasons}});
        return std::string();
    };
    std::vector<std::string> warnings = scope.corpus.warnings;
    if (best) {
        for (const TakeGroup& group : report.groups) {
            if (group.improvable()) {
                if (std::string problem = plan(group, 0); !problem.empty()) {
                    warnings.push_back("group " + std::to_string(group.id + 1) + ": " + problem);
                }
            }
        }
    } else {
        const std::size_t group = c.indexValue(c.params()["group"], "/group");
        const std::size_t take = c.has("take") ? c.indexValue(c.params()["take"], "/take") : 1;
        if (group < 1 || group > report.groups.size()) {
            fail(ErrorCode::InvalidParams, report.groups.empty() ? std::string("no repeated takes were found in this sequence")
                                                                 : "the groups are 1 to " + std::to_string(report.groups.size()) +
                                                                       " (takes.find lists them)",
                 Json{{"path", "/group"}});
        }
        const TakeGroup& g = report.groups[group - 1];
        if (take < 1 || take > g.takes.size()) {
            fail(ErrorCode::InvalidParams, "group " + std::to_string(group) + " has takes 1 to " + std::to_string(g.takes.size()),
                 Json{{"path", "/take"}});
        }
        if (std::string problem = plan(g, take - 1); !problem.empty()) {
            fail(ErrorCode::EditRejected, problem);
        }
    }
    CommandOutput out;
    if (edits.empty()) {
        out.state = c.state();
        out.warnings = std::move(warnings);
        out.warnings.emplace_back(best ? "every line already uses its best take" : "that take is already the only one in the sequence");
        out.data = Json{{"sequence", seq.id.value}, {"review", std::move(review)}, {"changed", false}};
        return out;
    }
    c.checkpoint("using the chosen takes");
    out.state = applyTakeEdits(c.state(), seq.id, scope.corpus.sources[scope.sequenceSource].transcript, std::move(edits), editOptions,
                               edit::editingCommands(), c.control(), c.services());
    out.data = Json{{"sequence", seq.id.value}, {"review", std::move(review)}, {"changed", true}};
    out.warnings = std::move(warnings);
    return out;
}

Json mediaRefs(std::string description) {
    return schema::array(Json{{"type", Json::array({"integer", "string"})}, {"description", "A media id or exact name."}, {"minimum", 1}},
                         std::move(description), 1);
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

Json takesOptionSchema(Json properties) {
    properties["minSimilarity"] = schema::number("How alike two attempts must be to count as takes of one line, 0.3..1 (default 0.7).", 0.3, 1.0);
    properties["maxGap"] = schema::number("Within one recording, attempts further apart than this (seconds) are not retakes (default 180).", 1.0, 36000.0);
    properties["acrossRecordings"] = schema::boolean("Attempts in different recordings count (default true).", true);
    properties["includeSourceMedia"] = schema::boolean("Also look in the whole recordings the sequence uses, for takes trimmed away (default true).", true);
    properties["media"] = mediaRefs("More media items whose transcripts hold takes.");
    properties["meaning"] = schema::boolean("Also group paraphrased attempts by meaning with the text encoder (slower; default false).", false);
    return properties;
}

} // namespace

// --- Shared -----------------------------------------------------------------------------------------------------------

std::optional<std::pair<std::size_t, std::size_t>> wordsOfTake(const ProjectState& state, const Sequence& sequence,
                                                               const edit::SequenceTranscript& words, const MediaRange& take,
                                                               std::size_t occurrence) {
    std::map<std::uint64_t, std::pair<MediaId, int>> clips;
    for (const TrackPtr& t : sequence.audioTracks) {
        for (const Clip& clip : t->clips) {
            clips.emplace(clip.id.value, std::pair{clip.media, clip.stream.index});
        }
    }
    (void)state;
    std::optional<std::pair<std::size_t, std::size_t>> run;
    std::size_t seen = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto it = clips.find(words[i].clip.value);
        const bool inTake = it != clips.end() && it->second.first == take.media && it->second.second == take.audioStream &&
                            take.source.contains(words[i].source.start + Ticks{words[i].source.duration.value / 2});
        if (inTake) {
            if (run && run->second + 1 == i) {
                run->second = i;
            } else {
                if (run) {
                    if (seen == occurrence) {
                        return run;
                    }
                    ++seen;
                }
                run = std::pair{i, i};
            }
        }
    }
    if (run && seen == occurrence) {
        return run;
    }
    return std::nullopt;
}

Json planToJson(const RoughCutPlan& plan, const ProjectState& state, const std::vector<std::optional<TimeRange>>* ranges) {
    Json beats = Json::array();
    for (std::size_t b = 0; b < plan.beats.size(); ++b) {
        const BeatPlan& beat = plan.beats[b];
        Json takes = Json::array();
        for (std::size_t t = 0; t < beat.takes.size(); ++t) {
            takes.push_back(takeJson(beat.takes[t], t, t == 0, state));
        }
        Json j{{"beat", b + 1},
               {"text", beat.beat.text},
               {"found", beat.found()},
               {"confidence", rounded(beat.confidence)},
               {"takes", std::move(takes)}};
        if (!beat.beat.section.empty()) {
            j["section"] = beat.beat.section;
        }
        if (!beat.beat.broll.empty()) {
            j["cues"] = beat.beat.broll;
        }
        if (!beat.note.empty()) {
            j["note"] = beat.note;
        }
        if (beat.beat.outline) {
            j["outline"] = true;
        }
        if (ranges && b < ranges->size() && (*ranges)[b]) {
            j["start"] = rounded((*ranges)[b]->start.toSeconds());
            j["end"] = rounded((*ranges)[b]->end().toSeconds());
            j["startTicks"] = (*ranges)[b]->start.value;
        }
        beats.push_back(std::move(j));
    }
    Json out{{"title", plan.script.title},
             {"beats", std::move(beats)},
             {"found", plan.found()},
             {"beatCount", plan.beats.size()},
             {"sources", plan.sources},
             {"words", plan.words},
             {"milliseconds", rounded(plan.milliseconds, 10.0)}};
    if (!plan.script.language.empty()) {
        out["language"] = plan.script.language;
    }
    return out;
}

Json takesToJson(const TakesReport& report, std::size_t limit) {
    Json groups = Json::array();
    for (std::size_t g = 0; g < report.groups.size() && g < limit; ++g) {
        const TakeGroup& group = report.groups[g];
        Json takes = Json::array();
        for (std::size_t t = 0; t < group.takes.size(); ++t) {
            const Take& take = group.takes[t];
            Json j{{"take", t + 1},
                   {"best", t == 0},
                   {"onTimeline", take.onTimeline},
                   {"source", take.sourceName},
                   {"start", rounded(take.range.start.toSeconds())},
                   {"end", rounded(take.range.end().toSeconds())},
                   {"text", take.text},
                   {"completeness", rounded(take.completeness)},
                   {"score", rounded(take.quality.score)},
                   {"fillers", take.quality.fillers},
                   {"falseStarts", take.quality.falseStarts},
                   {"hesitations", take.quality.hesitations},
                   {"complete", take.quality.startsSentence && take.quality.endsSentence},
                   {"clipped", take.quality.clipped},
                   {"wordsPerSecond", rounded(take.quality.wordsPerSecond, 100.0)},
                   {"reasons", take.quality.reasons}};
            if (take.onTimeline) {
                j["startTicks"] = take.range.start.value;
            }
            if (take.media) {
                j["media"] = take.media->media.value;
                j["sourceIn"] = rounded(take.media->source.start.toSeconds());
                j["sourceOut"] = rounded(take.media->source.end().toSeconds());
                j["sourceInTicks"] = take.media->source.start.value;
                j["sourceOutTicks"] = take.media->source.end().value;
            }
            if (take.quality.rmsDb && std::isfinite(*take.quality.rmsDb)) {
                j["levelDb"] = rounded(*take.quality.rmsDb, 10.0);
            }
            if (take.quality.peakDb && std::isfinite(*take.quality.peakDb)) {
                j["peakDb"] = rounded(*take.quality.peakDb, 10.0);
            }
            if (take.quality.noiseDb && std::isfinite(*take.quality.noiseDb)) {
                j["noiseDb"] = rounded(*take.quality.noiseDb, 10.0);
            }
            if (!take.diff.empty()) {
                j["diff"] = diffJson(take.diff);
            }
            takes.push_back(std::move(j));
        }
        groups.push_back(Json{{"group", g + 1},
                              {"text", group.text},
                              {"onTimeline", group.onTimeline},
                              {"improvable", group.improvable()},
                              {"confidence", rounded(group.confidence)},
                              {"takes", std::move(takes)}});
    }
    return Json{{"groups", std::move(groups)},
                {"groupCount", report.groups.size()},
                {"improvable", static_cast<std::size_t>(std::ranges::count_if(report.groups, [](const TakeGroup& g) { return g.improvable(); }))},
                {"sentences", report.sentences},
                {"milliseconds", rounded(report.milliseconds, 10.0)}};
}

void registerRoughCutCommands(command::CommandRegistry& registry) {
    using command::Capability;
    Json mediaList = mediaRefs("Media to take the lines from. Default: every media item with a transcript.");
    Json fromScript{
        {"type", "object"},
        {"properties",
         Json{{"text", Json{{"type", "string"}, {"description", "The script, outline or bullet points (plain text or Markdown)."}, {"minLength", 1}, {"maxLength", 262144}}},
              {"file", Json{{"type", "string"}, {"description", "A .txt / .md file holding the script (only where the host allows reading files)."}, {"minLength", 1}, {"maxLength", 4096}}},
              {"media", mediaList},
              {"fromSequence", schema::integer("Take the lines from the media this sequence uses, and its frame size and rate.", 1)},
              {"split", schema::enumeration({"auto", "lines", "sentences"}, "Beats: \"auto\" (bullets and short lines whole, prose per sentence; default), every line, or every sentence.")},
              {"prefer", schema::enumeration({"best", "cleanest", "first", "last"}, "Which take of a line to use: the best overall (default), the cleanest (fewest fillers and false starts), the first or the last recorded.")},
              {"minMatch", schema::number("How closely a take must say the line, 0.2..0.95 (default 0.45).", 0.2, 0.95)},
              {"cleanup", schema::boolean("Also remove fillers and pauses (transcript.cleanup) from the cut (default false).", false)},
              {"cleanupSilences", schema::number("With cleanup: the shortest pause removed, in seconds (default 0.8).", 0.2, 10.0)},
              {"broll", schema::boolean("Suggest B-roll from the footage for lines with visual cues or that the footage clearly shows, on their own track (default false).", false)},
              {"markers", schema::boolean("A marker at each section heading of the script (default true).", true)},
              {"interpret", schema::boolean("Let the user's cloud chat model place loose outline bullets first (transcript text only; needs consent; never for a no-cloud project; default false).", false)},
              {"name", Json{{"type", "string"}, {"description", "The new sequence's name (default \"Rough cut: <title>\")."}, {"maxLength", 200}}}}},
        {"additionalProperties", false}};
    add(registry,
        command::runsModel({"roughcut.fromScript", 1, "Rough Cut from Script",
         "Makes a first cut from a script, an outline or bullet points: each line is found in the transcripts of the "
         "recordings (fuzzy, Thai too), the best take of it chosen (fewest fillers and false starts, complete sentence, "
         "clean audio; alternates kept), and the takes assembled in script order as a new sequence with linked video and "
         "audio trimmed to the words with small handles. Optional: clean-up of fillers and pauses, B-roll suggestions on "
         "their own track. Returns the alignment report (per line: the chosen take, its confidence, the alternates). "
         "Needs transcripts. One undo step; dryRun previews it.",
         Capability::EditProject, fromScript,
         Json{{"text", "# Launch video\nWe built Wabcut for creators.\nIt cuts in seconds, not hours.\n[B-roll: timeline close-up]"}, {"prefer", "cleanest"}},
         {}, fromScriptCommand}));

    Json swap{{"type", "object"},
              {"properties", Json{{"sequence", schema::integer("The rough cut (a sequence made by roughcut.fromScript); only needed when the project has several.", 1)},
                                  {"beat", schema::integer("The line, 1-based as the report numbers it.", 1)},
                                  {"take", schema::integer("The take of that line to use, 1-based as the report lists them.", 1)}}},
              {"required", Json::array({"beat", "take"})},
              {"additionalProperties", false}};
    add(registry,
        {"roughcut.swapTake", 1, "Swap Take",
         "In a rough cut made by roughcut.fromScript, plays another recorded take of one line in place of the current "
         "one; what follows moves to fit (a ripple). The report of roughcut.fromScript numbers the lines and their takes.",
         Capability::EditSequence, swap, Json{{"beat", 3}, {"take", 2}}, {}, swapTakeCommand});

    Json find = takesOptionSchema(Json{{"sequence", schema::integer("The sequence; only needed when the project has several.", 1)},
                                       {"limit", schema::integer("Groups listed at most (default 100).", 1, 500)}});
    add(registry,
        command::runsModel({"takes.find", 1, "Find Takes",
         "Finds repeated attempts of the same line (retakes and false starts) in a sequence and the recordings it uses: "
         "groups of takes, best first, each with its words, a word-by-word difference from the best one, and quality "
         "(fillers, false starts, pauses, complete sentence, level, clipping, noise floor, pace). \"improvable\" groups "
         "would change with takes.use best. Needs transcripts. A read: it changes nothing.",
         Capability::Read, Json{{"type", "object"}, {"properties", std::move(find)}, {"additionalProperties", false}},
         Json{{"minSimilarity", 0.7}}, {}, findTakesCommand}));

    Json use = takesOptionSchema(Json{{"sequence", schema::integer("The sequence; only needed when the project has several.", 1)},
                                      {"group", schema::integer("The group of takes, 1-based as takes.find lists them (with the same options).", 1)},
                                      {"take", schema::integer("The take to use, 1-based (default 1: the best).", 1)},
                                      {"best", schema::boolean("Use the best take of every group (\"Keep best takes\").", false)}});
    add(registry,
        command::runsModel({"takes.use", 1, "Use Take",
         "Makes the sequence say a line once, with the chosen take: other takes of it on the timeline are removed with their "
         "pause, and a take that is not on the timeline replaces the first one there (linked video and audio, with small "
         "handles; what follows moves to fit). \"best\": true does this for every group whose best take is not already the "
         "one heard. Pass the same options as takes.find (and its expectedRevision). One undo step; returns the review list.",
         Capability::EditSequence, Json{{"type", "object"}, {"properties", std::move(use)}, {"additionalProperties", false}},
         Json{{"best", true}}, {}, useTakeCommand}));
}

} // namespace wabcut::ai::roughcut
