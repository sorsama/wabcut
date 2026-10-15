// CommandSpec::runsModel across the catalog (C.12): extensions need MediaAnalysis for the commands that run an AI model,
// so every command the AI modules add is classified here. A new command fails this test until it is either flagged
// (command::runsModel(...) where it is registered) or listed as running none, with a reason.

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Registry.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace wabcut;

namespace {

/// Commands that can run a model: always, or with some parameters, hosts or caches (the C.12 audit).
const std::set<std::string> kRunsModel{
    "transcript.transcribe",                       // whisper (local) or a cloud transcriber
    "llm.local.start",                             // loads the local assistant
    "gen.extend", "gen.broll", "gen.image",        // generation, through the host after approval
    "audio.enhanceSpeech", "audio.separateStems",  // DeepFilterNet3, HTDemucs (wabcut-embed)
    "audio.essentialSound",                        // options.repair.enhance runs Enhance Speech
    "search.footage", "search.indexMedia",         // SigLIP 2 embeddings
    "analysis.subjects", "reframe.createSequence", "reframe.rerun", // face / person detectors
    "shorts.find", "shorts.create",                // detectors, the cloud reranker
    "roughcut.fromScript", "takes.find", "takes.use", // the chat model, text embeddings
    "captions.translate",                          // OPUS-MT or a cloud model
    "dub.create", "narration.create",              // speech synthesis
    "analysis.speakers", "multicam.autoSwitch",    // the face detector
    "mask.objectSelect", "mask.refine", "mask.propagate", // SAM 2.1 (AI-14)
    "workflow.podcast",                            // transcribe, enhance, switch, shorts when run
};

/// Commands the AI modules add that run no model: they read caches, edit, or measure.
const std::set<std::string> kNoModel{
    // Cached transcripts.
    "transcript.text", "transcript.words", "transcript.review", "transcript.speakers", "transcript.corrections",
    "transcript.deleteRanges", "transcript.keepRanges", "transcript.moveRange", "transcript.cleanup",
    "transcript.correctWord", "transcript.correctRange", "captions.generate", "captions.generateFromTranscript",
    "captions.updateFromTranscript",
    // Masks, tracking (Lucas-Kanade) and redactions.
    "mask.add", "mask.update", "mask.remove", "mask.list", "track.mask", "track.transform", "redaction.add",
    "redaction.update", "redaction.remove", "redaction.override", "redaction.list",
    // The local assistant's status, and provenance.
    "llm.local.status", "llm.local.stop", "llm.local.models", "provenance.report",
    // DSP analysis.
    "analysis.detectShots", "clip.splitAtShots", "analysis.detectBeats",
    // Audio: effects, levels, loudness (libebur128), ducking, sync (GCC-PHAT), remix (DSP), plugins.
    "track.setRole", "track.setVolume", "track.setPan", "sequence.setMasterGain", "audio.effects", "audio.addEffect",
    "audio.setEffect", "audio.removeEffect", "audio.moveEffect", "audio.addKeyframe", "audio.removeKeyframe",
    "audio.moveKeyframe", "audio.loudness", "audio.normalize", "audio.masterLimiter", "audio.autoDuck",
    "audio.syncClips", "audio.remix", "audio.remixRender", "plugins.list", "plugins.scan",
    // Voice-over (A.8): record arming and recorded takes.
    "track.setRecordArm", "record.takes", "record.useTake",
    // Channel mapping and surround (A.9): pure parameter edits and measurements.
    "audio.channels", "audio.channelLevels", "audio.setChannels", "audio.setMediaChannels", "audio.setChannelVolume",
    "audio.splitToMono", "audio.fillChannel", "track.setFormat", "track.setSurroundPan", "sequence.setMasterFormat",
    // Colour: parameters, files and statistics.
    "color.grade.set", "color.grade.get", "lut.import", "lut.list", "cdl.export", "cdl.import", "scopes.read",
    "color.auto", "color.match", "color.matchScene",
    // Edits from cached results; frame differencing.
    "roughcut.swapTake", "autozoom.apply",
    // Gameplay highlights (G.2): DSP measurements, chat logs, markers and edits.
    "highlights.find", "highlights.importChat", "highlights.toReel", "highlights.toShort",
};

/// Namespaces whose every command runs a model; llm.* except its listed exceptions.
const std::set<std::string> kModelNamespaces{"gen", "search", "reframe", "shorts", "takes", "dub", "narration",
                                             "workflow"};

std::string namespaceOf(const std::string& name) { return name.substr(0, name.find('.')); }

} // namespace

TEST_CASE("Every command the AI modules add is classified as running a model or not", "[script][runsModel]") {
    const command::CommandRegistry& editing = ai::edit::editingCommands();
    const command::CommandRegistry& core = command::builtinCommands();
    std::set<std::string> seen;
    for (const command::CommandSpec& spec : editing.all()) {
        INFO(spec.name);
        if (core.knows(spec.name)) {
            CHECK_FALSE(spec.runsModel); // the core editing catalog never runs a model
            continue;
        }
        seen.insert(spec.name);
        const bool listedModel = kRunsModel.contains(spec.name);
        const bool listedNone = kNoModel.contains(spec.name);
        CHECK(listedModel != listedNone); // classified, exactly once
        CHECK(spec.runsModel == listedModel);
        const std::string ns = namespaceOf(spec.name);
        if (kModelNamespaces.contains(ns) || (ns == "llm" && !listedNone)) {
            CHECK(spec.runsModel);
        }
    }
    // The lists name only real commands.
    for (const auto* list : {&kRunsModel, &kNoModel}) {
        for (const std::string& name : *list) {
            INFO(name);
            CHECK(seen.contains(name));
        }
    }
}

TEST_CASE("Every command that names files in its parameters is flagged; so extensions never reach a file", "[script][touchesFiles]") {
    const std::set<std::string> kTouchesFiles{"history.export", "review.exportPackage", "review.importPackage",
                                              "project.merge", "cdl.export", "cdl.import", "lut.import",
                                              "plugins.scan", "lottie.import", "interchange.export", "highlights.importChat"};
    // Parameters that look like files but are not, or that only the host can open.
    const std::set<std::string> kExempt{
        "mask.add", "mask.update",            // "path": a mask's outline, not a file
        "autozoom.apply", "roughcut.fromScript", // "sidecar" / "file": read only where the host's services allow
                                                // (a local caller); extensions get the services of a remote client
    };
    const std::set<std::string> fileish{"path", "file", "files", "folder", "folders", "addFolders", "removeFolders",
                                        "sidecar", "base", "theirs", "directory", "output"};
    for (const command::CommandSpec& spec : ai::edit::editingCommands().all()) {
        INFO(spec.name);
        CHECK(spec.touchesFiles == kTouchesFiles.contains(spec.name));
        bool namesFile = false;
        const command::Json properties = spec.params.value("properties", command::Json::object());
        for (const auto& [key, schema] : properties.items()) {
            // A string (or list of strings) under a file-like name; enumerations ("output": "edits" | "take") are not.
            const auto typeIs = [](const command::Json& s, const char* type) {
                return s.contains("type") && s["type"].is_string() && s["type"] == type; // "type" may be a list
            };
            const bool text = typeIs(schema, "string") ||
                              (typeIs(schema, "array") && schema.contains("items") && typeIs(schema["items"], "string"));
            namesFile = namesFile || (fileish.contains(key) && text && !schema.contains("enum"));
        }
        if (namesFile) {
            CHECK((spec.touchesFiles || kExempt.contains(spec.name)));
        }
    }
}
