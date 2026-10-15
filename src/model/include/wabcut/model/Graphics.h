#pragma once

// Motion graphics (C.13, docs/graphics.md): a graphic clip holds layers - text with rich runs, shapes, images and
// groups - each with a keyframable transform, a blend mode, a drop shadow and masks; text layers also have text-range
// animators. Graphics can be saved as templates with editable fields (Premiere's MOGRTs) and take their fonts, colours
// and logos from the project's brand kit.
//
// Where it lives. A graphic clip plays a generator media item of kind "wabcut.graphic" (a canvas, like a title's) and
// carries its Graphic on its content block (ClipContent::graphic): copies of a clip are independent graphics, the way
// Premiere's Essential Graphics clips are. Keyframes use the clip parameters' curves (Parameters.h) in the clip's curve
// time, so they stick to the clip like any effect's - except in a graphic with responsive time (ResponsiveTime), whose
// keys are in design time: the protected intro plays from the clip's start, the protected outro ends at its end, and
// the middle stretches with the clip.
//
// Units. Positions, sizes and offsets are pixels of the graphic's design canvas (Graphic::width x height, y down);
// scales and opacities are percentages; rotations degrees clockwise; colours display-referred sRGB 0..1 with alpha.
// Layer masks (Masks.h, I.6) are in the layer's own space, normalised by the canvas size, so they move with the layer.
//
// Graphics come from project files, templates and Lottie files: untrusted input. graphicProblem() bounds and checks
// everything before anything else reads it.

#include <wabcut/model/Blend.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

/// GeneratorSpec::kind of the media items graphic clips play.
inline constexpr std::string_view kGraphicGenerator = "wabcut.graphic";

// ---- Bounds (graphics are untrusted input) ----------------------------------------------------------------------------
inline constexpr std::size_t kMaxGraphicLayers = 256;     ///< every layer of a graphic, nested ones included
inline constexpr std::size_t kMaxGraphicDepth = 8;        ///< groups inside groups
inline constexpr std::size_t kMaxGraphicTextBytes = 16 * 1024; ///< the text of one layer (all its runs)
inline constexpr std::size_t kMaxTextRuns = 256;
inline constexpr std::size_t kMaxTextAnimators = 16;
inline constexpr std::size_t kMaxPathVertices = 1024;
inline constexpr std::size_t kMaxPathKeys = 10'000;
inline constexpr std::size_t kMaxGradientStops = 16;
inline constexpr std::size_t kMaxDashes = 16;
inline constexpr std::size_t kMaxShapePoints = 64;        ///< polygon / star points
inline constexpr std::size_t kMaxGraphicNameBytes = 256;  ///< names, labels, ids, brand names
inline constexpr std::size_t kMaxFontBytes = 128;
inline constexpr std::size_t kMaxTemplateFields = 64;
inline constexpr std::size_t kMaxBrandColors = 64;
inline constexpr std::size_t kMaxBrandFonts = 16;
inline constexpr std::size_t kMaxBrandLogos = 32;
inline constexpr std::size_t kMaxProjectTemplates = 256;
inline constexpr std::int32_t kMaxGraphicCanvas = 16384;  ///< canvas side, pixels
inline constexpr double kMaxGraphicCoordinate = 1e6;      ///< |position|, size, offsets (pixels)

// ---- Paint ------------------------------------------------------------------------------------------------------------

/// A colour, optionally taken from the brand kit: when `brand` names a colour of the project's kit, that colour is used
/// (swapping the kit re-themes every graphic); otherwise, and when no kit has it, `rgba`.
struct GraphicColor {
    std::array<double, 4> rgba{1.0, 1.0, 1.0, 1.0};
    std::string brand;

    bool operator==(const GraphicColor&) const = default;
};

enum class PaintKind : std::uint8_t { None, Solid, Linear, Radial };

struct GradientStop {
    double offset = 0.0; ///< 0..1 along the gradient, non-decreasing
    GraphicColor color;

    bool operator==(const GradientStop&) const = default;
};

