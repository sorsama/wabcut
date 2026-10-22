#pragma once

// Editing motion graphics (C.13, Graphics.h): graphic clips and their layers, animation presets, templates and the
// brand kit. Each operation is one edit (one undo step) with a ChangeSet, like every edit op.
//
// Animation presets write editable keyframes into the layer (Premiere's and After Effects' presets do the same): fade,
// slide, scale pop, bounce and wipe animate the transform (or a mask); typewriter and word-by-word add a text-range
// animator whose selector is keyed. "In" presets key the start of the graphic, "Out" ones its end, "Loop" ones repeat
// over the whole clip. Keys a preset writes replace the keys already inside its time window; others stay.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/ProjectState.h>

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::edit {

/// The canvas item graphic clips play: one still video stream of width x height at `rate` (MediaItem::isGraphic).
MediaItem graphicCanvasItem(std::int32_t width, std::int32_t height, Rate rate);

/// Adds a graphic clip, overwriting [at, at + duration) on the video track: it plays the project's canvas item for the
/// sequence's frame size and rate (created when there is none) and holds `graphic` (layer ids 0 get fresh ones).
/// EditResult: placed (the clip), createdMedia (when the canvas was created).
/// Errors: NotFound, InvalidArgument (audio or locked track, at < 0, duration <= 0, graphicProblem).
EditOutcome addGraphic(const ProjectState& state, SequenceId sequence, TrackId track, Ticks at, Ticks duration,
                       Graphic graphic, BinId bin = {});

/// Replaces a graphic clip's graphic. Errors: NotFound, InvalidArgument (not a graphic clip, locked track, problem).
EditOutcome setGraphic(const ProjectState& state, SequenceId sequence, ClipId clip, Graphic graphic);

/// Adds a layer to a graphic clip, inside group `parent` (0 = the top level) at `index` (nullopt = on top). A layer id
/// of 0, or one the graphic already uses, is replaced by a fresh one (EditResult::createdLayer). Nested layer ids are
/// renumbered the same way.
EditOutcome addGraphicLayer(const ProjectState& state, SequenceId sequence, ClipId clip, GraphicLayer layer,
                            std::uint32_t parent = 0, std::optional<std::size_t> index = std::nullopt);
/// Replaces layer `layerId` (its id stays; `layer.id` is ignored).
EditOutcome updateGraphicLayer(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layerId,
                               GraphicLayer layer);
/// Removes a layer (a group with its children). Pins to it and template fields bound to it go too.
EditOutcome removeGraphicLayer(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layerId);

// ---- Animation presets ------------------------------------------------------------------------------------------------

enum class PresetKind : std::uint8_t { Fade, Slide, ScalePop, Typewriter, WordByWord, Wipe, Bounce };
enum class PresetPhase : std::uint8_t { In, Out, Loop };
enum class PresetEasing : std::uint8_t { Linear, EaseIn, EaseOut, EaseInOut, Overshoot };
enum class PresetDirection : std::uint8_t { Left, Right, Up, Down };

std::string_view toString(PresetKind kind);
std::optional<PresetKind> presetKindFromString(std::string_view text);
std::string_view toString(PresetPhase phase);
std::optional<PresetPhase> presetPhaseFromString(std::string_view text);
std::string_view toString(PresetEasing easing);
std::optional<PresetEasing> presetEasingFromString(std::string_view text);
std::string_view toString(PresetDirection direction);
std::optional<PresetDirection> presetDirectionFromString(std::string_view text);

struct PresetOptions {
    PresetKind kind = PresetKind::Fade;
    PresetPhase phase = PresetPhase::In;
    /// How long the animation takes (one cycle of a loop). In / Out: clamped to the graphic's length.
    Ticks duration = Ticks{kTicksPerSecond / 2};
    PresetEasing easing = PresetEasing::EaseOut;
    /// Slide and wipe: where the layer comes from (In), goes to (Out) or moves along (Loop). Bounce: Up or Down.
    PresetDirection direction = PresetDirection::Left;
    /// Slide and bounce: how far, pixels (0 = a quarter of the canvas).
    double distance = 0.0;
    /// Scale pop, bounce and overshoot: how far past the target, % (0..100).
    double overshoot = 10.0;
};

/// `layer` with the preset's keys written for a graphic whose time runs over [start, end) (graphic time); the canvas is
/// width x height. Errors: InvalidArgument (a text preset on another kind of layer, a loop of a preset that has none,
/// a duration <= 0, more than 256 loop cycles).
std::expected<GraphicLayer, Error> applyPresetToLayer(const GraphicLayer& layer, const PresetOptions& options, Ticks start,
                                                      Ticks end, std::int32_t width, std::int32_t height);
/// The same on a layer of a graphic clip (its graphic time range: the clip's curve time, or the responsive duration).
EditOutcome applyGraphicPreset(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layerId,
                               const PresetOptions& options);

// ---- Type on a path (0.2) ---------------------------------------------------------------------------------------------

/// The paths the Essential Graphics panel offers for type on a path (TextPath), each centred on the layer's origin:
/// Line (a straight line `width` wide), Arc (the top half of an ellipse `width` wide and `height` tall: text over a
/// rainbow), Circle (`width` across, clockwise from its left: text round the outside), Wave (two periods of a sine
/// `width` wide and `height` from crest to trough).
enum class TextPathShape : std::uint8_t { Line, Arc, Circle, Wave };

std::string_view toString(TextPathShape shape);
std::optional<TextPathShape> textPathShapeFromString(std::string_view text);

/// The vertices of a TextPathShape (cubic Bezier, tangents relative to their vertex); `closed` is set for a circle.
std::vector<MaskVertex> textPathVertices(TextPathShape shape, double width, double height, bool* closed = nullptr);

/// Puts text layer `layerId` of a graphic clip on `path` (nullopt: back on straight lines). Errors: NotFound,
/// InvalidArgument (not a text layer, locked track, graphicProblem of the result).
EditOutcome setTextPath(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layerId,
                        std::optional<TextPath> path);

// ---- Align and Distribute (0.2) ---------------------------------------------------------------------------------------

/// Premiere's Essential Graphics Align and Distribute buttons.
enum class LayerAlign : std::uint8_t {
    Left,
    HorizontalCenter,
    Right,
    Top,
    VerticalCenter,
    Bottom,
    DistributeHorizontal, ///< three or more layers: their centres evenly spaced between the outermost two
    DistributeVertical,
};
/// Premiere's "Align to Video Frame" / "Align to Selection" toggle (distributing is always within the selection).
enum class AlignTo : std::uint8_t { VideoFrame, Selection };

std::string_view toString(LayerAlign align);
std::optional<LayerAlign> layerAlignFromString(std::string_view text);

/// `graphic` with `layers` aligned (or distributed) as drawn at graphic time `time`: each layer's box on the canvas (a
/// group: its children's) moves to the video frame's (the design canvas) or the selection's edge or centre. A layer
/// moves by its Position; an animated Position moves with all its keys, so the animation keeps its shape. Errors:
/// InvalidArgument (no layers, an unknown or repeated id, fewer than three to distribute, a layer that shows nothing at
/// that time, a layer whose Position does not move it - pinned on that axis).
std::expected<Graphic, Error> alignGraphicLayers(const Graphic& graphic, std::span<const std::uint32_t> layers,
                                                 LayerAlign align, AlignTo to, Ticks time,
                                                 const ResolveOptions& options);
/// The same on a graphic clip, at the clip's graphic time `time` (GraphicResolve.h graphicTimeAt).
EditOutcome alignLayers(const ProjectState& state, SequenceId sequence, ClipId clip, std::span<const std::uint32_t> layers,
                        LayerAlign align, AlignTo to, Ticks time, const ResolveOptions& options);

// ---- Text styles (0.2) ------------------------------------------------------------------------------------------------

/// The text style a text layer shows: its first run's style, its alignment, background and shadow, named `name`.
ProjectTextStyle textStyleOf(const GraphicLayer& layer, std::string name);
/// `layer` (a text layer) in `style`: every run takes the run style, the layer the alignment, background and shadow, and
/// it follows the style (GraphicLayer::textStyle).
GraphicLayer withTextStyle(GraphicLayer layer, const ProjectTextStyle& style);
/// Adds a text style to the project, or replaces the one with its name (case-insensitive): every text layer that follows
/// it, in every graphic clip of the project (clips on locked tracks excepted), is restyled - Premiere's Push to Text
/// Style. Errors: InvalidArgument (textStyleProblem, more than kMaxProjectTextStyles).
EditOutcome saveTextStyle(const ProjectState& state, ProjectTextStyle style);
/// Premiere's Create Text Style: a text layer's look saved under `name`, the layer following it. Errors: NotFound,
/// InvalidArgument (not a text layer, a name in use).
EditOutcome createTextStyle(const ProjectState& state, SequenceId sequence, ClipId clip, std::uint32_t layer, std::string name);
/// Text layers of a graphic clip take a project text style and follow it. Errors: NotFound (the style, a layer),
/// InvalidArgument (not a text layer).
EditOutcome applyTextStyle(const ProjectState& state, SequenceId sequence, ClipId clip, std::span<const std::uint32_t> layers,
                           std::string_view name);
/// The ids of a graphic's text layers, those inside groups too, in drawing order (what a text style dropped on a whole
/// graphic clip styles).
std::vector<std::uint32_t> textLayerIds(const Graphic& graphic);
/// Renames a text style; the layers that follow it follow the new name. Errors: NotFound, InvalidArgument (taken, bad).
EditOutcome renameTextStyle(const ProjectState& state, std::string_view from, std::string to);
/// Removes a text style: the layers keep their look and follow nothing. Errors: NotFound.
EditOutcome removeTextStyle(const ProjectState& state, std::string_view name);
// ---- Templates --------------------------------------------------------------------------------------------------------

/// The value `field` shows now in `graphic` (a clip duration field: `clipDuration`).
FieldValue currentFieldValue(const Graphic& graphic, const TemplateField& field, Ticks clipDuration = {});

/// A template made of a graphic clip's graphic: `fields` bind layers (their default values are what the graphic shows),
/// the instance information is dropped. Errors: InvalidArgument (templateProblem).
std::expected<GraphicTemplate, Error> makeTemplate(const Graphic& graphic, std::string id, std::string name,
                                                   std::vector<TemplateField> fields, Ticks duration = {},
                                                   std::string description = {});

/// Adds `tmpl` to the project's library, or replaces the one with its id (its version then goes up by one).
EditOutcome saveProjectTemplate(const ProjectState& state, GraphicTemplate tmpl);
EditOutcome removeProjectTemplate(const ProjectState& state, std::string_view id);

/// `graphic` with field values set (by field id), through its instance's fields. Errors: InvalidArgument (unknown
/// field, a value of the wrong kind, a missing image media item, not a template instance).
std::expected<Graphic, Error> withFieldValues(const ProjectState& state, Graphic graphic,
                                              const std::vector<std::pair<std::string, FieldValue>>& values);

/// A graphic clip of `tmpl`: the design with `values` applied, remembering the template and its fields. Its length:
/// a "clip" duration field's value, else `duration`, else the template's duration, its responsive duration or 5 s.
EditOutcome instantiateTemplate(const ProjectState& state, SequenceId sequence, TrackId track, Ticks at,
                                std::optional<Ticks> duration, const GraphicTemplate& tmpl,
                                const std::vector<std::pair<std::string, FieldValue>>& values = {}, BinId bin = {});

/// Sets field values on a template instance clip: layers change, a "clip" duration field trims the clip's end. One edit.
EditOutcome setTemplateFields(const ProjectState& state, SequenceId sequence, ClipId clip,
                              const std::vector<std::pair<std::string, FieldValue>>& values);

// ---- Brand kit --------------------------------------------------------------------------------------------------------

/// Sets (or with nullopt removes) the project's brand kit: graphics, templates and caption styles that refer to it
/// re-theme. Errors: InvalidArgument (brandKitProblem, a logo whose media is missing).
EditOutcome setBrandKit(const ProjectState& state, std::optional<BrandKit> kit);

} // namespace wabcut::edit
