#pragma once

// Sequence evaluation for video: which clips are visible at a sequence time, with what source time and
// motion. Pure functions over an immutable snapshot (no decoding, no I/O), shared by the scheduler and
// export.

#include <wabcut/color/Grade.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>
#include <wabcut/model/TimeInterpolation.h>
#include <wabcut/playback/video/CaptionRedaction.h>
#include <wabcut/playback/video/MotionAnalysis.h>
#include <wabcut/render/GraphicRaster.h>
#include <wabcut/render/Caption.h>
#include <wabcut/render/PreparedComposite.h>
#include <wabcut/render/Title.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace wabcut::playback {

/// One visible video layer at a sequence time.
struct LayerSpec {
    ClipId clip;
    MediaId media;
    const MediaItem* item = nullptr; ///< Valid while the snapshot is held.
    /// Index into MediaInfo::video of the clip's stream.
    int streamIndex = 0;
    /// Container stream index for the decoder (-1 = best video stream).
    int containerStream = -1;
    /// Source time the clip shows (Clip::sourceTimeAt: speed, reverse and freeze aware; past the clip's own
    /// range for the handles of a transition).
    Ticks sourceTime{};
    render::LayerTransform transform;
    double opacity = 1.0;
    /// The clip's blend mode (C.15): how the layer combines with the layers below it.
    BlendMode blend = BlendMode::Normal;
    /// The clip's stack effects evaluated at the frame's curve time (disabled and bypassed ones left out).
    std::vector<render::LayerEffect> effects;
    /// Source-anchored redactions of the clip's media covering its source time (I.6), unless excepted for this clip.
    std::vector<render::LayerRedaction> redactions;
    /// The project's redactions covering the clip's media (model/RedactionMedia.h: its subclip master's and stem
    /// source's too), re-evaluated against the decoded frame's actual PTS/duration. Point into the snapshot: valid
    /// while it is held, like item.
    std::vector<const Redaction*> sourceRedactions;
    double redactionAspect = 16.0 / 9.0;
    /// Stack effects bypassed because the renderer cannot run them (unknown id, newer version, wrong kind).
    int bypassedEffects = 0;
    /// The clip's input colour space (I.9, Clip::inputColorSpace, else the camera log its media's metadata names when
    /// the project auto-detects log; unset = Auto from the frame's tags). Managed projects read the frame through it,
    /// Direct Rec.709 converts (tone-maps) such clips into Rec.709, Disable Color Management ignores it.
    std::optional<InputColorSpace> inputSpace;
    /// 0.2 field options (model/FieldOptions.h): how the clip shows its frames' lines - deinterlaced (one field kept,
    /// both fields at the field rate) or blended (Flicker Removal). Set on the layers that draw a frame or a nested
    /// canvas; the groups Time Interpolation and the stabilizers make hand it to the frames inside them.
    FieldRendering fields;
    /// Set on the first layer of a transition group (see render::LayerTransition).
    std::optional<render::LayerTransition> transition;
    /// A title clip (C.4 generator media): what render::TitleRenderer draws, at the frame's curve time. Nothing
    /// is decoded for it.
    std::optional<render::TitleStyle> title;
    /// A burned-in caption (the topmost layer, no media: `item` is null): what render::CaptionRenderer draws.
    std::optional<render::CaptionFrame> caption;
    /// A graphic clip (C.13, model/GraphicResolve.h): its draw list at the frame, rasterised by
    /// render::GraphicRasterCache into a `graphicSize` frame (the canvas item's: the design canvas fitted in), with the
    /// pictures of its image items from `graphicImages`, cached under `graphicKey`. Nothing is decoded for it.
    std::shared_ptr<const DrawList> graphic;
    render::CanvasSize graphicSize{};
    std::shared_ptr<const render::GraphicImageSource> graphicImages;
    std::uint64_t graphicKey = 0;
    /// A synthetic colour matte, black or transparent video (C.15, model/Synthetic.h): one fill of `fillSize`,
    /// premultiplied sRGB-encoded RGBA8. Nothing is decoded for it.
    std::optional<std::array<std::uint8_t, 4>> fill;
    render::CanvasSize fillSize{};
    /// 0 = V1 (of the sequence holding the layer).
    int track = 0;