/// A fill: none, a colour, or a gradient between `start` and `end` (Linear: the axis; Radial: the centre and a point on
/// the circle), in the layer's own pixels.
struct Paint {
    PaintKind kind = PaintKind::Solid;
    GraphicColor color;
    std::vector<GradientStop> stops; ///< Linear / Radial: 2..kMaxGradientStops
    std::array<double, 2> start{0.0, 0.0};
    std::array<double, 2> end{100.0, 0.0};

    bool operator==(const Paint&) const = default;
};

enum class LineJoin : std::uint8_t { Miter, Round, Bevel };
enum class LineCap : std::uint8_t { Butt, Round, Square };

struct Stroke {
    bool enabled = false;
    GraphicColor color{{0.0, 0.0, 0.0, 1.0}, {}};
    ParamValue width{2.0};      ///< pixels, 0..1000
    std::vector<double> dashes; ///< dash, gap, dash, ... pixels (empty = solid)
    LineJoin join = LineJoin::Miter;
    LineCap cap = LineCap::Butt;

    bool operator==(const Stroke&) const = default;
};

// BlendMode (Blend.h): a layer's blend against what is below it in the graphic. C.15 extended the list to Premiere's.

/// A drop shadow behind the layer's content.
struct DropShadow {
    bool enabled = false;
    GraphicColor color{{0.0, 0.0, 0.0, 1.0}, {}};
    double opacity = 0.75;  ///< 0..1
    double angle = 135.0;   ///< degrees clockwise from the x axis
    double distance = 6.0;  ///< pixels
    double softness = 8.0;  ///< blur radius, pixels

    bool operator==(const DropShadow&) const = default;
};

// ---- Text -------------------------------------------------------------------------------------------------------------

enum class TextAlign : std::uint8_t { Left, Center, Right, Justify };

struct TextStyle {
    std::string fontFamily;   ///< empty: the brand kit's font for `brandFont`, else the platform's sans-serif
    std::string fontStyle;    ///< e.g. "Bold Italic"; empty = regular
    std::string brandFont;    ///< a brand kit font role ("heading", "body"): used when it is set and the kit has it
    std::int32_t fontWeight = 400; ///< 100..900
    double fontSize = 72.0;   ///< pixels, 1..2000
    GraphicColor fill;
    bool italic = false;
    bool underline = false;
    double tracking = 0.0;    ///< 1/1000 em, -1000..10000 (Premiere's tracking)
    double leading = 0.0;     ///< line height in pixels; 0 = automatic (1.2 x the size)
    double baselineShift = 0.0; ///< pixels

    bool operator==(const TextStyle&) const = default;
};

/// A piece of text in one style. The runs of a layer join into its text; '\n' starts a new line.
struct TextRun {
    std::string text;
    TextStyle style;

    bool operator==(const TextRun&) const = default;
};

/// A box behind the text block.
struct TextBackground {
    bool enabled = false;
    GraphicColor color{{0.0, 0.0, 0.0, 0.6}, {}};
    double padding = 12.0; ///< pixels
    double radius = 0.0;   ///< corner radius, pixels

    bool operator==(const TextBackground&) const = default;
};

/// What a text-range animator steps over. Characters are grapheme clusters (a Thai consonant with its vowel and tone
/// marks is one); words come from the line breaker's opportunities (spaces, or a dictionary for Thai and other
/// unspaced scripts when the host provides one); lines are split at '\n'.
enum class TextUnit : std::uint8_t { Character, Word, Line };
/// How the selection's edges fall off: Square selects whole units, Ramp fades over `softness` units.
enum class TextSelectorShape : std::uint8_t { Square, Ramp };

/// After Effects-style range selector with properties: the units between `start` and `end` (+ `offset`), percentages of
/// the unit count, are selected; each selected unit gets the properties' values (blended by how much it is selected),
/// on top of its normal look. Typewriter = opacity 0 on characters with `start` keyed from 0 to 100 %.
struct TextAnimator {
    std::string name;
    TextUnit unit = TextUnit::Character;
    ParamValue start{0.0};   ///< %, -100..200
    ParamValue end{100.0};   ///< %
    ParamValue offset{0.0};  ///< %, -200..200
    TextSelectorShape shape = TextSelectorShape::Square;
    double softness = 1.0;   ///< units, 0..1000 (Ramp)
    double opacity = 100.0;  ///< % of the unit's opacity when fully selected, 0..100
    double offsetX = 0.0;    ///< pixels when fully selected
    double offsetY = 0.0;
    double scale = 100.0;    ///< %
    double rotation = 0.0;   ///< degrees

