// 0.7 (card 047, docs/audio.md "Spectral Repair"): the commands behind the Spectral tab of Audio Tools. A clip's repairs are
// one effect, wabcut.audio.spectralRepair, whose data is the edit list (wabcut/model/SpectralRepair.h): every command here
// reads that list, changes it and writes it back, one undo step each.
//   audio.spectralAttenuate / audio.spectralHeal / audio.spectralNoiseFloor / audio.spectralRemoveTone  add an edit
//   audio.spectralRepairRemove                                                                          remove one or all
//   audio.spectralRepairOptions                                                                         FFT size, listen mode, bypass
//   audio.spectralRepairEdits                                                                           the edits of a clip (read-only)
// The effect is created at the start of the clip's stack (after an Essential Sound record) on the first edit, so the repair
// hears the clip before any gain or EQ, and removed with the last edit.

#include "AudioCommandHelpers.h"

#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/model/SpectralRepair.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace wabcut::audio::edit {

namespace {

using namespace detail;

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

/// The longest region Heal looks around (SpectralRepair.cpp's reach, seconds).
constexpr double kHealReachSeconds = 0.88;

ClipId audioClipOf(ExecutionContext& c) {
    if (!c.has("clip")) {
        fail(ErrorCode::InvalidParams, "\"clip\" names the audio clip", Json{{"path", "/clip"}});
    }
    const std::vector<ClipId> clips = audioClipsOf(c, {c.clip("clip")});
    if (clips.size() != 1) {
        fail(ErrorCode::InvalidParams, "\"clip\" names one clip with audio", Json{{"path", "/clip"}});
    }
    return clips.front();
}

std::vector<SpectralPoint> pointsOf(ExecutionContext& c) {
    std::vector<SpectralPoint> out;
    if (!c.has("points")) {
        return out;
    }
    const Json& list = c.params().at("points");
    if (!list.is_array()) {
        fail(ErrorCode::InvalidParams, "\"points\" is a list of {time, hz}", Json{{"path", "/points"}});
    }
    for (const Json& p : list) {
        if (!p.is_object() || !p.contains("time") || !p.contains("hz") || !p["time"].is_number() || !p["hz"].is_number()) {
            fail(ErrorCode::InvalidParams, "a point is {\"time\": seconds, \"hz\": frequency}", Json{{"path", "/points"}});
        }
        out.push_back({p["time"].get<double>(), p["hz"].get<double>()});
    }
    return out;
}

/// The edit a request describes, `kind` fixed by the command.
SpectralEdit editOf(ExecutionContext& c, SpectralKind kind) {
    SpectralEdit e;
    e.kind = kind;
    const std::string shape = c.string("shape", "rectangle");
    const auto parsed = spectralShapeFromString(shape);
    if (!parsed) {
        fail(ErrorCode::InvalidParams, "\"shape\" is \"rectangle\", \"lasso\" or \"brush\"", Json{{"path", "/shape"}});
    }
    e.shape = *parsed;
    e.logFrequency = c.boolean("logFrequency", true);
    e.gainDb = c.has("gainDb") ? c.number("gainDb") : (kind == SpectralKind::Tone ? -60.0 : -24.0);
    e.amount = c.has("amount") ? c.number("amount") : 100.0;
    e.start = c.has("start") ? c.number("start") : 0.0;
    e.end = c.has("end") ? c.number("end") : 0.0;
    e.low = c.has("low") ? c.number("low") : 0.0;
    e.high = c.has("high") ? c.number("high") : 0.0;
    e.radiusSeconds = c.has("radiusSeconds") ? c.number("radiusSeconds") : 0.05;
    e.radiusOctaves = c.has("radiusOctaves") ? c.number("radiusOctaves") : 0.25;
    e.harmonics = static_cast<int>(c.integer("harmonics", 1));
    e.widthHz = c.has("widthHz") ? c.number("widthHz") : 12.0;
    e.path = pointsOf(c);
    if (kind == SpectralKind::Tone && e.path.empty() && c.has("hz")) {
        e.path.push_back({e.start, c.number("hz")});
    }
    if (const auto problem = spectralEditProblem(e)) {
        fail(ErrorCode::InvalidParams, *problem);
    }
    return e;
}

/// The data entries that put `edits` in `instance`: the packed ones, and an empty entry for each old one that is no longer used
/// (an empty value removes the key).
std::vector<EffectData> dataFor(const EffectInstance* instance, const std::vector<SpectralEdit>& edits) {
    std::vector<EffectData> data = packSpectralEdits(edits);
    {
        EffectInstance probe;
        probe.data = data;
        if (unpackSpectralEdits(probe).size() != edits.size()) {
            fail(ErrorCode::EditRejected, "the clip's Spectral Repair is full: remove edits, or make them with fewer points");
        }
    }
    if (instance != nullptr) {
        for (const EffectData& old : instance->data) {
            if (isSpectralEditKey(old.key) &&
                std::ranges::none_of(data, [&](const EffectData& d) { return d.key == old.key; })) {
                data.push_back(EffectData{old.key, std::string()});
            }
        }
    }
    return data;
}

std::size_t insertIndex(const EffectStack& stack) {
    return !stack.empty() && stack.front().effect == kEssentialSoundEffect ? 1 : 0;
}

Json editJson(const SpectralEdit& e, std::size_t index) {
    const SpectralBounds b = spectralBounds(e);
    Json points = Json::array();
    for (const SpectralPoint& p : e.path) {
        points.push_back(Json{{"time", p.time}, {"hz", p.hz}});
    }
    return Json{{"index", index},
                {"kind", std::string(toString(e.kind))},
                {"shape", std::string(toString(e.shape))},
                {"gainDb", e.gainDb},
                {"amount", e.amount},
                {"start", b.start},
                {"end", b.end},
                {"low", b.low},
                {"high", b.high},
                {"harmonics", e.harmonics},
                {"widthHz", e.widthHz},
                {"points", std::move(points)},
                {"description", describeSpectralEdit(e)}};
}

CommandOutput addEdit(ExecutionContext& c, SpectralKind kind) {
    const ClipId id = audioClipOf(c);
    const SpectralEdit edit = editOf(c, kind);
    ProjectState state = c.state();
    const SequenceId seq = c.sequenceId();
    const auto at = sequenceIn(state, seq).findClip(id);
    const Clip& clip = at->clip();
    if (clip.isNested() || clip.hasUnknownSource()) {
        fail(ErrorCode::EditRejected, "Spectral Repair works on a clip of a media file, not on a nested sequence");
    }
    const EffectInstance* existing = findEffect(clip, kSpectralRepairEffect);
    std::vector<SpectralEdit> edits = existing != nullptr ? unpackSpectralEdits(*existing) : std::vector<SpectralEdit>{};
    if (edits.size() >= kMaxSpectralEdits) {
        fail(ErrorCode::EditRejected, "a clip holds at most " + std::to_string(kMaxSpectralEdits) + " spectral edits");
    }
    edits.push_back(edit);

    CommandOutput out;
    EffectInstanceId instance;
    wabcut::edit::EffectChange change;
    if (existing != nullptr) {
        instance = existing->id;
        change.data = dataFor(existing, edits);
        out.state = c.take(wabcut::edit::setEffect(state, seq, id, instance, change)).state;
    } else {
        auto added = c.take(wabcut::edit::addEffect(state, seq, EffectTarget::ofClip(id), std::string(kSpectralRepairEffect), {},
                                                    insertIndex(clip.effects())));
        instance = added.createdEffects.front();
        change.data = dataFor(nullptr, edits);
        out.state = c.take(wabcut::edit::setEffect(added.state, seq, id, instance, change)).state;
        out.createdIds.push_back(instance.value);
    }
    const SpectralBounds bounds = spectralBounds(edit);
    if ((kind == SpectralKind::Heal || kind == SpectralKind::NoiseFloor) && bounds.end - bounds.start > kHealReachSeconds) {
        out.warnings.push_back("this region is longer than " + std::to_string(kHealReachSeconds).substr(0, 4) +
                               " s: Heal looks about a second around it, so what is further inside is filled from the "
                               "sound above and below it, or lowered when nothing is in reach");
    }
    out.data = Json{{"clip", id.value},
                    {"effect", instance.value},
                    {"edits", edits.size()},
                    {"edit", editJson(edit, edits.size() - 1)}};
    return out;
}

CommandOutput removeEdits(ExecutionContext& c) {
    const ClipId id = audioClipOf(c);
    ProjectState state = c.state();
    const SequenceId seq = c.sequenceId();
    const Clip& clip = sequenceIn(state, seq).findClip(id)->clip();
    const EffectInstance* existing = findEffect(clip, kSpectralRepairEffect);
    if (existing == nullptr) {
        fail(ErrorCode::EditRejected, "the clip has no spectral repair");
    }
    std::vector<SpectralEdit> edits = unpackSpectralEdits(*existing);
    const bool all = c.boolean("all", false);
    if (!all && !c.has("index")) {
        fail(ErrorCode::InvalidParams, "\"index\" names the edit to remove, or \"all\": true removes them all", Json{{"path", "/index"}});
    }
    CommandOutput out;
    std::size_t removed = 0;
    if (all) {
        removed = edits.size();
        edits.clear();
    } else {
        const std::int64_t index = c.integer("index", -1);
        if (index < 0 || static_cast<std::size_t>(index) >= edits.size()) {
            fail(ErrorCode::InvalidParams, "no edit " + std::to_string(index) + " (the clip has " + std::to_string(edits.size()) + ")",
                 Json{{"path", "/index"}});
        }
        edits.erase(edits.begin() + index);
        removed = 1;
    }
    if (edits.empty()) {
        out.state = c.take(wabcut::edit::removeEffect(state, seq, id, existing->id)).state;
    } else {
        wabcut::edit::EffectChange change;
        change.data = dataFor(existing, edits);
        out.state = c.take(wabcut::edit::setEffect(state, seq, id, existing->id, change)).state;
    }
    out.data = Json{{"clip", id.value}, {"removed", removed}, {"edits", edits.size()}};
    return out;
}

CommandOutput setOptions(ExecutionContext& c) {
    const ClipId id = audioClipOf(c);
    ProjectState state = c.state();
    const SequenceId seq = c.sequenceId();
    const Clip& clip = sequenceIn(state, seq).findClip(id)->clip();
    const EffectInstance* existing = findEffect(clip, kSpectralRepairEffect);
    if (existing == nullptr) {
        fail(ErrorCode::EditRejected, "the clip has no spectral repair");
    }
    wabcut::edit::EffectChange change;
    if (c.has("fftSize")) {
        const std::int64_t size = c.integer("fftSize", 2048);
        if (size != 1024 && size != 2048 && size != 4096) {
            fail(ErrorCode::InvalidParams, "\"fftSize\" is 1024, 2048 or 4096", Json{{"path", "/fftSize"}});
        }
        change.params.push_back({"fftSize", ParamValue(size == 1024 ? 0.0 : size == 2048 ? 1.0 : 2.0)});
    }
    if (c.has("listen")) {
        const std::string mode = c.string("listen", "off");
        if (mode != "off" && mode != "selection" && mode != "removed") {
            fail(ErrorCode::InvalidParams, "\"listen\" is \"off\", \"selection\" or \"removed\"", Json{{"path", "/listen"}});
        }
        change.params.push_back({"listen", ParamValue(mode == "off" ? 0.0 : mode == "selection" ? 1.0 : 2.0)});
    }
    if (c.has("enabled")) {
        change.enabled = c.boolean("enabled", true);
    }
    if (!change.enabled && change.params.empty()) {
        fail(ErrorCode::InvalidParams, "name \"fftSize\", \"listen\" or \"enabled\"");
    }
    std::ranges::sort(change.params, {}, &EffectParam::key);
    CommandOutput out;
    out.state = c.take(wabcut::edit::setEffect(state, seq, id, existing->id, change)).state;
    out.data = Json{{"clip", id.value}, {"effect", existing->id.value}};
    return out;
}

CommandOutput listEdits(ExecutionContext& c) {
    const ClipId id = audioClipOf(c);
    const Clip& clip = c.sequence().findClip(id)->clip();
    const EffectInstance* existing = findEffect(clip, kSpectralRepairEffect);
    Json list = Json::array();
    Json options = Json::object();
    if (existing != nullptr) {
        const std::vector<SpectralEdit> edits = unpackSpectralEdits(*existing);
        for (std::size_t i = 0; i < edits.size(); ++i) {
            list.push_back(editJson(edits[i], i));
        }
        const auto choice = [&](const char* key, double fallback) {
            const ParamValue* v = existing->find(key);
            return static_cast<int>(std::lround(v != nullptr ? v->constant() : fallback));
        };
        const int size = choice("fftSize", 1.0);
        const int listen = choice("listen", 0.0);
        options = Json{{"fftSize", size == 0 ? 1024 : size == 1 ? 2048 : 4096},
                       {"listen", listen == 1 ? "selection" : listen == 2 ? "removed" : "off"},
                       {"enabled", existing->enabled},
                       {"effect", existing->id.value}};
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"clip", id.value}, {"edits", std::move(list)}, {"options", std::move(options)}};
    return out;
}

