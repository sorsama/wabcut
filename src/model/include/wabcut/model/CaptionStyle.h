#pragma once

// How a caption track looks (C.7): saved with the project on each CaptionTrack, set by the caption commands, read by
// the compositor (playback CaptionSettings) for preview and burn-in alike. Sizes are relative so a style looks the
// same at any frame size: the font size is a fraction of the frame height, outline / shadow / box sizes are fractions
// of the font size (em), positions and widths fractions of the frame. Colours are display-referred sRGB, 0..1.
//
// The default style is the look captions had before styles were saved (I.4's boxed white text at the bottom), so
// projects from before schema 9 keep it. Named presets: five built in (builtinCaptionPresets) and the project's own
// (ProjectState::captionPresets).

#include <wabcut/model/Parameters.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

struct BrandKit; // Graphics.h

using CaptionRgb = std::array<double, 3>;

/// Brand kit references (C.13): names of the project brand kit's colours, and a font role, that replace the style's own
/// colours and font when the kit has them (resolvedCaptionStyle); empty = none. Setting another kit re-themes captions.
struct CaptionBrandRefs {
    std::string fill;
    std::string outline;
    std::string shadow;
    std::string box;
    std::string highlight;
    std::string font; ///< a font role ("body"): its family and weight

    bool operator==(const CaptionBrandRefs&) const = default;
    bool empty() const { return *this == CaptionBrandRefs{}; }
};

/// Where the text block sits: bottom or top of the safe area, or at `positionX/Y` (Custom).
enum class CaptionPlacement : std::uint8_t { Bottom, Top, Custom };
/// How lines align inside the block.
enum class CaptionAlign : std::uint8_t { Left, Center, Right };
/// Kinetic captions (the renderer evaluates them per frame): the whole caption pops or fades in, or each word follows the
/// speech. card 074 added the per-word looks: WordPop (words appear as spoken, each popping in), Bounce (all words
/// shown, the spoken one hops), HighlightBar (a bar slides to the spoken word, like CapCut and Opus), Typewriter (letters
/// appear one by one), Zoom (the spoken word grows). New values are only ever appended: projects name them as text.
enum class CaptionAnimation : std::uint8_t { None, Pop, Fade, WordByWord, WordPop, Bounce, HighlightBar, Typewriter, Zoom };
/// The curve of a word's transition: Smooth (ease out), Overshoot (ease out back: a little past, then settles), Linear.
enum class CaptionEase : std::uint8_t { Smooth, Overshoot, Linear };
/// Which words of a caption are emphasised (drawn in the emphasis colour and a little larger): none, the style's own
/// keywords, or those plus numbers, words in capitals and words followed by "!" (card 074).
enum class CaptionEmphasis : std::uint8_t { None, Keywords, Auto };
/// The case of the caption text as drawn and exported (card 114): as written, lower or UPPER case.
enum class CaptionTextCase : std::uint8_t { AsWritten, Lower, Upper };

std::string_view toString(CaptionPlacement value);
std::string_view toString(CaptionAlign value);
std::string_view toString(CaptionAnimation value);
std::string_view toString(CaptionEase value);
std::string_view toString(CaptionEmphasis value);
std::string_view toString(CaptionTextCase value);
std::optional<CaptionPlacement> captionPlacementFromString(std::string_view text);
std::optional<CaptionAlign> captionAlignFromString(std::string_view text);
std::optional<CaptionAnimation> captionAnimationFromString(std::string_view text);
std::optional<CaptionEase> captionEaseFromString(std::string_view text);
std::optional<CaptionEmphasis> captionEmphasisFromString(std::string_view text);
std::optional<CaptionTextCase> captionTextCaseFromString(std::string_view text);