    bool operator==(const TextAnimator&) const = default;
};

struct TextContent {
    std::vector<TextRun> runs;
    TextAlign align = TextAlign::Left;
    double boxWidth = 0.0;  ///< paragraph text: wrap at this width (pixels); 0 = point text (no wrapping)
    double boxHeight = 0.0; ///< paragraph text: the box's height; 0 = as tall as the text
    std::string language;   ///< ISO 639-1 for line breaking and word units ("th"); empty = from the text
    TextBackground background;
    std::vector<TextAnimator> animators;

    bool operator==(const TextContent&) const = default;
    /// Every run's text joined.
    std::string text() const;
};

// ---- Shapes and images ------------------------------------------------------------------------------------------------

enum class ShapeKind : std::uint8_t { Rectangle, Ellipse, Polygon, Star, Path };

/// A shape centred on the layer's origin (Path: vertices in the layer's pixels). Rounded rectangles are rectangles with
/// a corner radius. Trim paths draw only [start, end] % of the outline, rotated by `offset` (Lottie's trim).
struct ShapeContent {
    ShapeKind kind = ShapeKind::Rectangle;
    ParamValue width{200.0};
    ParamValue height{100.0};
    ParamValue cornerRadius{0.0};
    std::uint32_t points = 5;      ///< Polygon / Star: 3..kMaxShapePoints
    ParamValue outerRadius{100.0}; ///< Polygon / Star
    ParamValue innerRadius{50.0};  ///< Star
    /// Path: keys of the outline (Masks.h's vertex keys: same vertex count in every key), pixels.
    std::vector<MaskPathKey> path;
    bool closed = true;
    Paint fill;
    Stroke stroke;
    ParamValue trimStart{0.0};  ///< %
    ParamValue trimEnd{100.0};  ///< %
    ParamValue trimOffset{0.0}; ///< degrees

    bool operator==(const ShapeContent&) const = default;
};

enum class ImageFit : std::uint8_t { Contain, Cover, Fill, None };

/// A picture from the project's media (a still or a video's first frame... the renderer's call), drawn into a box
/// centred on the layer's origin.
struct ImageContent {
    MediaId media;
    std::string brandLogo; ///< a brand kit logo's name: used instead of `media` when the kit has it
    double width = 400.0;  ///< the box, pixels
    double height = 300.0;
    ImageFit fit = ImageFit::Contain;

    bool operator==(const ImageContent&) const = default;
};

// ---- Layers -----------------------------------------------------------------------------------------------------------

enum class LayerKind : std::uint8_t { Text, Shape, Image, Group };

/// Position, anchor, scale, rotation and opacity; all keyframable. The layer's content is drawn around its anchor point,
/// which sits at `position` in the parent (the canvas for a top-level layer).
struct LayerTransform {
    ParamValue x{0.0};
    ParamValue y{0.0};
    ParamValue anchorX{0.0};
    ParamValue anchorY{0.0};
    ParamValue scaleX{100.0};   ///< %, -100000..100000
    ParamValue scaleY{100.0};
    ParamValue rotation{0.0};   ///< degrees
    ParamValue opacity{100.0};  ///< %, 0..100

    bool operator==(const LayerTransform&) const = default;
};

/// Premiere's Responsive Design - Position: a layer pinned to edges of a sibling layer follows them (a box pinned to
/// all four edges of a name grows with the name). `offsets` are this layer's edge minus the target's, per edge
/// (left, top, right, bottom), as when it was pinned.
struct LayerPin {
    static constexpr std::uint8_t kLeft = 1;
    static constexpr std::uint8_t kTop = 2;
    static constexpr std::uint8_t kRight = 4;
    static constexpr std::uint8_t kBottom = 8;

    std::uint32_t target = 0;
    std::uint8_t edges = 0;
    std::array<double, 4> offsets{0.0, 0.0, 0.0, 0.0};

