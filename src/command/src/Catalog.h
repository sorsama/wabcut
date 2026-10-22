#pragma once

// Private: pieces shared by the built-in catalog files.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/TimeInterpolation.h>

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::command::detail {

void registerEditCommands(CommandRegistry& registry);
void registerProjectCommands(CommandRegistry& registry);
void registerQueryCommands(CommandRegistry& registry);
void registerFinishingCommands(CommandRegistry& registry);
void registerRhythmCommands(CommandRegistry& registry);
/// Source Monitor: source.*, edit.insertFromSource / overwriteFromSource, targets, patching, match frame.
void registerSourceCommands(CommandRegistry& registry);
/// Markers 2.0: marker.update / clearAll, clip markers (source.addMarker / updateMarker / removeMarker) and
/// sequence.exportMarkers.
void registerMarkerCommands(CommandRegistry& registry);
/// Caption workshop (C.7): caption tracks and cues, styles and presets, speakers.
void registerCaptionCommands(CommandRegistry& registry);
/// captions.import (one command, CatalogInterchange.cpp; 0.2 graphics folded in): a .srt, .vtt or timed .txt file
/// read leniently into a new caption track (nullopt: none of them, or no cue in it), and the placing of a
/// reader's cues by the same rules: "offset", overlaps trimmed to the next cue, 4096-byte cues, at most
/// kMaxCaptionCues, "name" / "language" / "preset" (CatalogCaptions.cpp). `extra` joins the result's data.
std::optional<CommandOutput> importCaptionFileCues(ExecutionContext& c);
CommandOutput placeImportedCaptions(ExecutionContext& c, const std::filesystem::path& file,
                                    const std::vector<CaptionCue>& cues, std::string language,
                                    std::optional<CaptionStyle> style, Json extra);
/// Nested sequences and multicam (C.8): sequence.nest / unnest, clip.openNested, multicam.*.
void registerNestingCommands(CommandRegistry& registry);
/// Version history, review notes and packages, project merge (C.9): history.*, review.*, project.merge.
void registerCollaborationCommands(CommandRegistry& registry);
/// Clipboard (C.11): clipboard.copy / cut / paste / get, clip.pasteAttributes / removeAttributes.
void registerClipboardCommands(CommandRegistry& registry);
/// Library organisation (C.11, over the M5k ops): bin.*, media.setAttributes / makeSubclip / setSubclip.
void registerLibraryCommands(CommandRegistry& registry);
/// 0.2 (P.3): the Project panel - Search Bins, the Freeform view, XMP, importing from another project
/// (CatalogProjectPanel.cpp).
void registerProjectPanelCommands(CommandRegistry& registry);
/// Motion graphics (C.13): graphic.*, template.*, brand.* and lottie.import.
void registerGraphicsCommands(CommandRegistry& registry);
/// Interchange (X.1): interchange.export.
void registerInterchangeCommands(CommandRegistry& registry);
/// C.15: blend modes, adjustment layers, synthetic media, the preview render cache (CatalogParity.cpp).
void registerParityCommands(CommandRegistry& registry);
/// C.15 (gaming): layout.apply, layout.list, sequence.presets, sequence.create (CatalogLayout.cpp).
void registerLayoutCommands(CommandRegistry& registry);
/// C.16 (gaming): clip.instantReplay, clip.zoomPunch, clip.shake, sequence.removeDeadTime, sequence.makeVersion
/// (CatalogGaming.cpp).
void registerGamingCommands(CommandRegistry& registry);
/// P.2 (E5): Premiere's editing and sequence commands - frame scaling, duplicates, sequence settings, replace edits,
/// subsequences, through edits, gaps, frame hold options, merged clips, footage interpretation (CatalogEditing.cpp).
void registerEditingCommands(CommandRegistry& registry);
/// P.2: clip.setTimeInterpolation (CatalogMotion.cpp).
void registerMotionCommands(CommandRegistry& registry);
/// P.2: the Time Interpolation "method" param ("frameSampling", "frameBlending", "opticalFlow") and its value (fails
/// with InvalidParams on another name).
Json timeInterpolationParam(std::string description);
TimeInterpolation timeInterpolationValue(ExecutionContext& context, std::string_view key);
/// 0.2: export.* - Quick Export, the export queue, user presets, watch folders (CatalogExport.cpp, ExportServices.h).
void registerExportCommands(CommandRegistry& registry);
/// P.2 (E6): the Source and Program Monitors: clip.setMotion, sequence.exportFrame, source.exportFrame (CatalogMonitor.cpp).
void registerMonitorCommands(CommandRegistry& registry);
/// 0.2: effect presets, custom bins and favourites, effects.* (CatalogEffects.cpp).
void registerEffectsLibraryCommands(CommandRegistry& registry);
/// 0.2 timeline parity: clip.group, clip.ungroup, clip.setFade (CatalogTimeline.cpp).
void registerTimelineCommands(CommandRegistry& registry);

/// Adds a built-in command; a failure is a programming error caught by the catalog tests.
void addBuiltin(CommandRegistry& registry, CommandSpec spec);

// --- Parameter schemas shared by many commands ------------------------------------------------------------

Json sequenceParam();
Json timeParam(std::string description);
Json deltaParam(std::string description);
Json trackParam(std::string description);
Json tracksParam(std::string description);
Json clipParam(std::string description);
Json clipsParam(std::string description, std::int64_t minItems = 1);
Json mediaParam(std::string description);
Json linkedParam(bool defaultValue);
Json edgeParam();
/// A source time: ticks, or a timecode in the media's frame rate; null allowed.
Json sourceTimeParam(std::string description);
/// A source time param value: ticks or a timecode in `rate`; null is nullopt.
std::optional<Ticks> sourceTimeValue(const Json& value, const std::string& path, Rate rate);
/// The marker type, URL and frame target params (marker.add, marker.update, source.addMarker, ...).
Json markerTypeParam();
Json markerUrlParam();
Json markerTargetParam();
/// C.15: the marker "highlight" param (MarkerHighlight): an object, or null to remove it (updates).
Json markerHighlightParam();
/// The highlight of a marker as command output: score, confidence, reasons, peak (timecode), peakTicks, source.
Json highlightJson(ExecutionContext& context, const MarkerHighlight& highlight);
/// Sets a marker's type, url and target from the params that are given.
void applyMarkerLinkParams(ExecutionContext& context, Marker& marker);
/// Fails with InvalidParams when the marker is outside the model's bounds (markerProblem).
void requireValidMarker(const Marker& marker);
/// An object schema with `properties` plus the optional "sequence" parameter.
Json withSequence(std::initializer_list<std::pair<const char*, Json>> properties,
                  std::vector<std::string> required = {});

// --- Output helpers ----------------------------------------------------------------------------------------

/// A caption style as command output (colours as "#RRGGBB").
Json captionStyleJson(const CaptionStyle& style);
/// {"ticks": n, "timecode": "hh:mm:ss:ff"} in the context's sequence.
Json timeJson(ExecutionContext& context, Ticks t);
/// An edit's new state and created clips, plus `data`.
CommandOutput edited(edit::EditResult result, Json data = nullptr);

/// The video track a new title or graphic goes on, from its "track" and "placement" params (title.add): the named or
/// targeted track, the first free one at or above it, or a new top track (added to `state`).
TrackId videoPlacementTrack(ExecutionContext& context, ProjectState& state, Ticks at, Ticks duration);

/// A graphic template found by id (C.13; C.16 adds the built-in library) and the library it came from: "project", "user"
/// or "builtin".
struct FoundTemplate {
    GraphicTemplate tmpl;
    std::string library;
};
/// The template `id`: the project's library first, then the user's, then the built-ins (`library`, when not empty, limits
/// the search to that one). Fails with NotFound.
FoundTemplate findTemplate(ExecutionContext& context, const std::string& id, const std::string& library);
/// Template field values from a {"field id": value} param (text a string, colour, font, image a media id or name,
/// duration a time, checkbox a boolean); unknown fields fail with InvalidParams.
std::vector<std::pair<std::string, FieldValue>> fieldValues(ExecutionContext& context, const std::vector<TemplateField>& fields,
                                                            const Json& values, const std::string& path);

/// Runs `edit` on the context's state with exactly `tracks` targeted (all as they are when `tracks` is
/// empty), then restores the original targeting. Returns the edit's result on the restored state.
edit::EditResult withTargets(ExecutionContext& context, const std::vector<TrackId>& tracks,
                             const std::function<edit::EditOutcome(const ProjectState&)>& edit);

} // namespace wabcut::command::detail
