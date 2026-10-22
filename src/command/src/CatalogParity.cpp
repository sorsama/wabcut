// The Premiere parity pack (C.15): clip blend modes, adjustment layers, synthetic media and the preview render cache's
// commands. Keyers, track mattes and Warp Stabilizer are effects (clip.addEffect, clip.setParam);
// effect.analyzeStabilization and effect.analyzeRollingShutter live with the analysis (src/ai/stabilize, P.2).

#include "Catalog.h"

#include <wabcut/model/Blend.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Synthetic.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

std::vector<std::string> blendModeNames() {
    std::vector<std::string> out;
    for (const BlendMode m : blendModesInMenuOrder()) {
        out.emplace_back(toString(m));
    }
    return out;
}

std::vector<std::string> syntheticNames() {
    std::vector<std::string> out;
    for (const SyntheticDescriptor& d : syntheticKinds()) {
        out.emplace_back(d.shortName);
    }
    return out;
}

/// The "params" schema of media.newSynthetic: every kind's settings (P.2: Bars and Tone's and the Universal Counting
/// Leader's too), each described with the kinds it applies to.
Json syntheticParamsSchema() {
    Json props = Json::object();
    for (const SyntheticDescriptor& d : syntheticKinds()) {
        for (const EffectParamInfo& p : d.params) {
            const std::string key(p.key);
            if (props.contains(key)) {
                props[key]["description"] = props[key]["description"].get<std::string>() + " " + std::string(d.shortName) + ".";
                continue;
            }
            props[key] = schema::number(std::string(d.shortName) + ": " + std::string(p.displayName) + ".", p.minValue, p.maxValue);
        }
    }
    return Json{{"type", "object"},
                {"properties", props},
                {"additionalProperties", false},
                {"description", "Settings by key; missing ones take the defaults (a white matte, HD bars with a 1 kHz tone "
                                "at -20 dBFS, the leader's Premiere colours)."}};
}

/// "params": an object of numbers, as synthetic parameters (checked by the model).
std::vector<EffectParam> numberParams(ExecutionContext& c, std::string_view key) {
    std::vector<EffectParam> out;
    if (!c.has(key)) {
        return out;
    }
    const Json& value = c.params().at(std::string(key));
    for (const auto& [name, v] : value.items()) {
        if (!v.is_number()) {
            fail(ErrorCode::InvalidParams, std::string(key) + "." + name + " must be a number");
        }
        out.push_back(EffectParam{name, ParamValue(v.get<double>())});
    }
    return out;
}

/// The range a render command covers: "start" / "end" when given, else the sequence's In/Out marks, else all of it.
TimeRange renderRange(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    Ticks start = c.optionalTime("start").value_or(seq.markIn.value_or(Ticks{}));
    Ticks end = c.optionalTime("end").value_or(seq.markOut.value_or(seq.duration));
    start = std::max(start, Ticks{});
    end = std::min(end, seq.duration);
    if (end <= start) {
        fail(ErrorCode::EditRejected, "nothing to render: the range is empty (set In and Out, or give start and end)");
    }
    return TimeRange::fromStartEnd(start, end);
}

Json segmentJson(ExecutionContext& c, const RenderSegment& s) {
    Json out{{"start", c.timecode(s.range.start)},
             {"end", c.timecode(s.range.end())},
             {"startTicks", s.range.start.value},
             {"endTicks", s.range.end().value},
             {"status", std::string(toString(s.status))},
             {"cost", std::round(s.cost * 100.0) / 100.0}};
    if (!s.reason.empty()) {
        out["reason"] = s.reason;
    }
    return out;
}

PreviewServices* previewServicesOf(ExecutionContext& c) { return findService<PreviewServices>(c.services()); }