    /// A nested or multicam clip (C.8; `item` is null, nothing is decoded for it): its picture is the composite of the
    /// `nestedCount` specs right after it - its whole subtree, nested groups included, bottom to top - on a canvas of
    /// `nestedCanvas`, placed with this spec's transform, opacity, effects and transition. A nested sequence's master
    /// effects are a group of their own inside it (identity transform). nextSibling() skips a subtree.
    bool nested = false;
    int nestedCount = 0;
    render::CanvasSize nestedCanvas{};
    /// A nested group that is the Track Matte of the next layer of its level (0.1.2): the matte track's clip at this
    /// time. Not drawn; buildCompositeLayers() hands its composite to that layer's TrackMatte effect.
    bool matte = false;
    /// 0.2: with `matte`, the next layer's library effect (its index in that layer's `effects`) that reads this group as
    /// its second picture (Echo's echoes, Gradient Wipe's or Difference Matte's layer of another track); -1 = the layer's
    /// Track Matte Keys.
    int matteEffect = -1;
    /// The nested and multicam clips leading to this layer, outermost first (null at the top level): a redaction that
    /// excepts any of them does not apply (docs/internal/c8-nested-multicam-handoff.md). Shared by the layers of one level.
    std::shared_ptr<const std::vector<ClipId>> nestPath;
    /// The decoder this layer reads from (sessionKey()): its clip id at the top level; inside nested clips a key of the
    /// path too, since one nested sequence can show twice at different times.
    std::uint64_t decodeKey = 0;

    /// P.2 Time Interpolation (docs/motion.md): set on the group (`nested`, two children, a canvas of the stream's
    /// displayed size) that stands for a retimed clip whose Frame Blending or Optical Flow decides its picture. Its
    /// `sourceTime` is the exact source time; the first child decodes the frame containing it, the second the frame
    /// after that one (interpolationNextTime). buildCompositeLayers mixes the two by where `sourceTime` lies between their
    /// PTS - a cross dissolve, or a flow mix along the motion between them - and places the group with the clip's
    /// Motion, Opacity, effects, redactions and transition.
    std::optional<TimeInterpolation> interpolation;
    /// P.2: the second frame of an interpolation group. Its sourceTime is a first guess (one nominal frame on): the
    /// scheduler and export decode the frame that follows the first child's.
    bool interpolationSecond = false;
    /// P.2 stabilizers (Warp Stabilizer, Rolling Shutter Repair): the warp of this layer's frame inside its clip's
    /// stabilizer group (render::CompositeLayer::warp, fractions of the frame); identity elsewhere. The group (a canvas of
    /// the stream's displayed size) carries the clip's Motion, effects (Stabilize, Crop's crop first) and transition.
    std::array<double, 9> warp{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    /// P.2: a Bars and Tone or Universal Counting Leader picture (model/Synthetic.h) of `item` at media time
    /// `sourceTime`, drawn at `fillSize` (render::syntheticPicture). Nothing is decoded for it.
    bool syntheticPicture = false;
};

/// P.2: the source time of the frame after `first` (its PTS plus its duration: the next frame's PTS).
inline Ticks interpolationNextTime(const media::VideoFrame& first) { return first.pts + first.duration; }

/// P.2: where `exact` lies between frames `a` and `b` (0 at a's PTS, 1 at b's), snapped to 0 or 1 within 1/1024; 0 when
/// `b` is missing or not after `a`.
double interpolationWeight(Ticks exact, const media::VideoFrame& a, const media::VideoFrame& b);

/// P.2: the picture of `spec` (a decodable media layer) a motion analysis reads at source time `time`, placed with the
/// spec's transform (`placed`) or filling a canvas of the stream's displayed size (identity). Null when the layer has
/// nothing to decode (no file, a generated layer).
std::optional<MotionPicture> motionPictureOf(const LayerSpec& spec, Ticks time, bool placed);

/// P.2: the motion analyses the composite of `specs` needs with these decoded `frames` (frames[j] for specs[j], as
/// buildCompositeLayers takes them), `canvas` the top level's: Optical Flow interpolation groups whose frames do not
/// coincide with the source time, and Morph Cut pairs. The scheduler waits for these (MotionAnalyzer) before it
/// publishes; tests list them.
std::vector<MotionRequest> motionRequestsOf(const std::vector<LayerSpec>& specs,
                                            const std::vector<media::VideoFrame>& frames, render::CanvasSize canvas);

/// The index of the spec after `j` and its nested subtree: the next layer of the same level.
inline std::size_t nextSibling(const std::vector<LayerSpec>& specs, std::size_t j) {
    return j + 1 + static_cast<std::size_t>(std::max(0, specs[j].nestedCount));
}

/// The key a decoder (preview session, export decoder) of `spec` is kept under.
inline std::uint64_t sessionKey(const LayerSpec& spec) {
    return spec.decodeKey != 0 ? spec.decodeKey : spec.clip.value;
}

/// When each word of a cue is spoken, in sequence time: the karaoke highlight of a cue whose words are known (a
/// transcript linked to the captions). Words are those of render::captionWordSpans(cue.text), in order.
struct CaptionWordTiming {
    Ticks start{};
    Ticks end{};
    bool operator==(const CaptionWordTiming&) const = default;
};
struct CaptionWordTimings {
    /// (caption track id, cue start) -> one timing per word of the cue.
    std::map<std::pair<std::uint64_t, std::int64_t>, std::vector<CaptionWordTiming>> cues;
};

/// Burned-in captions (Scheduler::setCaptions, export "Burn in captions"): which caption track, how it looks.
struct CaptionSettings {
    /// How they look: unset = the drawn track's own style (C.7, CaptionTrack::style, through captionStyleOf), which
    /// is what preview and export share; set = an explicit override (e.g. a panel's live preview of a style edit).
    std::optional<render::CaptionStyle> style;
    /// The caption track to draw; unset (or not found, or disabled) = the sequence's first enabled caption track.
    std::optional<CaptionTrackId> track;
    /// Karaoke: highlight the words already spoken: from `words` when the cue has timings for every word, else
    /// estimated (the cue's time shared among its words by length).
    bool karaoke = false;
    std::shared_ptr<const CaptionWordTimings> words;
    /// What a word the soundtrack bleeps or mutes is drawn as (CaptionRedaction.h; Codex wave-4 P1-3).
    std::string redactedWord{kRedactedWordToken};