command::CommandSpec editSpec(const std::string& name, const std::string& title, const std::string& description, SpectralKind kind,
                              Json example) {
    const Json pointsParam =
        schema::array(schema::object({{"time", schema::number("Seconds of the clip's source.")},
                                      {"hz", schema::number("Frequency.", kMinSpectralHz, kMaxSpectralHz)}},
                                     {"time", "hz"}),
                      "The lasso's corners, the brush stroke's centre line, or the tone's trace (its frequency over time; one point is "
                      "a steady tone).");
    return {name,
            1,
            title,
            description,
            Capability::EditSequence,
            withSequence({{"clip", schema::integer("The audio clip (a video clip stands for its linked audio).", 1)},
                          {"shape", schema::enumeration({"rectangle", "lasso", "brush"}, "The region's shape. Default rectangle.")},
                          {"start", schema::number("Rectangle, or the tone's time range: where it starts, in seconds of the clip's source.")},
                          {"end", schema::number("Rectangle, or the tone's time range: where it ends, in seconds of the clip's source.")},
                          {"low", schema::number("Rectangle: its lowest frequency, Hz.", kMinSpectralHz, kMaxSpectralHz)},
                          {"high", schema::number("Rectangle: its highest frequency, Hz.", kMinSpectralHz, kMaxSpectralHz)},
                          {"points", pointsParam},
                          {"logFrequency", schema::boolean("A lasso's edges are straight on a logarithmic frequency axis (default) or a linear one.")},
                          {"gainDb", schema::number(kind == SpectralKind::Tone ? "How far the tone goes down, dB (default -60)."
                                                                               : "Attenuate: the change in level, dB (default -24).",
                                                    kMinSpectralGainDb, 0.0)},
                          {"amount", schema::number("Heal and noise floor: how much of the repair replaces the sound, percent (default 100).", 1.0, 100.0)},
                          {"radiusSeconds", schema::number("Brush: half its size in time, seconds (default 0.05).", 0.001, 60.0)},
                          {"radiusOctaves", schema::number("Brush: half its size in frequency, octaves (default 0.25).", 0.01, 10.0)},
                          {"hz", schema::number("Remove Tone: the frequency of a steady tone (instead of \"points\").", kMinSpectralHz, kMaxSpectralHz)},
                          {"harmonics", schema::integer("Remove Tone: the fundamental and this many harmonics are removed (1..16, default 1).", 1, 16)},
                          {"widthHz", schema::number("Remove Tone: the width of each notch, Hz (default 12).", 1.0, 500.0)}},
                         {"clip"}),
            std::move(example),
            {},
            [kind](ExecutionContext& c) { return addEdit(c, kind); }};
}

} // namespace

