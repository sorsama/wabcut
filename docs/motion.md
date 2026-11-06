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
| AI Frame Interpolation (0.7) | the frame between made by a neural model (RIFE, run on this computer, downloaded on request): Process makes the pictures ahead of time; where one is missing the clip plays with Optical Flow and the monitor says so; an export makes it. See [ai.md](ai.md#ai-frame-interpolation-and-ai-refocus-07) |

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
- **AI Frame Interpolation (0.7).** The same group, with the model's picture alone on its canvas instead of the mix
  (`SourceFrameProcessor::interpolate`, keyed by the exact source time in 1/64 frame); while it is missing the Optical Flow
  mix stands in and the layer is flagged `aiUnprocessed`. Saved as `opticalFlow` + `"aiInterpolation": true`.
- **Render bar.** Retimed clips with Optical Flow (and AI Frame Interpolation) are red ("optical flow"), with Frame Blending yellow ("frame
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
- **Face Aware (0.7).** A talking head is the hardest part of a jump cut for optical flow: cheeks and forehead have no
  texture to follow, the mouth opens and closes, and a head that moved by more than a few patches loses its way, so the
  face ghosts or tears. The Morph Cut's **Face Aware** switch (Effect Controls, on by default; `faceAware` in
  `transition.set`) lines the two sides' faces up first: a face detector (YuNet, 0.2 MB, the one Auto Reframe uses; the
  header says **Face detector needed** with a **Download** and its licence until it is on this computer) finds the faces on
  both sides, and where it finds the same face on both (matched by place and size) its five landmarks (eyes, nose tip,
  mouth corners) give the head's own motion - the shift, turn and size that carry one side's landmarks onto the other's
  (`motion/FaceAlign.h`). Inside the face's window (the face box 1.7 times as large, feathered at its edge) the flow is
  estimated again as a *residual* on the pictures aligned by that transform, so even a large move is within the
  estimator's reach, and each pixel takes the flow, plain or face-aligned, whose pictures agree better there (the error
  over 5 x 5 pixels), in proportion to the window's weight. Outside the windows nothing changes. On the test material
  (`tests/motion/test_face_align.cpp`: a synthetic head that moves a tenth of the picture, grows, turns and opens its
  mouth) the face at the mid point is 2.6 times closer to the true in-between picture than the plain flow gives and 3.8
  times closer than a dissolve; `tests/playback/video/test_motion.cpp` does it end to end through the export path.
  **Fallbacks, silently and always to the old Morph Cut:** the switch off, the detector not downloaded, no AI helper in
  this build, no face on one side, faces that do not match (a head that moved by more than itself, or twice its size),
  landmarks that make no sense (a head that grew by more than two thirds or turned by more than 35 degrees in the
  picture's plane). The detector looks at the luma the flow is estimated on, so preview, Render In to Out and export
  agree, and what it finds is part of the motion's cache key (a Morph Cut analysed before the download is analysed again
  after it). Limits: one detection per side (a face that is only turned away on one side is not matched), and the
  alignment is a similarity: a head that turns in depth (profile to frontal) morphs as the plain flow does.

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
| Smoothness | 50 % (0..1000); can be keyframed (0.7, see below) |
| Method | Position; Position, Scale, Rotation; Perspective; Subspace Warp (default; computed as Perspective); **Gyro (camera metadata)** (0.7, for footage with gyro data) |
| Preserve Scale | off |
| Framing | Stabilize Only; Stabilize, Crop; Stabilize, Crop, Auto-scale (default); Stabilize, Synthesize Edges |
| Auto-scale | Maximum Scale 150 %, Action-Safe Margin 0 % |
| Additional Scale | 100 % |
| Advanced | Detailed Analysis (off), Rolling Shutter Ripple (Automatic Reduction, Enhanced Reduction), Crop Less <-> Smooth More 50 %, Synthesis Input Range, Synthesis Edge Feather |
| Gyro (0.7, only with that Method) | Sync (Auto-sync with the picture, default; Off), Offset (0 ms, -2000..2000), Field of View (0 = Auto, degrees), Rolling Shutter Read-out (0 = Auto, ms) |

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