struct CaptionStyle {
    std::string fontFamily;            ///< empty = the platform's sans-serif; missing glyphs fall back
    std::int32_t fontWeight = 600;     ///< 100..900 (400 regular, 700 bold)
    double fontSize = 48.0 / 1080.0;   ///< fraction of the frame height (0.005..0.5)
    CaptionRgb fill{1.0, 1.0, 1.0};
    double outlineWidth = 0.0;         ///< fraction of the font size (0..0.5); 0 = none
    CaptionRgb outline{0.0, 0.0, 0.0};
    double shadowOpacity = 0.0;        ///< 0..1; 0 = none
    CaptionRgb shadowColor{0.0, 0.0, 0.0};
    double shadowDistance = 0.08;      ///< fraction of the font size (0..2)
    double shadowAngle = 135.0;        ///< degrees, clockwise from the x axis (-360..360)
    double shadowSoftness = 0.1;       ///< blur radius, fraction of the font size (0..2)
    bool box = true;                   ///< a background box behind the text block
    CaptionRgb boxColor{16.0 / 255.0, 16.0 / 255.0, 19.0 / 255.0};
    double boxOpacity = 0.78;          ///< 0..1
    double boxPadding = 0.35;          ///< fraction of the font size (0..2)
    double boxRadius = 0.18;           ///< corner radius, fraction of the font size (0..2)
    CaptionPlacement placement = CaptionPlacement::Bottom;
    double safeArea = 0.1;             ///< inset of the safe area from each frame edge, fraction of the frame (0..0.4)
    /// Custom placement: centre of the text block, fractions of the frame width / height (0..1). The only keyframable
    /// fields: a curve here is keyed in sequence time (a caption track has no clip to stick to).
    ParamValue positionX{0.5};
    ParamValue positionY{0.85};
    CaptionAlign alignment = CaptionAlign::Center;
    double maxWidth = 0.8;             ///< widest text block, fraction of the frame width (0.1..1); longer lines wrap
    double lineSpacing = 1.0;          ///< 0.5..3
    CaptionRgb highlight{1.0, 0.82, 0.25}; ///< karaoke: colour of the words already spoken
    CaptionAnimation animation = CaptionAnimation::None;
    double animationSpeed = 0.15;      ///< seconds one word's transition takes (0.05..1); Fade takes a third longer
    CaptionEase animationEase = CaptionEase::Overshoot;
    CaptionEmphasis emphasis = CaptionEmphasis::None;
    std::vector<std::string> emphasisWords; ///< the keywords (compared without case; at most kMaxCaptionEmphasisWords)
    CaptionRgb emphasisColor{1.0, 0.86, 0.0};
    double emphasisScale = 1.12;       ///< how much larger an emphasised word is drawn (1..1.5)
    CaptionTextCase textCase = CaptionTextCase::AsWritten; ///< card 114: the case the text is drawn and exported in
    bool stripPunctuation = false;     ///< card 114: no commas, full stops, quotes... (apostrophes, hyphens inside words stay)
    bool stripAccents = false;         ///< card 114: é -> e, ł -> l, ð -> d... (not Thai vowel and tone marks)
    bool showSpeakers = false;         ///< a cue with a speaker starts with the speaker's name in the speaker's colour
    bool allowOverlap = false;         ///< cues of the track may overlap (the renderer stacks them)
    CaptionBrandRefs brand;            ///< C.13: brand kit colours and font that override the ones above
    /// 0.7 (card #138): OpenType features for the shaper (parseFontFeatures, Graphics.h): contextual alternates, ligatures,
    /// stylistic sets, ... for Persian, Arabic and branded fonts; empty = the font's defaults. Saved only when set.
    std::string features;

    bool operator==(const CaptionStyle&) const = default;
};

/// `style` with its brand references replaced by the kit's colours (their alpha ignored) and font; unchanged without a
/// kit or for names the kit does not have. The compositor draws captions with this.
CaptionStyle resolvedCaptionStyle(const CaptionStyle& style, const BrandKit* kit);

/// Why `style` is invalid (a value out of its range or not finite, a font family longer than kMaxCaptionFontBytes),
/// or nullopt.
std::optional<std::string> captionStyleProblem(const CaptionStyle& style);

/// A named style.
struct CaptionPreset {
    std::string name; ///< unique in the project (case-insensitive), never a built-in name
    CaptionStyle style;

    bool operator==(const CaptionPreset&) const = default;
};

/// The built-in presets: "Clean", "Bold Box", "Podcast", "Shorts", "Thai-friendly", and the animated gallery (card 074):
/// "Word Pop", "Highlighter", "Bounce", "Typewriter", "Zoom", "Hype".
std::span<const CaptionPreset> builtinCaptionPresets();
/// A built-in preset by name (case-insensitive), or null.
const CaptionPreset* findBuiltinCaptionPreset(std::string_view name);
/// Whether `name` is one of the five built-in presets that shipped before 0.7 (Clean, Bold Box, Podcast, Shorts,
/// Thai-friendly). A project cannot hold a preset of its own with such a name. The animated gallery added more built-in
/// names later, and a project made before that may already have a preset called "Bounce" or "Zoom": it still loads, and
/// the project's own preset wins over the built-in one of the same name (captionPresetStyle).
bool isOriginalBuiltinCaptionPreset(std::string_view name);

// Bounds for untrusted project files and requests.
inline constexpr std::size_t kMaxCaptionTracks = 64;          ///< per sequence
inline constexpr std::size_t kMaxCaptionCues = 100'000;       ///< per track
inline constexpr std::size_t kMaxCaptionTextBytes = 4096;     ///< per cue
inline constexpr std::size_t kMaxCaptionNameBytes = 256;      ///< track and preset names, languages
inline constexpr std::size_t kMaxCaptionFontBytes = 256;
inline constexpr std::size_t kMaxCaptionPresets = 256;        ///< per project
inline constexpr std::size_t kMaxCaptionEmphasisWords = 64;   ///< per style
inline constexpr std::size_t kMaxCaptionEmphasisWordBytes = 64;

} // namespace wabcut