    bool operator==(const LayerPin&) const = default;
};

struct GraphicLayer {
    std::uint32_t id = 0; ///< unique in its graphic, not 0 (template fields and pins refer to it)
    std::string name;
    LayerKind kind = LayerKind::Shape;
    bool visible = true;
    LayerTransform transform;
    BlendMode blend = BlendMode::Normal;
    DropShadow shadow;
    std::vector<Mask> masks; ///< Masks.h; this layer's space normalised by the canvas
    /// The layer shows from `in` until `out` (graphic time); `out` unset = to the end.
    Ticks in{};
    std::optional<Ticks> out;
    std::optional<LayerPin> pin;
    TextContent text;   ///< LayerKind::Text
    ShapeContent shape; ///< LayerKind::Shape
    ImageContent image; ///< LayerKind::Image
    std::vector<GraphicLayer> children; ///< LayerKind::Group, bottom to top

    bool operator==(const GraphicLayer&) const = default;
};

/// Premiere's Responsive Design - Time: the graphic was designed `duration` long; its first `intro` and last `outro`
/// play unchanged at the clip's start and end whatever the clip's length, the middle stretches (or shrinks). A clip
/// shorter than intro + outro plays both, squeezed in proportion.
struct ResponsiveTime {
    Ticks duration{};
    Ticks intro{};
    Ticks outro{};

    bool operator==(const ResponsiveTime&) const = default;
};

// ---- Templates (MOGRT-like) -------------------------------------------------------------------------------------------

/// What a template field edits.
///   Text      a text layer's text (one run in the style of its first run)      value: text
///   Color     a layer's colour: "fill" (text runs or shape), "stroke", "background" (text box), "shadow"  value: colour
///   Font      a text layer's font (every run)                                  value: font family (+ style, weight)
///   Image     an image layer's media                                           value: media id
///   Duration  "clip": the clip's length; "out": the layer's out point          value: ticks
///   Checkbox  a layer's visibility                                             value: true / false
enum class FieldKind : std::uint8_t { Text, Color, Font, Image, Duration, Checkbox };

struct FieldValue {
    std::string text;      ///< Text; Font: the family
    std::string fontStyle; ///< Font
    std::int32_t fontWeight = 0; ///< Font; 0 = unchanged
    GraphicColor color;    ///< Color
    MediaId media;         ///< Image
    Ticks duration{};      ///< Duration
    bool flag = false;     ///< Checkbox

    bool operator==(const FieldValue&) const = default;
};

struct TemplateField {
    std::string id;    ///< stable key: [A-Za-z][A-Za-z0-9_.-]*
    std::string label; ///< shown in the Essential Graphics panel
    FieldKind kind = FieldKind::Text;
    std::uint32_t layer = 0;
    std::string property; ///< see FieldKind
    FieldValue value;     ///< the default in a template; the current value in an instance

    bool operator==(const TemplateField&) const = default;
};

/// What a graphic clip remembers of the template it came from: its id and version, and its fields with their values.
struct TemplateInstance {
    std::string templateId;
    std::int32_t version = 1;
    std::vector<TemplateField> fields;

    bool operator==(const TemplateInstance&) const = default;
};

struct Graphic {
    std::string name;
    std::int32_t width = 1920; ///< design canvas, pixels
    std::int32_t height = 1080;
    std::vector<GraphicLayer> layers; ///< bottom to top
    std::optional<ResponsiveTime> responsive;
    std::optional<TemplateInstance> instance;

    bool operator==(const Graphic&) const = default;

    const GraphicLayer* findLayer(std::uint32_t id) const;
    GraphicLayer* findLayer(std::uint32_t id);
    /// Every layer, nested ones included, depth first (bottom to top).
    std::size_t layerCount() const;
    /// One more than the largest layer id.
    std::uint32_t nextLayerId() const;
};

/// A saved graphic with editable fields: the project's library (ProjectState::templates) and the user's library
/// folder (GraphicJson.h) hold them.
struct GraphicTemplate {
    std::string id;      ///< stable: [A-Za-z0-9][A-Za-z0-9_.-]*, at most 128 bytes
    std::string name;
    std::string description;
    std::int32_t version = 1; ///< bumped when it is saved again under the same id
    Graphic graphic;          ///< the design; field values in `fields` are the defaults
    std::vector<TemplateField> fields;
    Ticks duration{};         ///< default clip length; 0 = the graphic's responsive duration, else 5 s

