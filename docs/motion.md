# Motion: Time Interpolation, Morph Cut, Warp Stabilizer

P.2 brings Premiere Pro's motion-estimation features to Wabcut: **Time Interpolation** (Frame Sampling, Frame Blending,
Optical Flow) for every retimed clip, the **Morph Cut** transition for jump cuts, **Warp Stabilizer** and **Rolling
Shutter Repair**. They share one motion module (`src/render/motion`, `wabcut_motion`): dense optical flow, the flow
interpolation the compositor implements, and the parallel loop they use. No third-party code: the estimator is written
from the published method (Dense Inverse Search, below).

## Time Interpolation

Premiere: *Clip > Video Options > Time Interpolation*, also in the *Speed/Duration* dialog
([Adobe help: work with clip speed](https://helpx.adobe.com/premiere-pro/how-to/work-with-clips-channels-speed.html),
[slow motion](https://www.adobe.com/learn/premiere-pro/web/slow-motion)).

| Method | What a frame between two source frames shows |
|---|---|
| Frame Sampling (default) | the frame nearest below the source time: frames repeat or drop (what every clip did before) |
| Frame Blending | the two frames around the source time, cross-dissolved by where the source time lies between them |
| Optical Flow | both frames warped along their motion toward the source time and blended, each side weighted by how surely it sees the point (occlusions) |

It applies wherever a clip's video is retimed (`retimesFrames`, `model/TimeInterpolation.h`): a speed other than 100 %
(Speed/Duration, Rate Stretch), Reverse Speed, Time Remapping ramps, and footage at another frame rate than the sequence
(24 or 30 fps footage in a 60 fps sequence). Freeze frames, stills, generated items and nested sequences sample.
Footage with an assumed frame rate (Clip > Modify > Interpret Footage, `MediaItem::interpretation`) counts at that rate:
its clips' conform speed (`edit::conformSpeed`, a 120 fps capture assumed 30 fps plays at 1/4) is their 100 %, so in a
30 fps sequence every captured frame shows as it is, while the same capture assumed 24 fps in a 30 fps sequence, or
slowed further, is retimed and interpolated between its own 120 fps frames. The interpretation's pixel aspect and alpha
options hold inside the interpolation and stabilizer groups too.

- **Model.** `ClipContent::timeInterpolation` (shared by both halves of a razor cut), saved as clip
  `"timeInterpolation": "frameBlending" | "opticalFlow"` only when not Frame Sampling (project schema 17).
  `edit::setTimeInterpolation` (`model/MotionOps.h`) sets it on video clips (linked video partners too; audio partners
  are left alone), one undo step; the change reports `PropertyRole::Source`.
- **Commands.** `clip.setTimeInterpolation {clips, method}`; `clip.speedDuration` takes `timeInterpolation` too (the
  dialog's section; alone it also works on time-remapped clips).
- **UI.** The Speed/Duration dialog's *Time Interpolation* segmented control, the clip menu's *Time Interpolation*
  submenu and the Time Remapping section's *Time Interpolation* button.
- **Evaluation** (`playback/video/Evaluate.h`). A retimed clip with Frame Blending or Optical Flow becomes a group
  (`LayerSpec::interpolation`): a canvas of the stream's displayed size holding the frame containing the exact source
  time and the frame after it (`interpolationNextTime`: the first frame's PTS plus its duration, so variable frame rate
  footage pairs the right frames). `buildCompositeLayers` computes where the source time lies between the two PTS
  (`interpolationWeight`, snapped to a frame within 1/1024) and mixes them: a cross dissolve, or a flow mix along the
  motion between them. The group is then placed with the clip's Motion, Opacity, effects, redactions and transition,
  exactly like the frame would be.
- **Render bar.** Retimed clips with Optical Flow are red ("optical flow"), with Frame Blending yellow ("frame
  blending"): render them (Sequence > Render In to Out) for real-time playback, as in Premiere. The setting is part of
  the segment's content hash, so changing it drops the preview file.

## Morph Cut

Premiere: *Video Transitions > Dissolve > Morph Cut*
([Adobe help: Morph Cut](https://helpx.adobe.com/premiere-pro/using/morph-cut.html),
[troubleshooting Morph Cut](https://helpx.adobe.com/premiere-pro/kb/morph-cut.html)).
A transition (`"morphCut"`, `kMorphCutTransition`) like the others: drag it on an edit point, default duration and
alignment, `transition.add` / `transition.set`.

Over the transition each side is **frozen at the end it touches**: the outgoing clip's picture at the transition's
start and the incoming clip's picture at its end. The output morphs the first into the second along their motion (the
flow mix at the transition's progress), so it starts and ends exactly on the neighbouring frames, and no media beyond
the cut shows (the words the jump cut removed do not come back). That is why Adobe advises starting and ending it near
the peaks of the last and first words. The motion is estimated between the two sides **as they are placed on the
canvas** (each with its own Motion: a punch-in on the second shot is part of the morph).

- **Analysis** starts as soon as the transition is applied (the scheduler asks for every Morph Cut of the sequence
  whenever the project changes, `morphCutRequests`), and the program monitor shows **Analyzing in background** while
  it runs; until a frame's motion is there the monitor holds the frame. Export always computes it. A side that is not
  plain media (a title, a nested sequence) dissolves instead.
- **Render bar.** Red ("Morph Cut") over the transition.
- Not done: face-aware weighting. The flow follows the whole picture; on a talking head before a still background the
  face dominates the motion anyway.

## Motion analysis

`playback::MotionAnalyzer` (`playback/video/MotionAnalysis.h`) serves both: a `MotionRequest` names two pictures (a file,
a stream, a source time - the frame containing it - and where the frame lies on the canvas) and the canvas. The analyzer
decodes the **original files itself** on the CPU, whatever the preview decodes (proxies, reduced resolution, decoder
surfaces), draws each picture's luma on the canvas at the analysis size (longer side `motion::kAnalysisSide` = 960 px),
and estimates the motion both ways. The result depends only on the request, so the program monitor, Render In to Out and
export show the same pixels (preview == export). Results are cached (LRU, 256 MB); the preview asks in the background
(two workers, urgent requests first, the lookahead prefetched), export computes on its decode thread.

**The estimator** (`motion/Flow.h`) is Dense Inverse Search (Kroeger, Timofte, Dai, Van Gool, ECCV 2016), coarse to
fine over a 5-tap binomial pyramid: on each level a grid of 8 x 8 patches every 4 pixels, each patch's displacement by
inverse compositional Gauss-Newton on the zero-mean patch (brightness changes do not pull it) from the coarser level's
field, kept only when it lowers the error and stays within a patch of where it started; densification by the patches
covering each pixel weighted by how well each fits there; then a 3 x 3 median. Analysis estimates down to half the
analysis size (480 x 270 for 16:9). Deterministic on any number of threads.

**The flow mix** (`shaders/flowmix.frag`; CPU reference `motion::interpolate`) at progress t, with forward motion F and
backward motion G sampled at the output point x (the linear-motion approximation of Super SloMo): the point was at
x0 = x - (1 - t) t F + t^2 G in A and is at x1 = x + (1 - t)^2 F - t (1 - t) G in B; each side's forward-backward
disagreement e gives its visibility 1 / (1 + e^2); the output is the visibility- and time-weighted average of A(x0) and
B(x1). It runs in linear premultiplied light on the level's intermediates, like the other transition mixes.

**Measured cost** (the development box, 10 cores, shared with parallel builds at 60-85 % CPU): one flow pair (both
directions) for a 1080p frame, analysed at 960 x 540, takes 45-62 ms on the CPU (`Optical flow speed at the analysis
size` in the motion suite prints it), so Optical Flow and Morph Cut preview below real time until rendered, as in
Premiere; a pair is computed once per pair of source frames and cached. The GPU mix is one extra pass per mixed frame.

## Warp Stabilizer and Rolling Shutter Repair

Premiere: *Video Effects > Distort > Warp Stabilizer* and *Rolling Shutter Repair*
([Adobe help: Warp Stabilizer settings](https://helpx.adobe.com/premiere/desktop/add-video-effects/commonly-used-effects/warp-stabilizer-settings.html)).
Both are ordinary clip effects (`"wabcut.stabilize.warp"`, `"wabcut.stabilize.rollingShutter"`, Distort bin) with
Premiere's settings; their analysis is a command, so the CLI, MCP and the assistant reach it too.

| Warp Stabilizer setting | Values (default) |
|---|---|
| Result | Smooth Motion (default), No Motion |
| Smoothness | 50 % (0..1000) |
| Method | Position; Position, Scale, Rotation; Perspective; Subspace Warp (default; computed as Perspective) |
| Preserve Scale | off |
| Framing | Stabilize Only; Stabilize, Crop; Stabilize, Crop, Auto-scale (default); Stabilize, Synthesize Edges |
| Auto-scale | Maximum Scale 150 %, Action-Safe Margin 0 % |
| Additional Scale | 100 % |
| Advanced | Detailed Analysis (off), Rolling Shutter Ripple (Automatic Reduction, Enhanced Reduction), Crop Less <-> Smooth More 50 %, Synthesis Input Range, Synthesis Edge Feather |

**Analysis** (`effect.analyzeStabilization`, `src/ai/stabilize`; ported from the parked C.15 branch and finished here).
The clip's media is tracked frame by frame at 640 px wide (the host's pictures: the app's decoders, a fake in tests):
features spread over a grid of cells (Shi-Tomasi), the I.6 pyramidal Lucas-Kanade tracker with a forward-backward
check, then a robust fit (RANSAC and least squares) of a translation (Position), a similarity (Position, Scale,
Rotation) or a homography (Perspective) against a keyframe that is kept while it overlaps well, so errors do not pile
up. A frame that cannot be matched (a cut, a blank frame) is lost and holds the camera. Detailed Analysis tracks two
and a half times the features with a tighter forward-backward check. The raw motion is cached per media fingerprint
(analysis cache), so changing Smoothness, Result or Framing and analysing again re-smooths without decoding, as in
Premiere.

**Smoothing.** The camera path (the composed motions) is smoothed per parameter: a Gaussian-weighted local linear fit
with sigma = Smoothness seconds (a pan keeps its speed to the last frame), or locked to the first frame (No Motion).
Preserve Scale keeps the path's scale (push-ins stay). When the framing crops, Crop Less <-> Smooth More scales the
smoothing (x0.5 at 0 %, x1.5 at 100 %). The compensation for each frame is "smoothed path after the inverse of the real
path"; Stabilize, Crop, Auto-scale then finds the smallest uniform scale (capped at Maximum Scale, Action-Safe Margin
left out) that hides every moving border, times Additional Scale; Stabilize, Crop reports the edges ever uncovered.

**Rolling Shutter Ripple** (Perspective and Subspace Warp only: a corner pin can carry a skew). A rolling-shutter
camera reads its lines one after the other, so while the picture moves each line shows it a little further on: the
camera path's skew follows the motion. `rollingShutterShares` measures the scan's share of the frame time by regressing
the path's skew on the frame's motion: one share for the clip (Automatic Reduction) or one per frame over a window of a
second each side (Enhanced Reduction, for ripple that changes through a shot). A global-shutter camera, or too little
change in motion to tell, measures 0 and nothing changes. Each frame is then de-skewed before its compensation, and the
path is smoothed as a global shutter would have shot it.

**The result is keys.** The compensating transform is keyed per analysed frame into the instance (`transform.x/y`,
`transform.rotation`, `transform.scale` for Position and Position, Scale, Rotation; `corner.*` offsets for Perspective),
sparse within a tolerance (auto reframe's curve fit), in the clip's curve time; `autoScale` and `crop.*` hold the
framing, data `status` ("analyzed") and `analysis` (a summary) the state. The keys stay editable; one undo step adds
and keys the instance. A time-remapped or nested clip is refused (analyse the media, then remap).

**Rendering** (`playback/video/Evaluate.cpp`, `stabilizationOf`). A clip with an analysed stabilizer becomes a group
on a canvas of the stream's displayed size: inside, the frame (or its Time Interpolation group) drawn through
`CompositeLayer::warp`, the projective warp the keys give (applied about the frame centre, before the clip's Motion,
Opacity, effects and transition, which stay on the group); Stabilize, Crop puts a Crop of the analysed edges first in
the group's effects. Stabilize, Synthesize Edges draws the unwarped frame behind the stabilized one, so the borders show
the frame's own picture rather than black - Premiere synthesizes them from neighbouring frames, which Wabcut does not
yet. The stabilizer itself is not a stack effect at render time (it is skipped in the effect chain).

**Rolling Shutter Repair** (`effect.analyzeRollingShutter`). Settings: Rolling Shutter Rate (50 %), Scan Direction (Top
to Bottom, Bottom to Top, Left to Right, Right to Left), Advanced Method (Warp, Pixel Motion), Detailed Analysis, Pixel
Motion Detail. The analysis keys the picture's motion per frame (`motion.x`, `motion.y`, source pixels per frame: central
differences of the tracked camera path; it shares the Position analysis in the cache). Each line then moves back by
its share of the scan: line u (0 at the first line read, 1 at the last) shifts by -rate * (u - 0.5) * motion, a shear
of the frame in the same group as Warp Stabilizer's (both compose when a clip has both). Pixel Motion is treated as
Warp (a per-pixel flow would be the next step).

**UI.** Effect Controls' header of either effect shows its state (Not analyzed, Analyzing N %, Analysis complete) and
**Analyze** / **Reanalyze** / **Cancel**: the `Stabilizer` singleton runs the command on a worker with the media
decoders' pictures and the analysis cache and applies the keyed instance as one undo step ("Analyzing in background",
as Premiere's banner).

**Measured cost** (same box, loaded by parallel builds): tracking at the analysis width (640 x 360 for 16:9) takes
about 15 ms a frame for Position, Scale, Rotation or Perspective (65 frames/s, so a minute of 30 fps footage analyses in
about 30 s) and about 31 ms with Detailed Analysis; re-smoothing a cached analysis is instant. The pictures come from
the app's decoders, whose cost adds to that.

## Bars and Tone, Universal Counting Leader

Premiere: *File > New > Bars and Tone / HD Bars and Tone / Universal Counting Leader* (parked C.15 work, finished
here). Synthetic project items (`media.newSynthetic`, `model/Synthetic.h`) like Black Video, Color Matte and
Transparent Video, made for the sequence's frame size and rate:

- **Bars and Tone**: SMPTE colour bars (pattern 0, HD RP 219; pattern 1, SD EG 1) with a sine tone (1 kHz at -20 dBFS
  by default; `tone.frequency`, `tone.level`) on one stereo stream. The picture is drawn per pixel
  (`render/SyntheticPicture.h`), the tone by the audio feed (`renderSyntheticAudio`).
- **Universal Counting Leader**: 8 seconds counting down from 8 to 2 (then black) with a clock wipe, crosshair and
  target circles in Premiere's colours (each settable: wipe, background, line, target, numeral) and cue blips (on 2 by
  default; at the out point, at every second).

They are in the Library's tools menu (*New item*) and need no file; projects save them as generators.