void registerSpectralRepairCommands(command::CommandRegistry& registry) {
    add(registry,
        editSpec("audio.spectralAttenuate",
                 "Attenuate Spectral Selection",
                 "Lowers a region of a clip's sound on the time-frequency plane (Audition's spectral frequency display with the "
                 "marquee, lasso or brush) by \"gainDb\" decibels, with a soft edge, and leaves the rest of the sound alone: a "
                 "cough, a chair squeak, a camera click or a single note over dialogue. The region is a rectangle (\"start\", "
                 "\"end\" in seconds of the clip's source, \"low\", \"high\" in Hz), a lasso (\"points\") or a brush stroke "
                 "(\"points\" with \"radiusSeconds\" and \"radiusOctaves\"). Not destructive: the edit is added to the clip's "
                 "Spectral Repair effect (made on the first edit), which plays the same in preview and export and stays on the "
                 "sound when the clip is trimmed, moved or retimed. One undo step.",
                 SpectralKind::Attenuate,
                 Json{{"clip", 12}, {"start", 4.2}, {"end", 4.7}, {"low", 700}, {"high", 3200}, {"gainDb", -30}}));
    add(registry,
        editSpec("audio.spectralHeal",
                 "Heal Spectral Selection",
                 "Replaces a region of a clip's sound with what surrounds it (Audition's healing brush): the level of the nearest "
                 "untouched sound before, after, above and below the region, with a natural phase so no tone or musical noise is "
                 "made. Regions are as in audio.spectralAttenuate; \"amount\" is how much of the repair replaces the sound. Heal "
                 "looks about a second around the region; a longer one is filled from the sound above and below it (a warning "
                 "says so). Not destructive; one undo step.",
                 SpectralKind::Heal,
                 Json{{"clip", 12}, {"start", 4.2}, {"end", 4.7}, {"low", 300}, {"high", 4000}}));
    add(registry,
        editSpec("audio.spectralNoiseFloor",
                 "Fill Spectral Selection with Noise Floor",
                 "Replaces a region of a clip's sound with the quiet background around it: the lower quartile of the untouched "
                 "sound near the region, with a natural phase. For a region whose surroundings carry speech or music that Heal "
                 "would copy into it. Regions are as in audio.spectralAttenuate. Not destructive; one undo step.",
                 SpectralKind::NoiseFloor,
                 Json{{"clip", 12}, {"start", 4.2}, {"end", 4.7}, {"low", 300}, {"high", 4000}}));
    add(registry,
        editSpec("audio.spectralRemoveTone",
                 "Remove Tone",
                 "Takes a steady tone and its harmonics out of a clip's sound, or a tone whose pitch moves: mains hum, a whistle, "
                 "an alarm, a single held note. \"hz\" is the frequency of a steady tone (or \"points\" trace a moving one over "
                 "the clip's source time), \"harmonics\" how many multiples to remove, \"widthHz\" how wide each notch is, "
                 "\"start\" and \"end\" the time range it is on for, \"gainDb\" how far the tone goes down. Cuts only the narrow "
                 "band of the tone; the sound around it is untouched. Not destructive; one undo step.",
                 SpectralKind::Tone,
                 Json{{"clip", 12}, {"start", 0}, {"end", 30}, {"hz", 50}, {"harmonics", 4}}));
    add(registry,
        {"audio.spectralRepairRemove", 1, "Remove Spectral Edit",
         "Takes one edit (\"index\", counted from 0 in audio.spectralRepairEdits) or all of them (\"all\": true) off a clip's "
         "Spectral Repair; the effect goes with the last edit. One undo step.",
         Capability::EditSequence,
         withSequence({{"clip", schema::integer("The audio clip.", 1)},
                       {"index", schema::integer("The edit to remove, counted from 0.", 0)},
                       {"all", schema::boolean("Remove every edit.")}},
                      {"clip"}),
         Json{{"clip", 12}, {"index", 0}}, {}, removeEdits});
    add(registry,
        {"audio.spectralRepairOptions", 1, "Spectral Repair Options",
         "Sets how a clip's Spectral Repair is analysed and heard: \"fftSize\" (1024, 2048 or 4096: more frequency or more time "
         "resolution), \"listen\" (\"off\" plays the repaired sound, \"selection\" only the sound inside the edits, \"removed\" "
         "only what the repair takes away: for judging an edit by ear), and \"enabled\" (false plays the clip without the repair, "
         "for an A/B comparison). One undo step.",
         Capability::EditSequence,
         withSequence({{"clip", schema::integer("The audio clip.", 1)},
                       {"fftSize", schema::integer("1024, 2048 or 4096.", 1024, 4096)},
                       {"listen", schema::enumeration({"off", "selection", "removed"}, "What is heard.")},
                       {"enabled", schema::boolean("false bypasses the repair.")}},
                      {"clip"}),
         Json{{"clip", 12}, {"listen", "removed"}}, {}, setOptions});
    add(registry,
        {"audio.spectralRepairEdits", 1, "Spectral Repair Edits",
         "Lists the edits of a clip's Spectral Repair in order (kind, shape, time range in seconds of the source, frequency range, "
         "level change, points) and its options (FFT size, listen mode, enabled). Changes nothing.",
         Capability::Read, withSequence({{"clip", schema::integer("The audio clip.", 1)}}, {"clip"}), Json{{"clip", 12}}, {}, listEdits});
}

} // namespace wabcut::audio::edit
