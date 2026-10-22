#include <wabcut/ai/generation/GenerationCommands.h>
#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/llm/Tiers.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/ProjectOps.h>
#include <stdexcept>
namespace wabcut::ai::generation {
using namespace command;
namespace {
CommandOutput local(ExecutionContext& c, std::string_view operation) {
    CommandOutput out; out.state = c.state();
    if (operation == "models") {
        out.data = Json::array(); ModelStore store(ModelStore::defaultRoot());
        // AI-15: each tier's fit on this computer now and the recommended one (nothing is downloaded or started).
        const auto probe = llm::probeSystem();
        const llm::Recommendation recommendation = llm::recommend(probe);
        for (const auto& m : builtinModels()) if (m.kind == ModelKind::Llm) {
            Json row{{"id", m.id}, {"license", m.license}, {"licenseCard", m.licenseCard},
                {"downloadBytes", m.size}, {"installed", store.isInstalled(m)}, {"sha256", m.sha256}};
            if (const llm::TierSpec* tier = llm::tierForModel(m.id)) {
                const llm::Admission a = llm::admit(*tier, m, probe);
                row["tier"] = std::string(tier->id);
                row["context"] = a.context;
                row["fits"] = a.fits;
                row["gpu"] = a.gpu;
                row["ramBytes"] = a.ramNeeded;
                row["vramBytes"] = a.vramNeeded;
                row["reason"] = a.reason;
                row["recommended"] = recommendation.tier.has_value() && *recommendation.tier == tier->id;
            }
            out.data.push_back(std::move(row));
        }
    } else if (auto* host = findService<GenerationServices>(c.services())) out.data = host->local(operation, c.params());
    else if (operation == "status") out.data = {{"state", "unavailable"}, {"reason", "host has no managed runtime service"}};
    else fail(command::ErrorCode::InvalidRequest, "this host has no managed runtime service");
    return out;
}
CommandOutput generate(ExecutionContext& c, std::string_view operation) {
    auto* host = findService<GenerationServices>(c.services());
    if (!host) fail(command::ErrorCode::InvalidRequest, "Open Generate and approve this exact request and cost first");
    return host->generate(operation, c);
}
}
void registerGenerationCommands(CommandRegistry& registry) {
    const auto add = [&](CommandSpec s) { auto r = registry.add(std::move(s)); if (!r) throw std::logic_error(r.error()); };
    for (const std::string operation : {"status", "start", "stop", "models"}) {
        Json props = Json::object();
        if (operation == "start") props["model"] = schema::string("Installed allow-listed model id.");
        Json params{{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
        // Starting loads gigabytes of weights into a new process: a heavy local job like transcription, never a read
        // (review wave 4, P2-1). A read-only MCP client may only look.
        const bool control = operation == "start" || operation == "stop";
        CommandSpec spec{"llm.local." + operation, 1, "Local assistant " + operation,
            control ? "Start or stop the managed local assistant (an installed, allow-listed model); never downloads implicitly."
                    : "Inspect the managed local assistant and its installable models; never downloads implicitly.",
            control ? Capability::MediaAnalysis : Capability::Read,
            params, Json::object(), {}, [operation](ExecutionContext& c) { return local(c, operation); }};
        spec.runsModel = operation == "start"; // C.12: starting runs the model
        add(std::move(spec));
    }
    add({"provenance.report", 1, "Generated media provenance", "Unsigned report of generated segments; no prompts or credentials.",
        Capability::Read, schema::object({{"sequence", schema::integer("Sequence id.", 1)}}), Json::object(), {},
        [](ExecutionContext& c) { CommandOutput out; out.state = c.state(); out.data = provenanceReport(c.state(), c.sequenceId()); return out; }});
    for (const std::string operation : {"extend", "broll", "image", "sfx"}) {
        Json props{{"prompt", schema::string("What to generate.")}, {"sequence", schema::integer("Sequence id.", 1)}};
        std::vector<std::string> required;
        if (operation == "extend") {
            props["clip"] = schema::integer("Original clip id.", 1);
            props["side"] = schema::string("head or tail");
            props["side"]["enum"] = Json::array({"head", "tail"});
            props["seconds"] = schema::number("Extension seconds.", 0.1, 10.0);
            required = {"clip", "side", "seconds"};
        } else if (operation == "broll") {
            props["duration"] = schema::number("Duration in seconds.", 0.1, 10.0);
            props["aspect"] = schema::string("16:9 or 9:16");
            props["aspect"]["enum"] = Json::array({"16:9", "9:16"});
            props["at"] = schema::integer("Placement in timeline ticks.", 0);
            required = {"prompt", "duration", "aspect"};
        } else if (operation == "sfx") {
            // 0.2: a sound effect from a prompt (Premiere's Generative Media), through the user's own key.
            props["duration"] = schema::number("Seconds (0.5 to 30); omit and the model picks the length.", 0.5, 30.0);
            props["loop"] = schema::boolean("A seamless loop (ambience, engine hum). Default false.", false);
            props["at"] = schema::integer("Placement in timeline ticks, on a new audio track.", 0);
            required = {"prompt"};
        } else {
            props["size"] = schema::string("Image output tier, currently 720p.");
            props["size"]["enum"] = Json::array({"720p"});
            required = {"prompt", "size"};
        }
        Json params{{"type", "object"}, {"properties", props}, {"additionalProperties", false}, {"required", required}};
        add(command::runsModel({"gen." + operation, 1, "Generate " + operation,
            "App: opens a human-reviewed Generate proposal with consent and cost preview; acceptance imports one staged edit. Standalone CLI/MCP refuses paid generation without an approval host.",
            Capability::EditProject, params,
            operation == "extend" ? Json{{"clip", 1}, {"side", "tail"}, {"seconds", 4}} :
            operation == "broll" ? Json{{"prompt", "Aerial city"}, {"duration", 4}, {"aspect", "16:9"}} :
            operation == "sfx" ? Json{{"prompt", "Footsteps on gravel, slow"}, {"duration", 4}} :
            Json{{"prompt", "Soft paper background"}, {"size", "720p"}}, {}, [operation](ExecutionContext& c) { return generate(c, operation); }}));
    }
}
std::expected<CommandOutput, Error> stageAsset(const ProjectState& original, SequenceId sequenceId, MediaItem asset,
    Ticks duration, std::optional<Ticks> at, std::optional<ClipId> extend, bool head) {
    const auto invalid = [](std::string text) { return std::unexpected(Error{wabcut::ErrorCode::InvalidArgument, std::move(text)}); };
    if (!asset.generated) return invalid("generated media must have provenance");
    const auto* sequence = original.findSequence(sequenceId);
    if (!sequence) return invalid("sequence not found");
    if (duration.value <= 0) return invalid("duration must be positive");
    TrackId track;
    std::optional<Clip> source;
    if (extend) {
        for (const auto& t : sequence->videoTracks) for (const auto& clip : t->clips) if (clip.id == *extend) {
            if (t->locked) return invalid("source track is locked");
            track = t->id; source = clip;
        }
        if (!source) return invalid("extension requires a video clip");
        at = head ? source->start() - duration : source->end();
        if (at->value < 0) return invalid("there is no room before the clip");
        for (const auto& clip : sequence->findTrack(track)->clips)
            if (clip.start() < *at + duration && clip.end() > *at) return invalid("extension would overwrite another clip");
        asset.generated->source = source->media;
        asset.generated->operation = head ? "extend-head" : "extend-tail";
    }
    // 0.2: a generated sound effect (audio only) is placed on an audio track of its own; it never extends a clip.
    const bool sound = asset.info.video.empty() && !asset.info.audio.empty();
    if (sound && extend) return invalid("a sound effect cannot extend a clip");
    if ((at || extend) && !sound) {
        if (asset.info.video.empty()) return invalid("placed asset has no video");
        const auto& video = asset.info.video.front();
        if (video.width != sequence->width || video.height != sequence->height || video.frameRate != sequence->frameRate)
            return invalid("generated video must be conformed to the sequence before placement");
    }
    auto imported = wabcut::edit::addMedia(original, std::move(asset));
    if (!imported) return std::unexpected(imported.error());
    CommandOutput out; out.state = std::move(imported->state); out.createdIds.push_back(imported->id.value);
    out.data = {{"media", imported->id.value}, {"aiGenerated", true}};
    if (!at) return out; // Library-only import
    if (!track.isValid()) {
        auto added = wabcut::edit::addTrack(out.state, sequenceId, sound ? TrackKind::Audio : TrackKind::Video,
                                            sound ? "Generated sound" : "Generated B-roll");
        if (!added) return std::unexpected(added.error());
        track = added->id; out.state = std::move(added->state);
    }
    wabcut::edit::SourceClip clip{imported->id, {Ticks{}, duration}, {{{sound ? StreamKind::Audio : StreamKind::Video, 0}, track}}};
    auto placed = wabcut::edit::overwriteEdit(out.state, sequenceId, clip, *at);
    if (!placed) return std::unexpected(placed.error());
    out.state = std::move(placed->state);
    if (source && !placed->placed.empty()) {
        std::vector<ClipId> linked{source->id, placed->placed.front()};
        if (source->linkGroup.isValid()) {
            const auto collect = [&](const auto& tracks) {
                for (const auto& t : tracks) for (const auto& c : t->clips)
                    if (c.id != source->id && c.linkGroup == source->linkGroup) linked.push_back(c.id);
            };
            collect(sequence->videoTracks); collect(sequence->audioTracks);
        }
        auto link = wabcut::edit::linkClips(out.state, sequenceId, linked);
        if (!link) return std::unexpected(link.error());
        out.state = std::move(link->state);
    }
    for (auto id : placed->placed) out.createdIds.push_back(id.value);
    return out;
}
}