Json rangeParams(std::initializer_list<std::pair<const char*, Json>> extra = {}) {
    Json props = Json::object();
    props["start"] = timeParam("Where to start. Default: the sequence's In mark, else its start.");
    props["end"] = timeParam("Where to end. Default: the sequence's Out mark, else its end.");
    for (const auto& [k, v] : extra) {
        props[k] = v;
    }
    Json s = Json{{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
    s["properties"]["sequence"] = sequenceParam();
    return s;
}

} // namespace

void registerParityCommands(CommandRegistry& registry) {
    // --- Blend modes (Blend.h) ------------------------------------------------------------------------------------
    addBuiltin(
        registry,
        {"clip.setBlendMode", 1, "Set Blend Mode",
         "Sets the blend mode of video clips (the Opacity section's Blend Mode, Premiere's list): how each clip combines "
         "with what is below it. \"normal\" puts it over; the darken, lighten, contrast, inversion and component groups "
         "follow Premiere's menu. Opacity (keyframable) still applies on top. Audio clips are refused.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("The video clips.")},
                       {"mode", schema::enumeration(blendModeNames(), "The blend mode (Premiere's menu order).")}},
                      {"clips", "mode"}),
         Json{{"clips", Json::array({42})}, {"mode", "screen"}}, {},
         [](ExecutionContext& c) {
             const auto mode = blendModeFromString(c.string("mode"));
             if (!mode) {
                 fail(ErrorCode::InvalidParams, "unknown blend mode");
             }
             edit::ClipAttributeChange change;
             change.blendMode = *mode;
             const std::vector<ClipId> clips = c.clips("clips");
             return edited(c.take(edit::setClipAttributes(c.state(), c.sequenceId(), clips, change)),
                           Json{{"mode", std::string(toString(*mode))}, {"clips", clips.size()}});
         }});

    // --- Adjustment layers (Synthetic.h, Resolve.h's composite plan) ----------------------------------------------
    addBuiltin(
        registry,
        {"adjustment.add", 1, "New Adjustment Layer",
         "Adds an adjustment layer clip (File > New > Adjustment Layer, dropped on the timeline): its effects (colour "
         "grade, blur, masks, keyframes) apply to the composite of every video track below it for its duration; its "
         "Opacity and blend mode mix the adjusted picture over the unadjusted one. Inside a nested sequence it adjusts "
         "that sequence's lower tracks only. \"media\" reuses an existing adjustment layer item; without it a new one is "
         "made for the sequence. Add effects with clip.addEffect. createdIds: the clip (and the new item).",
         Capability::EditSequence,
         withSequence({{"at", timeParam("Where it starts.")},
                       {"duration", timeParam("How long it lasts. Default 5 seconds.")},
                       {"track", trackParam("A video track. Default: the first targeted unlocked video track.")},
                       {"placement",
                        schema::enumeration({"targeted", "freeTrackAtOrAbove", "newTrack"},
                                            "\"targeted\" (default): on \"track\", overwriting what is there. "
                                            "\"freeTrackAtOrAbove\": the first unlocked video track from \"track\" "
                                            "upwards that is free over its range. \"newTrack\": a new video track on top.")},
                       {"media", mediaParam("An existing adjustment layer item to reuse.")}},
                      {"at"}),
         Json{{"at", "00:00:10:00"}, {"duration", "00:00:05:00"}, {"placement", "newTrack"}}, {},
         [](ExecutionContext& c) {
             const Ticks at = c.time("at");
             const Ticks duration = c.optionalTime("duration").value_or(edit::kDefaultStillDuration);
             const MediaId media = c.has("media") ? c.media("media") : MediaId{};
             ProjectState state = c.state();
             const TrackId track = videoPlacementTrack(c, state, at, duration);
             edit::EditResult r =
                 c.take(edit::addAdjustmentLayer(state, c.sequenceId(), track, at, duration, media));
             const ClipId clip = r.placed.front();
             const MediaId used = r.createdMedia.isValid() ? r.createdMedia : media;
             const Sequence& after = *r.state.findSequence(c.sequenceId());
             const Clip& placed = after.findClip(clip)->clip();
             Json data{{"clip", clip.value},
                       {"media", used.value},
                       {"newItem", r.createdMedia.isValid()},
                       {"track", "V" + std::to_string(after.trackPosition(track)->index + 1)},
                       {"start", c.timecode(placed.start())},
                       {"end", c.timecode(placed.end())}};
             const MediaId created = r.createdMedia;
             CommandOutput out = edited(std::move(r), std::move(data));
             if (created.isValid()) {
                 out.createdIds.push_back(created.value);
             }
             return out;
         }});

    // --- Synthetic media (Synthetic.h) -----------------------------------------------------------------------------
    addBuiltin(
        registry,
        {"media.newSynthetic", 1, "New Synthetic Item",
         "Makes a synthetic project item (File > New): \"barsAndTone\" (SMPTE colour bars, params pattern 0 HD "
         "RP 219 or 1 SD EG 1, with a sine tone: tone.frequency, tone.level), \"blackVideo\", \"colorMatte\" (params "
         "color.r/g/b, 0..1: a background for text, a flash), \"countingLeader\" (Premiere's Universal Counting Leader: "
         "8 seconds counting down from 8 to 2 with a clock wipe, its colours and cue blips as params) or "
         "\"transparentVideo\" (a layer of its own for effects that draw). It is made for the sequence's frame size and "
         "rate, needs no file and can be any length (the leader is 8 seconds). Put it on the timeline with "
         "clip.overwrite or clip.insert. createdIds: the item.",
         Capability::EditProject,
         withSequence({{"kind", schema::enumeration(syntheticNames(), "What to make.")},
                       {"params", syntheticParamsSchema()},
                       {"name", schema::string("The item's name. Default: the kind's name (\"Color Matte\").")},
                       {"bin", schema::integer("The bin to put it in (bin id). Default: the root.", 1)}},
                      {"kind"}),
         Json{{"kind", "colorMatte"}, {"params", Json{{"color.r", 0.1}, {"color.g", 0.2}, {"color.b", 0.6}}}}, {},
         [](ExecutionContext& c) {
             const SyntheticDescriptor* d = findSynthetic(c.string("kind"));
             if (!d) {
                 fail(ErrorCode::InvalidParams, "unknown synthetic kind");
             }
             const BinId bin{static_cast<std::uint64_t>(c.integer("bin", 0))};
             auto added = edit::addSyntheticMedia(c.state(), d->kind, numberParams(c, "params"), c.sequenceId(), bin,
                                                  c.string("name"));
             if (!added) {
                 fail(added.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::InvalidParams,
                      added.error().message);
             }
             const MediaItem& item = *added->state.findMedia(added->id);
             CommandOutput out;
             out.state = std::move(added->state);
             out.createdIds.push_back(added->id.value);
             out.data = Json{{"media", added->id.value}, {"name", item.name}, {"kind", std::string(d->shortName)}};
             return out;
         }});

    // --- The preview render cache (PreviewCache.h) -------------------------------------------------------------------
    addBuiltin(
        registry,
        {"render.status", 1, "Render Bar",
         "The render bar of a sequence (Premiere's red/yellow/green bar): segments between edit points, each \"red\" "
         "(must be rendered to play in real time: nested sequences, adjustment layers or effects heavier than real "
         "time), \"yellow\" (effects, transitions, blend modes or track mattes that may play in real time), \"rendered\" "
         "(a valid preview file exists) or \"none\" (plays as it is). Rendered segments show only in hosts with a "
         "preview cache.",
         Capability::Read, rangeParams(), Json{{"start", "00:00:00:00"}, {"end", "00:01:00:00"}}, {},
         [](ExecutionContext& c) {
             const Sequence& seq = c.sequence();
             Ticks start = c.optionalTime("start").value_or(Ticks{});
             Ticks end = c.optionalTime("end").value_or(seq.duration);
             start = std::max(start, Ticks{});
             end = std::min(end, seq.duration);
             Json segments = Json::array();
             Json counts{{"none", 0}, {"yellow", 0}, {"red", 0}, {"rendered", 0}};
             if (end > start) {
                 const TimeRange range = TimeRange::fromStartEnd(start, end);
                 PreviewServices* services = previewServicesOf(c);
                 const std::vector<RenderSegment> bar = services && services->cache()
                                                            ? services->cache()->renderBar(c.state(), seq.id, range)
                                                            : renderSegments(c.state(), seq.id, range);
                 for (const RenderSegment& s : bar) {
                     segments.push_back(segmentJson(c, s));
                     counts[std::string(toString(s.status))] = counts[std::string(toString(s.status))].get<int>() + 1;
                 }
             }
             CommandOutput out;
             out.state = c.state();
             out.data = Json{{"segments", std::move(segments)}, {"counts", std::move(counts)},
                             {"cache", previewServicesOf(c) != nullptr && previewServicesOf(c)->cache() != nullptr}};
             return out;
         }});
    addBuiltin(
        registry,
        {"render.previewInOut", 1, "Render In to Out",
         "Renders preview files for the red and yellow segments between In and Out (or \"start\" / \"end\"; else the "
         "whole sequence), so they play in real time; segments with a valid preview are skipped. The cache keys each "
         "file by the segment's content and drops it when an edit changes that content. Needs a host with a preview "
         "renderer (the app); a dry run lists the segments it would render.",
         Capability::MediaAnalysis, rangeParams(), Json{{"start", "00:00:00:00"}, {"end", "00:00:30:00"}}, {},
         [](ExecutionContext& c) {
             const TimeRange range = renderRange(c);
             PreviewServices* services = previewServicesOf(c);
             PreviewCache* cache = services ? services->cache() : nullptr;
             const std::vector<RenderSegment> bar =
                 cache ? cache->renderBar(c.state(), c.sequenceId(), range) : renderSegments(c.state(), c.sequenceId(), range);
             std::vector<RenderSegment> todo;
             for (const RenderSegment& s : bar) {
                 if (s.status == RenderStatus::Red || s.status == RenderStatus::Yellow) {
                     todo.push_back(s);
                 }
             }
             Json planned = Json::array();
             for (const RenderSegment& s : todo) {
                 planned.push_back(segmentJson(c, s));
             }
             CommandOutput out;
             out.state = c.state();
             if (c.dryRun()) {
                 out.data = Json{{"segments", std::move(planned)}, {"rendered", 0}};
                 return out;
             }
             if (!cache || !services->renderer()) {
                 fail(ErrorCode::InvalidRequest,
                      "preview rendering is not available here: the host has no preview renderer (the app renders "
                      "previews)");
             }
             Json rendered = Json::array();
             std::int64_t bytes = 0;
             for (std::size_t i = 0; i < todo.size(); ++i) {
                 const RenderSegment& s = todo[i];
                 const std::string hash = renderContentHash(c.state(), c.sequenceId(), s.range);
                 const std::string file = PreviewCache::fileNameFor(c.sequenceId(), s.range, hash);
                 // The renderer is the host's code: tell it to stop by returning false rather than throwing through it.
                 const auto progress = [&](double fraction) {
                     const double overall = (static_cast<double>(i) + std::clamp(fraction, 0.0, 1.0)) /
                                            static_cast<double>(todo.size());
                     ExecutionControl* control = c.control();
                     return !control ||
                            !control->checkpoint("rendering previews (" +
                                                 std::to_string(static_cast<int>(overall * 100.0)) + "%)");
                 };
                 auto size = services->renderer()->render(c.state(), c.sequenceId(), s.range, cache->folder() / file, progress);
                 if (!size) {
                     if (size.error().code == wabcut::ErrorCode::Cancelled) {
                         c.checkpoint("rendering previews"); // the caller stopped: report it as such
                     }
                     (void)cache->save();
                     fail(ErrorCode::EditRejected, "rendering " + c.timecode(s.range.start) + " - " +
                                                       c.timecode(s.range.end()) + " failed: " + size.error().message);
                 }
                 cache->record(PreviewEntry{c.sequenceId(), s.range, hash, file, *size, 0});
                 bytes += *size;
                 rendered.push_back(segmentJson(c, s));
             }
             if (auto saved = cache->save(); !saved) {
                 out.warnings.push_back("the preview cache index could not be saved: " + saved.error().message);
             }
             out.data = Json{{"segments", std::move(rendered)}, {"rendered", todo.size()}, {"bytes", bytes},
                             {"cacheBytes", cache->totalBytes()}};
             return out;
         }});
    addBuiltin(
        registry,
        {"render.deletePreviewFiles", 1, "Delete Render Files",
         "Deletes preview files: of the sequence between \"start\" and \"end\" (In to Out with Sequence > Delete Render "
         "Files In to Out), of the whole sequence, or with \"all\" of every sequence in the project. The edit itself "
         "is not affected; the segments show red or yellow again. Needs a host with a preview cache; a dry run lists "
         "what would be deleted.",
         Capability::MediaAnalysis,
         rangeParams({{"all", schema::boolean("Every sequence's preview files.", false)}}), Json{{"all", true}}, {},
         [](ExecutionContext& c) {
             PreviewServices* services = previewServicesOf(c);
             PreviewCache* cache = services ? services->cache() : nullptr;
             if (!cache) {
                 fail(ErrorCode::InvalidRequest, "there is no preview cache here: the host keeps none (the app does)");
             }
             const bool all = c.boolean("all", false);
             const SequenceId sequence = all ? SequenceId{} : c.sequenceId();
             std::optional<TimeRange> range;
             if (!all && (c.has("start") || c.has("end"))) {
                 const Ticks start = c.optionalTime("start").value_or(Ticks{});
                 const Ticks end = c.optionalTime("end").value_or(c.sequence().duration);
                 if (end <= start) {
                     fail(ErrorCode::InvalidParams, "end must come after start");
                 }
                 range = TimeRange::fromStartEnd(start, end);
             }
             std::vector<PreviewEntry> hit;
             if (c.dryRun()) {
                 for (const PreviewEntry& e : cache->entries()) {
                     if ((!sequence.isValid() || e.sequence == sequence) && (!range || e.range.overlaps(*range))) {
                         hit.push_back(e);
                     }
                 }
             } else {
                 hit = cache->remove(sequence, range);
             }
             std::int64_t bytes = 0;
             for (const PreviewEntry& e : hit) {
                 bytes += e.bytes;
             }
             CommandOutput out;
             out.state = c.state();
             if (!c.dryRun()) {
                 if (auto saved = cache->save(); !saved) {
                     out.warnings.push_back("the preview cache index could not be saved: " + saved.error().message);
                 }
             }
             out.data = Json{{"files", hit.size()}, {"bytes", bytes}, {"deleted", !c.dryRun()}};
             return out;
         }});
}

} // namespace wabcut::command::detail
