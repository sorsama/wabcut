# Masks, tracking and source-anchored redaction (I.6)

Owner: Graphics/playback. Code: `src/model` (Masks.h, MaskOps.cpp: the data and its edits), `src/render` (Mask.h,
shaders/mask.frag: the GPU passes), `src/render/masks` (the tracker and the commands; pure C++), `src/playback`
(evaluation and the redaction sound), `src/editor` (Tracking.h: pictures for the tracker), `src/app` (the UI).

Wabcut's masks decide **where** something happens: a clip cut out (opacity masks), an effect or the colour grade
applied only inside a shape (local corrections, blur and mosaic patches), and privacy redactions that follow a source
recording into every cut made from it. The tracker makes the shapes follow the picture; redactions also mute or bleep
the words they cover.

Everything here is ordinary project data: masks live in a clip's effect instances, redactions in the project, tracking
results are plain keyframes. Preview and export render them with the same code (the compositor and the track renderer),
so what the monitor shows is what an export, a burn-in or a proxy-free render contains.

## Model

### Masks (`model/Masks.h`)

A mask belongs to an effect instance (`EffectInstance::masks`, at most 16 per instance) and has an `id` (unique in its
list), a `name`, a `shape` (`ellipse`, `rectangle`, `path`), a `mode` (`add`, `subtract`, `intersect`), `inverted`,
`enabled`, keyframable parameters and, for paths, path keys.