    bool operator==(const GraphicTemplate&) const = default;
};

// ---- Brand kit --------------------------------------------------------------------------------------------------------

struct BrandColor {
    std::string name; ///< unique in the kit (case-sensitive), e.g. "Primary", "Accent"
    std::array<double, 4> rgba{1.0, 1.0, 1.0, 1.0};

    bool operator==(const BrandColor&) const = default;
};

struct BrandFont {
    std::string role; ///< unique in the kit: "heading", "body", ...
    std::string family;
    std::string style;
    std::int32_t weight = 400;

    bool operator==(const BrandFont&) const = default;
};

struct BrandLogo {
    std::string name;
    MediaId media;

    bool operator==(const BrandLogo&) const = default;
};

/// The project's fonts, colours and logos: templates, graphics and caption styles refer to them by name, so setting a
/// new kit re-themes everything that uses it.
struct BrandKit {
    std::string name;
    std::vector<BrandColor> colors;
    std::vector<BrandFont> fonts;
    std::vector<BrandLogo> logos;

    bool operator==(const BrandKit&) const = default;

    const BrandColor* color(std::string_view name) const;
    const BrandFont* font(std::string_view role) const;
    const BrandLogo* logo(std::string_view name) const;
};

// ---- Names and checks -------------------------------------------------------------------------------------------------

std::string_view toString(LayerKind kind);
std::optional<LayerKind> layerKindFromString(std::string_view text);
std::string_view toString(ShapeKind kind);
std::optional<ShapeKind> shapeKindFromString(std::string_view text);
std::string_view toString(PaintKind kind);
std::optional<PaintKind> paintKindFromString(std::string_view text);
std::string_view toString(TextAlign align);
std::optional<TextAlign> textAlignFromString(std::string_view text);
std::string_view toString(TextUnit unit);
std::optional<TextUnit> textUnitFromString(std::string_view text);
std::string_view toString(TextSelectorShape shape);
std::optional<TextSelectorShape> textSelectorShapeFromString(std::string_view text);
std::string_view toString(ImageFit fit);
std::optional<ImageFit> imageFitFromString(std::string_view text);
std::string_view toString(LineJoin join);
std::optional<LineJoin> lineJoinFromString(std::string_view text);
std::string_view toString(LineCap cap);
std::optional<LineCap> lineCapFromString(std::string_view text);
std::string_view toString(FieldKind kind);
std::optional<FieldKind> fieldKindFromString(std::string_view text);

/// Why `graphic` is invalid (a bound, a value out of range or not finite, a malformed curve or path, duplicate or zero
/// layer ids, content of another kind, a pin to a missing or non-sibling layer or in a cycle, a template field that
/// does not fit its layer), or nullopt.
std::optional<std::string> graphicProblem(const Graphic& graphic);
std::optional<std::string> brandKitProblem(const BrandKit& kit);
std::optional<std::string> templateProblem(const GraphicTemplate& tmpl);
/// Template field ids: [A-Za-z][A-Za-z0-9_.-]*, at most 64 bytes. Template ids: [A-Za-z0-9][A-Za-z0-9_.-]*, <= 128.
bool isValidFieldId(std::string_view id);
bool isValidTemplateId(std::string_view id);

/// The colour a GraphicColor stands for under `kit` (null = no kit).
std::array<double, 4> resolveColor(const GraphicColor& color, const BrandKit* kit);

// ---- Text units -------------------------------------------------------------------------------------------------------

/// Byte ranges [first, second) of the grapheme clusters of UTF-8 `text`: a code point with the combining marks, Thai and
/// Lao vowel and tone marks, variation selectors, emoji modifiers and zero-width joiner sequences that follow it ("\r\n"
/// is one). Newlines are clusters of their own.
std::vector<std::pair<std::size_t, std::size_t>> graphemeClusters(std::string_view text);

} // namespace wabcut