**Keyframed Smoothness** (0.7). Smoothness has a stopwatch like Motion's controls: key it to steady only part of a shot.
The Gaussian window of each analysed frame takes its width from the Smoothness at that frame, so a ramp from 0 % to 50 %
brings the stabilization in gradually, and **0 % leaves the shot exactly as it was shot** at that frame (the camera moves
as it did). The keys are read when the shot is analysed, at the frames' media times (a time-remapped clip: at the output
frame that shows them), so after moving a key the header reads **Out of date**; **Reanalyze** (or Analyze All) re-smooths
from the cached motion without decoding a frame. Crop Less <-> Smooth More scales every key. The L1 smoothing (the reframe
tools') uses the mean of the keys; No Motion ignores Smoothness. A project that keys Smoothness does not open in an older version
(it treats a keyed constant setting as an invalid project).

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
and keys the instance. A nested clip is refused (analyse the clips inside it, or use Analyze All); a time-remapped clip is analysed over the frames its remap shows (see Speed changes below).

**Speed changes** (0.7, roadmap #107). A clip at another constant speed (also reversed) is analysed over the source frames
it shows; its keys are at the media's times, which are its curve times, so each timeline frame finds the compensation of
the source frame it shows - no nesting. A **time-remapped** clip (speed ramps, holds, reverse segments) is analysed too:
the frames its remap shows are tracked once (the cached motion is per media), and the compensation is keyed per frame the
clip shows on the timeline, in the clip's curve time (which runs with the timeline): Frame Sampling takes the compensation of
the whole frame it shows, Frame Blending and Optical Flow interpolate between the analysed frames. Changing the remap
afterwards means **Reanalyze** (the keys follow the remap as it was).

**Analyze All** (0.7, roadmap #128). **Sequence > Analyze All Stabilizers** (also the command palette, and **Analyze All** next to
Analyze in a stabilizer's Effect Controls header, which shows when the sequence has more than one) analyses every Warp
Stabilizer and Rolling Shutter Repair of the sequence, the sequences nested in it and the Master Clip Effects of its media
that is **not analysed or out of date** - the clip was trimmed longer or moved onto other footage, or a setting such as
Smoothness changed (the header then reads "Out of date") - in one job with one progress bar and one undo step "Analyze All
Stabilizers"; effects already current are skipped (**Reanalyze All** redoes them), and the media's cached motion makes a
re-analysis quick. Effects that cannot be analysed (switched off, a freeze frame, nothing to track) are listed in the
message. `effect.analyzeAll` (`kind`, `reanalyze`, `nested`) does the same from the CLI, MCP and the assistant.
Morph Cut needs no such step: its analysis starts when the transition is applied and runs in the background.

**Warning before export** (0.7, roadmap #107). A Warp Stabilizer or Rolling Shutter Repair that has not been analysed (or is
out of date) exports **without** its stabilization, so the Export panel warns first - "2 stabilizers are not analyzed:
V1: Interview (Warp Stabilizer) ... They will export unstabilized" - for the stabilizers inside the range being exported,
with **Analyze now** (the same job as Analyze All). The export itself stays allowed.

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
as Premiere's banner). A Warp Stabilizer or Rolling Shutter Repair in the **Master Clip Effects** (shown in Effect
Controls under the clip's "Master Clip" group) is analysed on the media: over the frames every clip of that media shows
(one contiguous stretch of the file), keyed on the media item, so every instance plays stabilized. A clip added later
outside that stretch needs Reanalyze.

**Measured cost** (same box, loaded by parallel builds): tracking at the analysis width (640 x 360 for 16:9) takes
about 15 ms a frame for Position, Scale, Rotation or Perspective (65 frames/s, so a minute of 30 fps footage analyses in
about 30 s) and about 31 ms with Detailed Analysis; re-smoothing a cached analysis is instant. The pictures come from
the app's decoders, whose cost adds to that.

## Stabilize with the camera's gyro

(0.7, roadmap #130.) Many cameras record their own gyroscope in the video file. **Method: Gyro (camera metadata)** in a Warp
Stabilizer uses that rotation instead of tracking the picture, so the correction does not depend on what is in the shot
(featureless sky, water, a dark room, a face filling the frame) and has no drift. Adobe asks for the same ([feature
request](https://community.adobe.com/feature-requests-730/add-gyro-data-support-to-warp-stabilizer-1327942)).

**What is read.** The gyro (and accelerometer) come out of the file's data tracks as untrusted binary input: every length is
bounded, a damaged packet ends that packet's data, never the program. The reading is by camera:

| Camera / format | Status | Notes |
|---|---|---|
| **GoPro** HERO5 and later (MP4, `gpmd` track, GPMF) | **Read** | Gyro and accelerometer, the readings timed by the camera's clock (STMP, HERO8 and later) or spread over their packet; the IMU's axis order from ORIN (or the order GoPro lists for the camera). The axis mapping is checked against real HERO5, HERO6, HERO7 and HERO8 footage. GoPro's format is documented by GoPro ([GPMF](https://github.com/gopro/gpmf-parser)). |
| **Sony** Alpha, FX, ZV and RX cameras with a gyro (MP4: XAVC S, XAVC HS, XAVC S-I; `rtmd` track) | **Read** | Gyro and accelerometer per frame, the sensor's read-out time (the rolling shutter), the exposure, and the lens's focal length and sensor size (the field of view) when the file has them; the camera's model from its NonRealTimeMeta XML. Sony does not publish the layout: it follows the open-source parser of the Gyroflow project (see THIRD-PARTY-NOTICES.md). The axis table of Sony cameras is **not checked against footage** here (none was available), so Auto-sync checks it against the picture and turns the axes when that fits much better. |
| Sony MXF (XDCAM, XAVC in MXF, Venice) | Not read | The metadata sits in MXF system items, not in a track. |
| **DJI** (Osmo Action, Avata, Neo, drones) | Not read | The gyro is in a protobuf `djmd` track whose layout DJI has not published; the community's reverse-engineered schemas differ per model and none could be checked against a real file here. The Effect Controls note says so. |
| **Insta360** | Not read | The IMU data is a trailer in Insta360's own format, not documented by Insta360. |
| **RED** R3D | Not read | The gyro is read through the REDCODE SDK, which cannot be linked into Wabcut (R3D plays through a decoder helper that carries pictures only). |
| **Blackmagic RAW** | Not read | The gyro is read through Blackmagic's RAW SDK (same reason). |
| Canon, ARRI, Panasonic, Z CAM, phones (CAMM) and others | Not read | Other formats; the picture-based methods work as before. |

A clip whose file has no readable gyro keeps the picture-based methods: the Method menu offers **Gyro (camera metadata)** only for
footage with gyro data (or for a stabilizer that already uses it), and a file that has gyro data Wabcut does not read says why in
the note under the header. The project's `analyzeStabilization` command refuses a Gyro analysis of such a file with the same
reason.

**The method.** The rotation is integrated from the angular velocity (quaternions, body rates) and put in one camera frame (x
right, y down, z out of the lens). For every analysed frame the camera's orientation is read at the middle line's exposure
(the frame's time + the sync offset + half the sensor's read-out). The path is smoothed in **rotation space** - not on the
picture, so a camera that turns through a hundred degrees is as good as one that shakes - with the same Gaussian-weighted local
linear fit and the same Smoothness as the picture-based path: Smoothness is keyframable (0 % leaves the shot exactly as shot),
Result: No Motion locks the camera to the first frame, Crop Less <-> Smooth More scales the smoothing. The correction of
each frame is the rotation between the real camera and the smoothed one as a homography on the picture, `K E^T K^-1`, with the
lens's focal length in pixels in K. A **rolling-shutter** camera (the file gives its read-out time, or you do) has the skew
of its scan taken out first from the angular velocity, the way Rolling Shutter Repair does from the picture's motion. The framing (Stabilize Only, Crop,
Crop with Auto-scale, Synthesize Edges, Maximum Scale, Action-Safe Margin, Additional Scale) is the picture-based path's, on the same
corner-pin keys (`corner.*`), so **preview and export draw the same keys** as for Perspective. A frame the gyro does not reach is left as shot.

**Sync and field of view (Auto-sync).** The file does not say everything: the gyro's clock and the picture's can be a few
milliseconds apart, the lens's field of view depends on the lens mode and any crop, and an unchecked camera's axes may be turned. With
**Sync: Auto-sync with the picture** (the default) the analysis measures the picture's own motion on up to three stretches
of about two seconds spread over the clip (Similarity tracking, cached like every analysis), compares it with the gyro
integrated over each frame interval, and searches the offset (+-0.5 s, to a millisecond), the 24 turns of the axes and the
focal length (the least squares scale between the two). The note under the header reports it: "Synced to the picture: gyro
+12 ms, lens 96 degrees (measured), 94 % match". It is applied when the gyro explains the picture well enough (a match of 50 %
or more, higher for fewer frames); when it does not (the camera's own stabilization was on and moved the picture against the
gyro, or the picture hardly moves) the camera's own timing and the file's or the camera's usual field of view are used and the note says
so. **Offset** is added to the sync (a manual trim, or the whole offset with Sync off); **Field of View** and **Rolling Shutter
Read-out** replace the automatic values when set. The lens is modelled as rectilinear: a very wide mode (GoPro Wide, SuperView,
HyperView) is right near the centre and approximate toward the edges, so Linear or a narrower mode steadies best.

**Use it well.** Turn the camera's own stabilization off (GoPro HyperSmooth, Sony Active SteadyShot, in-body or lens
stabilization): it moves the picture against the gyro, which no metadata here undoes. Analyze (or Analyze All) as for
the other methods; speed changes and time remapping work as in "Speed changes" above (the analysis is per source frame), and a
changed Gyro setting makes the header read **Out of date**.

**Commands.** `effect.analyzeStabilization` with the effect's Method 4 does it for the assistant and MCP inside the app (the effect's
`gyroSync`, `gyroOffset`, `gyroFov`, `gyroReadout` settings; `data.gyro` reports the camera, the sync and the field of view); like
the picture-based analysis it needs the app's media readers, and a host without them refuses it with a message. The camera note is the
effect's data `gyro`. In the UI, **Effect Controls** puts one line under the stabilizer's header: what the file has ("This clip has
gyro data (GoPro HERO8 Black, 202 readings per second)"), how the analysis synced it, or why the file's camera data is not used
("No gyro for this clip: DJI records its gyro in a metadata track whose layout DJI has not published ...").

**Not in this version, and the next steps:** a horizon lock from the accelerometer (it is read, not used yet); GoPro's published
lens polynomial (ZFOV, POLY) for a true wide-lens model; Sony's in-body stabilization and lens tables; a per-line mesh for the rolling
shutter (the skew here is the affine, first-order one); DJI, Insta360, RED and Blackmagic where their layouts or SDKs allow; MXF.

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