Coordinates are **normalised source-frame coordinates**: (0, 0) is the frame's top-left, (1, 1) its bottom-right, of
the stored frame (before the clip's Motion). Lengths that must be isotropic (feather, expansion) are in **frame
heights**; rotation and roundness are isotropic in displayed pixels (the frame's display aspect). Keyframe times are
**curve time**, like every clip parameter. For ordinary clips this is media time; with C.6 Time Remapping or a frame
hold it runs with the timeline (`sourceIn + timeline offset`), so effect-mask animation continues through held and
reversed source segments. Source redaction geometry instead always uses the displayed source picture's PTS.

| parameter | default | range | meaning |
|---|---|---|---|
| `center.x`, `center.y` | 0.5 | -10..10 | ellipse / rectangle centre |
| `size.x`, `size.y` | 0.5 | 0..20 | width and height (of the frame) |
| `rotation` | 0 | ±36000° | the shape's rotation |
| `roundness` | 0 | 0..1 | rectangle corner radius (of the shorter side) |
| `feather` | 0 | 0..1 | soft edge width (frame heights) |
| `expansion` | 0 | -1..1 | grow (+) or shrink (-) (frame heights) |
| `opacity` | 1 | 0..1 | how much the mask covers |
| `transform.x`, `transform.y` | 0 | -10..10 | offset (what the tracker writes) |
| `transform.scaleX`, `transform.scaleY` | 1 | ±100 | scale |
| `transform.rotation` | 0 | ±36000° | rotation |
| `transform.skew` | 0 | ±85° | skew (affine tracks) |
| `transform.anchorX`, `transform.anchorY` | 0.5 | -10..10 | the point the transform turns and scales about |

A **path** is a closed cubic Bezier: vertices with in/out tangents as offsets (zero tangents = a corner). An animated
path has keys with the same vertex count; shapes interpolate vertex by vertex (Hold, Linear, Ease*; Bezier is treated
as Ease In/Out). An ellipse is 4 Bezier arcs, a rectangle 8 vertices (rounded corners).

**Combination**: a list starts empty when its first mask adds and full when it subtracts or intersects. Then, mask by
mask, with `a` = opacity × (inverted ? 1 − coverage : coverage): add `m = m + a − m·a`, subtract `m = m·(1 − a)`,
intersect `m = m·a`. The feather is a smoothstep across the signed distance (±feather/2 around the outline, moved out by
the expansion); without feather the edge is antialiased over one pixel.

Effects with masks: any video stack effect (Basic Correction, Vignette, Gaussian Blur, Sharpen, Crop, Drop Shadow,
**the colour grade**) changes only the masked area: its result is mixed into its input by the coverage. Two effects
exist for masks alone:

- `wabcut.mask` (Masks category): the clip's **opacity mask** - the layer's alpha times the coverage (cut-outs);
- `wabcut.mosaic` (Stylize): pixelation in cells of `cellSize` frame heights; with a mask it is a mosaic patch.

Bounds (project files are untrusted): 16 masks per instance, 256 vertices, 10 000 path keys, names of 1 024 bytes;
`maskProblem()` reports anything else malformed (non-finite numbers, key times not increasing, mismatched counts).

### Object masks (`MaskShape::Matte`, AI-14)

A mask of shape `matte` shows, at each frame, an alpha matte a segmentation model made from the media (the AI Object
Mask, [ai.md](ai.md#ai-object-mask-ai-14)): `Mask::matte` names its track in the analysis cache (`track`, opaque, at most
256 bytes), the `media` and video `stream` it was made from, the media time it covers (`source`) and the `model`. It
keeps its **bounding box** as `center.*` / `size.*` keys (Hold, the union of the object's boxes over windows of frames,
so the box always contains it): where a frame has no matte (not tracked there, another computer, a cleared cache) the box
stands in. Every other parameter means what it means for any shape: `feather` softens the matte's edge, `expansion`
grows it or (negative) chokes it, `inverted` covers everything but the object, `opacity` and `mode` combine it. The
schema is 12: an older build opening a project with a matte mask says it was made by a newer version.

**On nested and multicam clips (0.2, schema 18).** The matte is made from what the clip shows - its nested sequence
rendered at nested time with only the tracks it shows (a multicam clip's active angle) - and `matte` then names that
`sequence` instead of a `media` (exactly one of the two is set). Its frames are at nested time, which is the nested
clip's curve time, so the compositor looks them up exactly as for a media clip; the mask applies to the nested clip's
composited picture. An angle switch keeps the mask (where it was on the frame); `mask.propagate` re-tracks the angle
shown then. Builds before schema 18 refuse such a project.

### Redactions (`Redaction` in `model/Masks.h`)

A redaction marks a region of a **media item** in **source time**: `media`, `source` (the media time range), `region`
(a mask whose key times are media times; its mode is ignored), `style` (`blur`, `mosaic`, `solid` + colour),
`strength` (blur radius / mosaic cell in frame heights), `audio` (`none`, `mute`, `bleep`) with optional `audioRanges`
(transcript word ranges; empty = the whole `source`), and `exceptions` (clips it skips: "don't redact here").

Because it is anchored in the source, it applies to **every use** of that range in every sequence: trimmed, sped up,
slowed down, reversed, frozen, reused, nested - without anyone re-marking it. A frame is redacted when **any moment** of
it (the decoded frame's span) lies in the marked range, so a marked frame stays covered for as long as it is on screen,
at any sequence rate. A clip cut from an excepted clip (the other half of a razor cut) is redacted again until it is
excepted too: the safe direction.

Before decode, evaluation uses the nominal frame interval. The shared preview/export layer builder replaces that
estimate with the decoded picture's actual PTS and duration, including VFR pictures that cross a marked range. Its
tracked region stays at that picture's PTS throughout its display interval. A range marked through a remapped clip
includes every source excursion between its endpoints, including interior reverse/forward extrema.

A redaction always renders at full strength over its whole marked area (`playback::redactionMaskAt`): its region's
opacity is ignored (always 1), it is never inverted (the commands refuse `inverted`; an older file's flag is ignored),
and it is grown by half its feather, so the soft edge lies **outside** the marked area instead of across it. Source and
word timecodes in `redaction.add` / `redaction.update` are read in the **media's** rate (`sourceTimecodeRate`), like the
Source Monitor's, never the sequence's. A `region` is either a complete box (`x`, `y`, `width`, `height`, optional
`shape` and `feather`) or mask fields; unknown keys, a missing coordinate, a zero size or an inverted region are
refused.

**Captions.** A word the soundtrack bleeps or mutes is never shown as text either (`playback/video/CaptionRedaction.h`):
burned-in captions (program monitor and export draw the same thing) and SRT/VTT sidecars replace it with `[bleep]`
(`CaptionExportSettings::redactedWord`). With karaoke word timings exactly the bleeped words are replaced; without them
which word was bleeped is unknown, so the whole cue becomes `[bleep]`. Plain clips map the bleep through their speed
and direction, remapped clips exactly in their 100 % segments and conservatively whole elsewhere.

**Nested and multicam clips (C.8, I.8).** A redaction reaches its media inside nested and multicam clips at any depth,
in the preview, export, scopes and review thumbnails alike. A leaf inside a nest carries the redactions its media has
at its source time unless the leaf **or any nested clip on its path** is excepted: excepting a nested clip excepts
everything inside it (the resolver's rule, `ResolvedLeaf::redactions`). The nested picture is rendered with its leaves'
redactions applied before it is placed; a nested picture that cannot be rendered is left out, never drawn without them
(fail closed). Review package thumbnails come from the export path, so they are redacted too, and a thumbnail that
cannot be rendered that way is missing rather than unredacted.

**One source, many items (review wave 5).** A redaction marks the source material, not the Library item it was drawn
on (`model/RedactionMedia.h`): a subclip plays its master's file at the master's media times, and separated stems are
sample-aligned with their source, so they share one **redaction root**. A redaction on a master covers every clip of its
subclips and stems, and one drawn while the Source Monitor shows a subclip covers the master's clips - picture, sound,
caption mask, thumbnails, uploads, transcription and search alike (`redactionsCovering`). A master that was removed
still anchors what was made from it. A rendered music remix take rearranges time, so a redaction cannot be mapped onto
it: `audio.remix` refuses to render a take of a stretch with muted or bleeped audio (its un-rendered pieces are clips of
the same media and stay redacted). The caption mask follows speech into nested and
multicam clips like the picture does (the active angle only; excepting a nested clip excepts what is inside it).
History's "restore as a new sequence" keeps media ids (a renamed or relinked item is the same source; a removed one
comes back under its own id, where its redactions apply again), and the clipboard carries the copied media's
redactions, so a paste into another project, or after the item was removed here, is redacted too.

**Memory (C.10, I.8).** `ProjectState::redactions` is a `SharedList`: snapshots share it until a redaction changes, so a
long tracked redaction (about 420 KB of keys per tracked minute) is no longer copied by every edit and undo step; the
undo budget counts it only for a step that changed it.

**Pictures outside the compositor.** Library thumbnails, timeline filmstrips and smart-search hit frames are decoded
straight from the file; they get the source's redactions on the CPU (`app::redacted`: the same hardened coverage, a box
blur, the mosaic of the small picture, or the solid colour), in display orientation, and their cache keys change with
the redactions so a new one re-renders them. The Source Monitor maps its redaction tool and outlines through the private
clip's real placement (display rotation, pixel aspect), so phone and anamorphic footage are covered where they are
drawn.

## Rendering (render/Mask.h, shaders/mask.frag)

Playback evaluates the masks at the frame (`render::maskSpecAt`: outlines flattened, 16 segments per Bezier curve,
transform, edge, combination) and the compositor renders them on the GPU in three steps:

1. **Distances**: per shape, the exact signed distance to its flattened outline (frame heights, positive inside by the
   even-odd rule) into an RGBA16F texture over the shape's bounds (at least one frame) plus a 25 % margin, or more when
   the soft edge reaches further (feather / 2 + expansion, in whole margins, part of the cache key),
   at 1.5× the layer's resolution. Each shape has its own domain, so an off-screen outline moved back into view is
   covered correctly. Cached by the outline: a static shape, or one only moved by its (tracked) transform, is never
   rasterised again.
2. **Coverage**: per frame, each shape's distances sampled through its inverse transform (bilinear - the GPU-friendly
   approximation of the exact distance after a transform), feathered, expanded, inverted, weighted and combined, four
   shapes per pass (more ping-pong). Cached per owner (layer + effect or redaction) while its inputs are unchanged.
3. **Apply**, in canvas space through the layer's placement: masked effects mix their result into their input by the
   coverage; opacity masks multiply the layer; redaction styles mix in before any of the clip's effects.

`maskCoverageCpu()` is the double-precision reference with exact distances. The GPU coverage matches it at 64-76 dB PSNR
(tests/render/test_masks.cpp: ellipse, feathered ellipse, rotated rounded rectangle, Bezier path, inverted and partial
opacity, expansion both ways, subtract, intersect, a first subtracting mask, a skewed and scaled transform, six shapes);
masked Basic Correction and blur match their references mixed by the reference coverage at 60-62 dB.

Initial I.6 GPU measurements, before the C.6 integration fixes, on one 1920x1080 layer (RTX 3060, D3D11;
median whole-frame GPU time minus the unmasked frame, test-output/render/masks/perf.md). These are historical
measurements; the coordinator's final quiet-machine gate on combined master is pending:

| case | mask cost |
|---|---|
| static Bezier mask (cached coverage: the apply pass only) | 0.11 ms |
| tracked mask (transform changes every frame: coverage again) | 0.17 ms |
| reshaped mask (outline changes every frame: distances + coverage) | 1.8 ms |
| four shapes, one tracked | 0.43 ms |
| a masked 20 px blur, beyond the blur itself | 0.01 ms |

**Mattes** (`render/Matte.h`): a matte mask's distances come from the installed `MatteSource` (the app, wabcut-cli and
export install `ai::segment::CacheMatteSource`; the render code never reads the cache) as a signed-distance field over
the frame - the matte's exact Euclidean distance transform, frame heights, positive inside - uploaded as an R16F texture
(RGBA16F where unsupported) into the shape's distance slot (step 1), so steps 2 and 3 are those of any shape. **Fail
closed**: for redactions (`MatteUse::Redaction`) the source returns, at a frame without a matte or where the model lost
the object, the nearest present matte grown by 1 % of the frame height + 15 % per second of the gap (at most half the
frame); with no matte at all the redaction covers the mask's bounding box grown by a quarter. A redaction never covers
nothing. GPU / CPU parity holds for mattes too (tests/render/test_matte_masks.cpp: 72-86 dB PSNR with feather, choke,
grow, invert on opacity, combinations and a moved matte); preview equals export (tests/export/test_matte_export.cpp,
59 dB minimum over 30 frames), and thumbnails / filmstrips (RedactedPictures) and scopes go through the same coverage.

Redaction styles: **blur** (Gaussian, sigma = strength / 2 frame heights), **mosaic** (cells of `strength` frame heights,
each the average of its block through the mip chain), **solid** (an opaque colour). Each is mixed over the layer's own
pixels by the region's coverage, before the clip's effects, so no effect (sharpen, grade) can bring the original back.

## Redaction sound (playback/audio/AudioRedactions.h)

The audio ranges of the enabled redactions are resolved from the project snapshot **off the audio thread** - by the
player's feeder when the snapshot changes (the queued audio is refilled from the edit cutover, ~40 ms ahead of the
playhead) and by an export's offline mix when it starts - and applied by `TrackRenderer` to each clip's source audio
after its Volume and Panner, before its effects. Inside a range the clip's sound is replaced, sample-accurately in media
time: silence (**mute**) or a **1 kHz sine at -12 dBFS** (**bleep**) whose phase follows media time. 5 ms gain ramps sit
just **outside** the range, so nothing of it is heard and nothing clicks. Preview and export run the same code on the
same samples: bit-identical (tests/playback/test_redaction_audio.cpp); the audio callback is unchanged and stays free of
allocations and locks with redactions edited while playing (tests/playback/test_rt_audit.cpp).

For C.6 Time Remapping, only straight forward 100 % spans have sound. Redaction replacement intersects those spans
and follows their actual source time; holds, reverse segments and ramps remain silent, including through effects.

## Tracking (masks/Tracker.h, masks/MaskCommands.h)

The tracker works on the clip's **own source frames** (not the edited picture), as luma with square pixels at a working
width (640 by default): `editor::MediaTrackingService` decodes them from the original, or from its proxy when the
original is offline (frame for frame the same). Per frame it

1. picks up to 120 good features (Shi-Tomasi) inside the region on the previous picture;
2. follows each with pyramidal Lucas-Kanade (4 levels, 15×15 window, sub-pixel) and back again, keeping those whose
   round trip lands within 0.5 px;
3. fits the region's motion - **translation**, **similarity** (move, turn, scale) or **affine** - with RANSAC (0.75 px)
   and least squares on the inliers;
4. reports a **confidence** (the share of features that agree, discounted by their residual). Below `minConfidence`
   (0.3) or with fewer than 6 inliers the frame is **lost**: tracking stops there and the keyframes end at the last
   frame tracked.

On synthetic moving textures with exact truth (tests/masks/test_tracker.cpp) the error is 0.003 px per frame, 0.1 px
after 20 frames of turning and scaling, and 0.12 px after tracking 20 frames forward and back again; on a real H.264
clip panning a known amount, within 1e-6 of the frame after 20 frames. Initial speed at 640×360 (one thread) was
~90-120 frames per second (test-output/masks/tracker-perf.md); the final combined-master timing gate is pending.

**track.mask** composes the tracked motion after the mask's transform at the start frame and writes `transform.*`
keyframes (x, y; + scale and rotation for similarity; + skew for affine) at each frame's media time, replacing keys in
the tracked span, as one undo step. Forward or backward, one frame or to the clip's end. The direction is the
timeline's: on a reversed clip "forward" runs back through the media. A mask that starts flipped (negative scale X)
keeps keys on the flipped decomposition, so it never collapses between them. Mask ids are per effect: without
`effect`, a mask id that several effects hold is refused (name the effect).

**track.transform** tracks a region (a rectangle, a mask's outline, or a detector's find) and keys a clip's Motion:
**follow** moves another clip with the region (Position; with similarity also Scale and Rotation), **stabilize** keys the
tracked clip itself so the region stays where it was, upright and the same size.

Tracking a source clip with Time Remapping enabled is currently rejected before decoding, with a message to turn it
off first. This prevents writing source-frame keys into the remapped clip's different curve-time domain. Existing
mask keys and source redactions still evaluate through remapping. A UI tracking job also checks its starting project
snapshot and sequence before committing; intervening edits or cancellation discard its result without adding undo
history or overwriting the newer edit.

**Face / person detection** is a seam: `TrackingServices::detect(picture, kind)` (AI-9 plugs a detector in there); until
then `{"detect": "face"}` fails with a clear message.

## Commands

All edits are one undo step and support dry runs. The job commands (`track.*`) report progress through the request's
checkpoints (MCP progress notifications) and through `TrackingServices::progress`, and can be cancelled.

| command | what it does |
|---|---|
| `mask.add` | a mask on an effect (`effect`) or on the clip's opacity mask; shape, mode, params, path |
| `mask.update` | name, shape, mode, inverted, enabled, params (numbers or `{"keys": [...]}`), path / pathKeys |
| `mask.remove`, `mask.list` | |
| `track.mask` | `clip`, `mask`, `time`, `direction`, `frames`, `model`, `workingWidth`, `minConfidence` |
| `track.transform` | `clip`, `region`, `target`, `mode` (`follow` / `stabilize`), and the tracking options |
| `redaction.add` | by `media` + `source`, or through a `clip` between `in` and `out`; `region`, `style`, `strength`, `color`, `audio`, `words` |
| `redaction.update`, `redaction.remove`, `redaction.list` | |
| `redaction.override` | except one clip (and its linked partners), or `redact: true` again |
| `mask.objectSelect` | AI-14: click (`points`), draw (`box`) or name (`subject`) an object on a frame; SAM 2.1 segments it and tracks it through the shot; a matte mask on `effect` or the opacity ([ai.md](ai.md#ai-object-mask-ai-14)) |
| `mask.refine`, `mask.propagate` | more clicks on a frame of an object mask; track it further |
| `redaction.detectFaces` | 0.2: every face of a `media` item (between `in` and `out`) or a `clip`'s part of it, found by the face detector, followed (ByteTrack, reset at shot cuts) and covered by its own moving redaction (an ellipse keyed at the samples, grown by `margin`); `style`, `strength`, `samplesPerSecond`, `minFaceHeight`, `name`. Fail closed: a face seen once is redacted |

```json
{"command": "redaction.add", "params": {"clip": {"track": "V1", "time": "00:00:04:00"}, "in": "00:00:03:00",
 "out": "00:00:06:00", "style": "mosaic", "audio": "bleep",
 "region": {"x": 0.62, "y": 0.55, "width": 0.14, "height": 0.08}}}
```

The agent sees these commands as tools like every other, except the redaction ones that change the project
(`redaction.add`, `.update`, `.remove`, `.override`, `.detectFaces`): privacy stays in the user's hands. The Source
Monitor's Redactions popover runs `redaction.detectFaces` ("Redact faces") in the style chosen there.

## User interface

Everything goes through the `Masks` QML singleton (src/app/src/bridge/MaskBridge.h): typed edits on the session, one
undo step per click, drag or slider gesture.

- **Effect Controls > Masks** (under the effect stack, for the clip the panel shows): the **target** menu - the clip's
  *Opacity mask* (cut-outs) or one of its video effects, the colour grade included (*local*: the effect changes only
  the masked area); the tools **Ellipse**, **Rectangle**, **Pen** (they draw on the program monitor) and **+** (a
  centred ellipse); one row per mask (shape, name, a crosshair when tracked, SUB / INT for the mode, invert, on/off,
  delete); for the selected mask its mode (Add / Subtract / Intersect), **Feather**, **Expansion** and **Opacity** (a
  key at the playhead when the parameter is animated), and **Track**: to the start, one frame back, one frame forward,
  to the end, Stop, with a progress bar and a **confidence strip** (one bar per frame: green sure, amber unsure, red
  lost) and what happened ("Tracked 120 frame(s)", "Lost after 37 frame(s)").
- **Program monitor**: the target's outlines (the selected one solid, the others dashed), the selected mask's feather
  band (dashed, inside and outside) and handles - centre (move), corner (size), the round handle above (rotate), path
  points (move) and their tangent ends, the expansion square and the feather dot on the right. Click an outline to
  select its mask. Ellipse / Rectangle: drag a box. Pen: click for corners, drag for curves, click the first point (or
  Enter) to close, Esc cancels. The overlay sits beside the monitor slot (the slot keeps its one child) and maps
  through the clip's Motion, so handles stay on the picture when the clip is moved, scaled or rotated.
- **Source Monitor**: the **Redact** button (eye-off) arms the tool - drag over the picture to cover that region from
  In to Out (no marks: the whole media) in every sequence that uses the source; the **Redactions** button (list)
  shows the style and sound of the next one (Blur / Mosaic / Solid, Sound / Mute / Bleep) and this source's
  redactions with their media time range, style, sound, on/off, *don't redact on the selected clip* and delete. The
  mini timeline shows redacted ranges in red; the regions covered at the source position are outlined over the
  picture (which already shows them: the Source Monitor's private copy carries the redactions).

- **Object** (AI-14, the sparkles tool in the mask section): click the person or object on the program monitor (the minus
  toggle or Alt-click: not this), or drag a box around it; more clicks refine it; **Track** backward / both ways /
  forward through the shot with progress, cancel and the confidence strip; the matte **preview** (the object tinted, or
  the matte in grey). An object mask has no handles: its outline is the bounding box that stands in where a frame has no
  matte. The Source Monitor's redaction popover has **Track an object (AI)**: the drag then marks an object, followed
  through In..Out, and the redaction's region is its matte. Screenshots: test-output/ui/object-mask-*.png.

Screenshots: test-output/e2e/screenshots/masks_local_correction.png (a feathered local correction with its handles),
masks_effect_controls.png, masks_tracking_progress.png (mid-track: progress and confidence), masks_tracked.png,
redaction_two_sequences.png (one redaction, two sequences) and redaction_source_monitor.png; test-output/ui/masks-*.png.

## Limitations

- `track.transform` places the tracked point through the clip's Motion; a display-rotated media file is tracked in its
  stored orientation (masks are, too).
- A collapsed shape (fewer than three outline points or a singular transform) covers nothing, but still combines
  like any shape: inverted it covers the whole frame, as an Intersect it clears it, as a Subtract it changes nothing. The
  CPU reference and the GPU agree on this.
- Tracked **effect** masks (a masked blur used for privacy) are evaluated at the requested source time; only
  redactions are re-pinned to the decoded frame's PTS, so under rate conversion an effect mask can lead its subject by
  up to a frame. Use a redaction for privacy (Codex wave-4 P2-35, by design for now).
- The thumbnail disk cache, the analysis cache and the smart-search index keep unredacted pictures (and embeddings) of
  a source on this computer; what the app *shows* is redacted.
- The tracker does not re-acquire a lost region by itself: move the mask back and track again from there.
- Object masks need their mattes on this computer (the analysis cache); elsewhere the bounding box stands in. They are
  refused on remapped clips and frame holds (curve time is not media time there).
- Automatic face redaction finds what the face detector (YuNet) finds: a face in profile, very small (below
  `minFaceHeight`) or heavily blurred can be missed, so review the result before you publish; between samples the
  ellipse is interpolated with a margin rather than tracked per frame.