    bool operator==(const CaptionSettings&) const = default;
};

/// The caption track `settings` draw in `sequence` (null: none enabled).
const CaptionTrack* captionTrackFor(const Sequence& sequence, const CaptionSettings& settings);
/// The renderer's form of a caption track's style (C.7, docs/internal/c7-caption-handoff.md): sizes in pixels of a 1080-line
/// frame, the custom position's curves evaluated at sequence time `time`.
render::CaptionStyle captionStyleOf(const CaptionStyle& style, Ticks time);
/// The caption shown at `time`, framed by the sequence size: the cue containing it (every cue containing it, stacked
/// in start order, when the track's style allows overlap), with its karaoke highlight, its speaker's name in the
/// speaker's colour (showSpeakers, with `project`), and its animation at `time` (pop: scaled in over 0.15 s; fade: in
/// and out over 0.2 s; word by word: the words not spoken yet are hidden, the layout stays). With `project` (whose
/// sequence it is), words its redactions bleep or mute are drawn as `settings.redactedWord` (redactedCueText):
/// evaluateVideoLayers passes it, so the program monitor and export burn in the same text.
std::optional<render::CaptionFrame> evaluateCaption(const Sequence& sequence, Ticks time, const CaptionSettings& settings,
                                                    const ProjectState* project = nullptr);

/// One proxy the preview may decode instead of a media item's original.
struct ProxyEntry {
    std::string path; ///< UTF-8
    /// Resolution divisor the proxy was made with (2 = half): placement scales its frames back up.
    int divisor = 2;
};

/// The proxies of a project by media id: the editor's proxy registry (MediaLocator::proxy is its saved copy).
/// Immutable once published; the scheduler swaps whole tables.
struct ProxyTable {
    std::unordered_map<std::uint64_t, ProxyEntry> entries;
};

/// What the preview decodes (Scheduler::setPreview). Export never uses these: it always decodes originals.
struct PreviewSettings {
    /// Decode a media item's proxy instead of its original where it has one. An item whose original is offline
    /// plays from its proxy either way (and export refuses it until it is relinked).
    bool useProxies = false;
    /// Decode resolution as a fraction of the original (1, 2 or 4): frames larger than original / divisor are
    /// box-downscaled right after decoding (FFmpeg has no reduced-resolution decoding for H.264/HEVC/ProRes), so
    /// caches, uploads and composites work on fewer pixels. A proxy is only reduced further when it is still
    /// larger than that.
    int decodeDivisor = 1;
    /// The proxy registry; null = each item's MediaLocator::proxy.
    std::shared_ptr<const ProxyTable> proxies;
    /// C.16: the project's preview render cache (PreviewCache.h). Inside a rendered segment whose file still shows what
    /// the snapshot shows (PreviewCache::validAt), the scheduler decodes that file instead of compositing the layers
    /// (burned-in captions are still drawn on top). Set it again (Scheduler::setPreview) after a render finishes so the
    /// held picture and the lookahead pick the file up.
    std::shared_ptr<PreviewCache> previews;

    bool operator==(const PreviewSettings&) const = default;
};

/// The file the preview decodes for a media item.
struct ResolvedSource {
    const std::string* path = nullptr; ///< null: nothing to decode (offline without a proxy, no path)
    bool proxy = false;
    /// The proxy stands in for an offline original.
    bool originalOffline = false;
    /// Resolution divisor of `path` relative to the original (1 for the original).
    int divisor = 1;
};

/// Resolves `item` for the preview: its proxy when `preview.useProxies` and it has one (or when the original is
/// offline), else its original. Points into `item` or into `preview.proxies` (valid while both are held).
ResolvedSource resolveSource(const MediaItem& item, const PreviewSettings& preview);

/// Whether `spec` needs a decoded frame (false for generated layers such as titles, and for nested groups).
inline bool needsDecode(const LayerSpec& spec) {
    return !spec.title && !spec.caption && !spec.nested && !spec.graphic && !spec.fill && !spec.syntheticPicture;
}

/// Visible video layers of `sequence` at `time`, bottom (V1) to top, following the model's evaluator
/// obligations (docs/architecture.md):
///   * the frame at `clip.sourceTimeAt(time)` (speed, reverse, freeze), parameters (Motion, Opacity and every
///     stack effect) at `clip.curveTimeAt(time)`, keyframe curves in media time;
///   * the stack in order, skipping disabled instances and those `isEvaluable()` rejects (counted);
///   * inside a transition's range [cut - before, cut + after), both clips: A past its Out point and B before
///     its In point read their handles; the pair is one transition group (A then B), one-sided transitions
///     give a group of one.
/// Disabled tracks and clips, clips without a usable video stream, offline media (unless `preview` resolves a
/// proxy for it) and fully transparent layers are skipped. With `cullOccluded`, layers completely covered by an
/// opaque layer above (no alpha, opacity 1, no effects, no transition, an axis-aligned frame covering the
/// canvas) are skipped too: the scheduler then decodes only what can be seen, evaluating from the topmost
/// enabled track down.
///
/// With `captions`, the cue under `time` of the chosen caption track is added on top (evaluateCaption()).
///
/// A video track with effects (Track::effects, I.9) becomes a group over its layers (a clip, a nested subtree, a
/// transition's pair) carrying the track's effects at the level's timeline time; the sequence's master stack
/// (Sequence::effects) is a group over all its tracks, below the burned-in caption. A layer on a track with effects
/// never counts as covering the canvas.
///
/// Nested and multicam clips (C.8, docs/internal/c8-nested-multicam-handoff.md) become a group (LayerSpec::nested): the nested
/// sequence evaluated the same way at the clip's source time (speed, reverse, freeze and remap applied), over the tracks
/// nestedTracksRead() gives (a multicam video clip: its one active angle's track, at the angles' sync), each leaf with
/// its own effects and the redactions of its media that neither it nor a clip on its path excepts, its tracks'
/// effects, then the nested sequence's master effects (not for a multicam source), then - when the top level shows
/// `captions` - the nested sequence's own caption (its first enabled caption track, in that track's style, at nested
/// time; I.9), then the group placed with the nested clip's Motion, Opacity, effects and transition. Past the nested
/// sequence's duration, or with nothing visible inside, a nested clip shows nothing. Groups are never treated as
/// covering the canvas.
void evaluateVideoLayers(const ProjectState& project, const Sequence& sequence, Ticks time,
                         std::vector<LayerSpec>& out, bool cullOccluded = true,
                         const PreviewSettings* preview = nullptr, const CaptionSettings* captions = nullptr);

/// The compositor's form of a video transition at `progress` (0..1 over its length, before its Start and End are
/// applied): the kind and options its descriptor's parameters give (render::LayerTransition, render/reference/
/// Transitions.h); user colours become linear light, the seed of random patterns is the transition's id. nullopt for
/// an id the compositor does not implement (shown as a plain cut).
std::optional<render::LayerTransition> transitionMixAt(const Transition& transition, double progress);

/// 0.2: what an Animations transition (model/Animations.h: Pop, Spin, Flip...) does to the layer it animates.
struct LayerAnimation {
    render::Vec2 offset{};        ///< added to the layer's position (sequence pixels)
    double scale = 1.0;           ///< times its scale
    double rotationDegrees = 0.0; ///< added to its rotation
    double opacity = 1.0;         ///< times its opacity
    /// After its own effects: Basic 3D, then Transform keeping Fold's hinge edge in place (Flip, Fold); Block Dissolve.
    std::vector<render::LayerEffect> effects;
};
/// The animation of an Animations transition at `progress` (0..1 over its length, before Start and End are applied):
/// animating in (an In point, the incoming clip of an edit point) or, with `out`, out (an Out point: the In animation
/// reversed, turns carrying on). `canvas` is the level's, `frame` the animated layer's frame (Basic 3D's). Surprise Me
/// that was never resolved animates as its id picks. nullopt for any other transition.
std::optional<LayerAnimation> transitionAnimationAt(const Transition& transition, double progress, bool out,
                                                    render::CanvasSize canvas, render::CanvasSize frame);

/// Source time of `clip` at timeline time `t`, also outside its range (transition handles): Clip::sourceTimeAt
/// without the clamp to the clip's own source range (never negative). A freeze frame shows its source In.
Ticks sourceTimeAround(const Clip& clip, Ticks t);
/// Curve time of `clip` at `t`, also outside its range (Clip::curveTimeAt extended like sourceTimeAround).
Ticks curveTimeAround(const Clip& clip, Ticks t);

/// What a few library effects read besides the curve time (0.2, docs/effects.md): the sequence's frame rate (the
/// Transform effect's shutter, Timecode), times and names (Timecode, Clip Name) and the canvas the text effects draw on.
/// Without a context those effects draw nothing extra (no motion blur, no text).
struct EffectContext {
    Rate frameRate{};
    bool dropFrame = false;
    render::CanvasSize canvas{};
    Ticks clipTime{};          ///< time since the clip's start
    Ticks sourceTime{};        ///< the media time shown
    std::optional<Ticks> mediaStartTimecode;
    std::string clipName;      ///< in the sequence
    std::string projectClipName;
    std::string fileName;
    /// The project the clip is in: Image Matte Key reads its still image item (null: no image).
    const ProjectState* project = nullptr;
};

/// The stack effects of `clip` the compositor implements, evaluated at `curveTime`; `bypassed` counts the
/// enabled instances it cannot run. A title's "wabcut.title.style" contributes its shadow (a Drop Shadow at its
/// place in the stack). A colour grade ("wabcut.color.grade") is evaluated whole (color::evaluateGrade: curves baked
/// once per distinct set, LUTs found through `luts`; without a resolver only built-in LUTs resolve). Effect masks
/// (I.6) evaluate through render::maskSpecAt; `frameAspect` = the frame's displayed width / height. `mediaTime` = the
/// media time the frame shows (Clip::sourceTimeAt): AI object mattes are looked up there, not at the curve time, so a
/// clip remapped or frozen after its matte was made still shows the matte of the frame on screen (AI-14 review).
/// `context`: what the time and text effects need (0.2, EffectContext).
void evaluateEffects(const Clip& clip, Ticks curveTime, std::vector<render::LayerEffect>& out, int& bypassed,
                     const color::LutResolver* luts = nullptr, double frameAspect = 16.0 / 9.0,
                     std::optional<Ticks> mediaTime = std::nullopt, const EffectContext* context = nullptr);

/// The redactions of `clip`'s media covering media time `sourceTime` and not excepted for the clip, evaluated there.
/// With the stream's `frameRate`, also those marking any moment of the frame on screen at `sourceTime` (the frame
/// at or before it): a marked frame is redacted for as long as it shows, whatever the sequence's rate or the clip's
/// speed.
/// The coverage of `redaction`'s region at media time `mediaTime` as playback renders it: always opaque, never inverted,
/// grown by half its feather so the marked area is fully covered (Codex wave-4 P2-29). `frameAspect` = the frame's
/// displayed width / height. Null when the region has nothing to draw.
std::shared_ptr<const render::MaskSpec> redactionMaskAt(const Redaction& redaction, Ticks mediaTime, double frameAspect);

/// `path`: the nested and multicam clips leading to `clip` (C.8): a redaction that excepts one of them does not apply.
std::vector<render::LayerRedaction> evaluateRedactions(const ProjectState& project, const Clip& clip, Ticks sourceTime,
                                                       double frameAspect, Rate frameRate = {},
                                                       std::span<const ClipId> path = {});

/// The title a title clip draws at `curveTime`: text and font from its generator media, numeric styling from its
/// "wabcut.title.style" effect (defaults when it has none), framed by the canvas.
render::TitleStyle evaluateTitle(const MediaItem& title, const Clip& clip, Ticks curveTime, render::CanvasSize canvas);

/// The composite layers of evaluated `specs` with their decoded `frames` (frames[j] for specs[j]; an invalid frame
/// = failed, or not needed): stack effects, transition groups (a pair missing one side becomes one-sided) and
/// generated layers (titles, drawn by render::TitleRenderer::shared()). The preview scheduler and export both
/// build composites with it, so preview == export. Frames are moved out.
///
/// A frame smaller than its media's stream (a proxy, a reduced-resolution decode) is placed exactly where the
/// original would be: Motion is defined in original pixels, so its scale grows and its anchor shrinks by the
/// height ratio, and the frame's pixel aspect (kept by proxies and downscales) absorbs any width rounding.
///
/// P.2: flow mixes (Optical Flow interpolation groups, Morph Cut) take their motion from `motion` (null, or no motion
/// there: they mix as cross dissolves); `canvas` is the top level's size (Morph Cut analyses on it; empty: no Morph
/// Cut motion).
void buildCompositeLayers(const std::vector<LayerSpec>& specs, std::vector<media::VideoFrame>& frames,
                          std::vector<render::CompositeLayer>& out, MotionSource* motion = nullptr,
                          render::CanvasSize canvas = {0, 0});

/// P.2: the Morph Cut analyses `sequence`'s video tracks (the top level) need, for the background analysis Premiere
/// starts as soon as the transition is applied (the same requests buildCompositeLayers makes there).
std::vector<MotionRequest> morphCutRequests(const ProjectState& project, const Sequence& sequence,
                                            const PreviewSettings* preview = nullptr);

/// The Motion transform that shows a `frameHeight`-pixel-high frame of a stream `streamHeight` pixels high exactly
/// where the full-size frame would be (identity when they match).
render::LayerTransform transformForFrameHeight(const render::LayerTransform& transform, int streamHeight,
                                               int frameHeight);

/// The file to decode for `item` (its MediaLocator): the proxy when `preferProxy` and one exists, else the
/// original; null when the media is offline or has no path. Points into `item` (valid while its snapshot is
/// held). Relative-path relinking is the project loader's job; the locator's absolute path is authoritative
/// here.
const std::string* sourcePathOf(const MediaItem& item, bool preferProxy = false);

/// Whether a layer with `transform` and an opaque `frameWidth` x `frameHeight` frame covers the whole canvas.
bool coversCanvas(const render::LayerTransform& transform, double frameWidth, double frameHeight,
                  double pixelAspect, render::CanvasSize canvas);

} // namespace wabcut::playback
