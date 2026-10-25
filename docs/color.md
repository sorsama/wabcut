# Colour grading: the Color workspace, the grade, LUTs, scopes (I.5), colour management and HDR (I.9, 0.2)

Owner: Graphics/playback. Code: `src/render/color` (the colour library: grade maths, LUTs, CDL, scopes, Auto and Match,
the commands), `src/render/shaders/grade.frag` and `scopes.comp` (GPU), `src/render/src/Compositor.cpp` (grade passes),
`src/render/src/GpuScopes.cpp`, `src/playback/src/Evaluate.cpp` (the grade at a frame),
`src/editor/src/ColorFrames.cpp` (pictures for the tools), `src/app/src/bridge/ColorBridge.cpp` / `ScopesBridge.cpp` and
`src/app/qml/panels/color/` (the workspace).

## Background

Editors expect one grading effect that does everything a colourist needs for a delivery (a Lumetri-class stack: basic
correction, creative look, curves, wheels, a secondary, a vignette), live scopes and a before/after, not a chain of small
effects. Wabcut's version is one effect, `wabcut.color.grade`, rendered on the GPU at preview and export alike, with a
double-precision CPU reference of exactly the same maths (the tests compare them) and "smart" tools (Auto, Color Match,
Match Scene) that write ordinary, editable parameters instead of baking a result.

## Where it lives

- **The Color workspace** (title-bar tabs: Edit · **Color** · Audio · Captions · Export; workspace id `wsColor` = 3):
  the **Scopes** panel replaces the browser on the left, the **Color** panel takes the side slot, the program monitor
  gets the height.
- **The Color panel** grades the selected video clip (else the topmost clip under the playhead): Basic Correction
  (input LUT, white balance, tone, saturation), Creative (look, intensity, faded film, tints, vibrance, saturation),
  Curves (RGB, hue vs sat/hue/luma, luma vs sat, sat vs sat), Color Wheels (lift, gamma, gain, offset + saturation),
  HSL Secondary (with mask view), Vignette. Every section has an on/off switch and a reset; the panel switch bypasses the
  whole grade. **Auto**, **Match** (to a reference), **Match Scene**, the secondary's eyedropper (click the monitor:
  the HSL qualifier's hue, saturation and luma ranges centre on a 5×5 average of the picture without the secondary),
  CDL import/export and the comparison views are in its header and menus. One drag = one undo step.
- **Comparison views** (monitor overlay, preview only, never exported): before/after split with a draggable divider
  (left: the clip without its grade), reference side by side (any frame of the sequence or a clip, held as a still),
  and the secondary's mask view.
- **The grade is an effect**: it sits in the clip's effect stack, its scalars keyframe like any other effect's, and
  it exports exactly as previewed.

## The pipeline

Stages run in a fixed order. A stage at its neutral values is skipped, so an untouched grade costs nothing and changes
nothing. "Linear" is the compositor's scene-linear Rec.709 working space; "display" is BT.709-encoded values (the
BT.709 OETF, extended beyond [0, 1]). The authoritative contract is `src/render/color/include/wabcut/color/Grade.h`.

| # | Stage | Domain | What it does |
|---|---|---|---|
| 1 | Input LUT | display | a .cube (log → Rec.709 and the like), full strength |
| 2 | White balance | linear | von Kries gains in Bradford cone space (temperature, tint), luminance-normalised |
| 3 | Exposure | linear | × 2^stops |
| 4 | Contrast | linear | a power about 18 % grey: 0.18 (x / 0.18)^(1 + c/100) |
| 5 | Highlights, shadows, whites, blacks | display | one monotone cubic (PCHIP) of the encoded luma through five points, added to R, G and B alike (no clipping, no hue shift) |
| 6 | Saturation, vibrance | linear | luminance-preserving; vibrance weighs low-saturation pixels and protects skin hues |
| 7 | Creative | display | look LUT × intensity (above 100 % extrapolates), faded film, shadow/highlight tints with balance, creative vibrance and saturation |
| 8 | Curves | display | RGB curves (master, then R/G/B) as monotone cubics baked to 1024-entry tables; periodic hue curves; luma vs sat, sat vs sat (0.5 = no change) |
| 9 | Wheels | display | ASC CDL v1.2: slope = gain − lift, offset = lift + offset, power = gamma, clamped "ASC" style; then CDL saturation |
| 10 | HSL secondary | display | a matte from hue/saturation/luma ranges (softness, invert), refined by denoise and blur, then temperature/tint, contrast, sharpen, saturation and an offset mixed in by the matte |
| 11 | Vignette | linear | `wabcut.vignette`'s formula |

The descriptor declares `Neighborhood | LinearLight`: the secondary's refine and sharpen read neighbouring pixels, so
the compositor renders the layer to an intermediate first, exactly as for blur.

### Parameters (`effect.list` has them all with ranges)

62 keyframable scalars, registry keys by stage:

- Stage switches: `basic.enabled`, `creative.enabled`, `curves.enabled`, `wheels.enabled`, `hsl.enabled`,
  `vignette.enabled` (1 on, 0 bypassed).
- Basic: `basic.temperature`, `basic.tint` (−100..100), `basic.exposure` (−5..5 stops), `basic.contrast`,
  `basic.highlights`, `basic.shadows`, `basic.whites`, `basic.blacks`, `basic.vibrance` (−100..100),
  `basic.saturation` (0..200, 100 = unchanged).
- Creative: `creative.lookIntensity` (0..200), `creative.fadedFilm` (0..100), `creative.vibrance`,
  `creative.saturation`, `creative.shadowTint.hue/.amount`, `creative.highlightTint.hue/.amount`,
  `creative.tintBalance`.
- Wheels: `wheels.lift.red/green/blue` (−1..1), `wheels.gamma.*` (0.1..10), `wheels.gain.*` (0..8),
  `wheels.offset.*` (−2..2), `wheels.saturation` (0..4).
- HSL: `hsl.hue.center/.width/.softness` (degrees), `hsl.sat.low/.high/.softness`, `hsl.luma.low/.high/.softness`,
  `hsl.invert`, `hsl.denoise`, `hsl.blur`, `hsl.temperature`, `hsl.tint`, `hsl.contrast`, `hsl.sharpen`,
  `hsl.saturation`, `hsl.offset.red/green/blue`.
- Vignette: `vignette.amount` (−5..5), `vignette.midpoint`, `vignette.roundness`, `vignette.feather`.

### Data keys (not keyframable)

Curves and LUT references are the effect instance's `data` (written to the project file only when present; no schema
change):

| Key | Kind | Meaning |
|---|---|---|
| `curves.master`, `curves.red`, `curves.green`, `curves.blue` | points | input → output in [0, 1]; empty = identity |
| `curves.hueSat`, `curves.hueHue`, `curves.hueLuma` | points | x = hue in turns (periodic), y 0.5 = no change |
| `curves.lumaSat`, `curves.satSat` | points | x in [0, 1], y 0.5 = no change |
| `basic.inputLut`, `creative.look` | text | `builtin:<name>` or `asset:<id>` (a project LUT asset); empty = none |

Untrusted data is bounded when a project loads: at most 64 entries per instance, 256 points per curve, 1 KiB of text;
numbers finite, x strictly increasing. A reference to a LUT asset that no longer exists is a warning
(`assetReferenceWarnings`), never a load failure: the stage is bypassed and the panel shows "LUT missing".

## LUTs

- **Format**: `.cube` (Adobe Cube LUT Specification 1.0, plus Resolve's 1D shaper + 3D form). 3D up to 65³, 1D up to
  65536 entries, `DOMAIN_MIN/MAX`, `LUT_3D_INPUT_RANGE`, CRLF/LF, BOM, comments. Parsing is defensive and reports the
  line of the first problem (sizes, data count, non-finite numbers, bad domains, repeated keywords, file size).
- **Sampling**: tetrahedral by default (smooth for log conversions), trilinear on request. The GPU stores each 3D table
  as an RGBA32F 3D texture and samples it with `texelFetch` on the lattice, so GPU and CPU agree to float rounding.
- **Project assets**: `lut.import` or the panel's "Import LUT…" adds the file as a project asset (kind `lut`). Assets
  are checked on disk and relinked like media: an offline LUT counts in the "files cannot be found" prompt, is listed
  in the relink dialog ("name (LUT)"; Locate… or Search folder…), and **Collect Files** copies the referenced ones into
  `LUTs/` of the capsule (manifest role `lut`). Importing the same file again returns the existing asset.

### Built-in looks and input LUTs

Built-ins are generated from maths at first use and cached; **no LUT file is shipped** and nothing third-party is
redistributed. Camera conversions implement the manufacturers' published formulas (formulas are facts, not licensed
works); the rendering choices (legal-range input, Wabcut's highlight roll-off) are Wabcut's and under the project
licence.

| Reference | Name | Size | Source and licence |
|---|---|---|---|
| `builtin:teal-orange` | Teal & Orange | 33³ | Wabcut (generated; project licence) |
| `builtin:warm-film` | Warm Film | 33³ | Wabcut (generated; project licence) |
| `builtin:cool-night` | Cool Night | 33³ | Wabcut (generated; project licence) |
| `builtin:bleach-bypass` | Bleach Bypass | 33³ | Wabcut (generated; project licence) |
| `builtin:mono-silver` | Mono Silver | 33³ | Wabcut (generated; project licence) |
| `builtin:slog3-sgamut3cine-to-rec709` | Sony S-Log3 (S-Gamut3.Cine) to Rec.709 | 65³ | S-Log3 transfer function and S-Gamut3.Cine primaries from Sony's published "Technical Summary for S-Gamut3.Cine/S-Log3 and S-Gamut3/S-Log3" (maths only, no Sony LUT); rendering by Wabcut (project licence) |
| `builtin:vlog-vgamut-to-rec709` | Panasonic V-Log (V-Gamut) to Rec.709 | 65³ | V-Log transfer function and V-Gamut primaries from Panasonic's published "V-Log/V-Gamut Reference Manual" (maths only, no Panasonic LUT); rendering by Wabcut (project licence) |

Input LUTs expect legal-range files (code value = 64 + 876 v of 1023), decode the log curve, convert the gamut to
Rec.709 primaries (D65, via XYZ from the published xy primaries), apply a soft highlight roll-off above 18 % grey and
encode BT.709. **Not included**: Canon Log 3 and Apple Log — their constants could not be verified against the
manufacturers' documents in this change; users can import the vendors' own .cube files as assets.

### Export .cube and Lumetri presets (0.2)

- **Export .cube** (Premiere's Lumetri panel menu; the Color panel's menu, `lut.export`): the grade at the playhead
  (the command: at `time`, default the clip's middle) baked into a 3D LUT of 33 points per side (`size` 2..65) by the
  CPU reference (`gradeToLut`, `color/Grade.h`): display-referred Rec.709 code values in and out, the form Wabcut and
  other applications apply a look in, outputs clamped to 0..1. Every pixel-local stage goes in: the input LUT, Basic,
  Creative (with its look), Curves, Color Wheels and the HSL secondary through its qualifier. A LUT cannot hold what
  depends on where a pixel is or on its neighbours: the **Vignette**, the secondary's denoise / blur / sharpen and the
  effect's masks are left out and reported (`leftOut`). Applying the exported file as a look at 100 % reproduces the
  grade within the lattice's interpolation (tested: at most 0.02 of full scale on a strong grade). Premiere's `.look`
  (SpeedGrade) and `.itx` exports are not offered.
- **Save Preset** (the Color panel's menu, or right-click Color Grade in Effect Controls): the grade as an effect preset
  in the Effects panel's Presets bin ([effects.md](effects.md) "Presets"); a LUT file it uses is kept by path.
- **Lumetri Presets**: the Effects panel's Lumetri Presets bin applies Color Grade with each built-in look (Cinematic:
  Teal & Orange, Cool Night; Filmstock: Warm Film, Bleach Bypass; Monochrome: Mono Silver) or camera input LUT
  (Technical). Apply a look or LUT file from the Basic section's Input LUT and the Creative section's Look menus ("Import
  .cube…").
## Scopes

- **Kinds**: waveform (luma), RGB parade, YCbCr parade, vectorscope (1×, 2×, 5×, 75 %/100 % targets, skin-tone line),
  RGB + luma histogram, and statistics: luma percentiles in IRE, clipped highlights, crushed blacks, mean saturation,
  colour cast and a verdict ("underexposed", "overexposed", "low contrast", "clipped highlights", "crushed blacks",
  "ok"). IRE = Y′ × 100; nits for a 100-nit BT.1886 display.
- **What they measure**: the picture as the program monitor shows it (after the output transform), on a grid of every
  n-th pixel (automatic: at most ~600k pixels, 2-px grid at 1080p). The binning contract is in
  `src/render/color/include/wabcut/color/Scopes.h`; the GPU compute shader (`scopes.comp`) and the CPU path give the
  same counts (tested).
- **When they run**: only while the Scopes panel is visible (the Color workspace). The monitor then records a scope pass
  after its frame at most every 30 ms (~30 Hz) and reads 1.8 MB of bins back asynchronously; outside the workspace no
  compute and no readback happen at all (tested: the generation counter does not move while the monitor keeps drawing).
  Where compute shaders are missing, a small output pass plus the CPU produce the same bins.
- **Views** pool neighbouring bins to the view's device pixels (sums, not filtering) and draw brightness on a lifted log
  scale, so thin traces stay visible next to flat areas.
- **Command line**: `wabcut-cli scopes <project> --at <timecode> [--sequence <name|id>] [--clip <id>] [--out scopes.png]
  [--json stats.json] [--width <px>] [--zoom 1|2|5]`.

## Auto, Color Match and Match Scene

No machine learning; every tool returns ordinary Basic parameters you can keep editing, and fits against the grade's
CPU reference, so what it computes is what the compositor shows.

- **Auto**: white balance from a grey-world / white-patch hybrid on the most neutral fifth of the pixels (re-picked
  iteratively, so grass, sky or a red wall do not count as a cast), solved for temperature and tint by Nelder–Mead; then
  exposure, whites and blacks fitted to histogram percentiles (99.5 % → 92 %, 0.5 % to at most 4 %, the median drawn
  towards mid grey, small moves preferred). It looks at the clip without its own grade.
- **Color Match**: moves the clip's Oklab statistics (mean and spread of L, a, b) towards a reference frame's, by
  strength, fitting exposure, contrast, temperature, tint and saturation. Skin priority weighs skin-hued pixels 4× in
  both pictures. **Face detection** (0.2, `faceDetection`, Premiere's checkbox, off by default) finds the faces on the
  reference and on each clip with the local subject detector (YuNet in wabcut-embed, the host's `color::FaceFinder`) and
  weighs the faces' skin up to 12× (`faceWeights`: the ellipse in each face box, by skin likelihood, so hair and
  background in the box count less): the match brings the skin tones together first. A picture without a face is
  matched as a whole with a warning; without the detector (Settings > AI > Local models) the match is refused with the
  reason. The Color panel's "Face detection" toggle.
- **Match Scene**: matches every clip of the reference clip's scene: by default the contiguous run of clips on its track
  cut from the same media without gaps (what Split at Shots makes); also "track", "media" or an explicit list. One undo
  step; dry run available.

## Apply Grade to other clips (0.4)

The Color panel menu (the `…` button) has two rows above **Export .cube…**:

- **Apply Grade to Selected Clips**: the targeted clip's grade onto the other selected video clips (enabled when the
  targeted clip has a grade and another video clip is selected).
- **Apply Grade to All Clips of "<media>"**: onto every other video clip of the sequence that shows the same source
  (the name is cut to 32 characters). For a nested clip "the same source" is the same nested sequence; for a
  multi-camera clip the same sequence and angle.

The whole grade is copied: every parameter with its keyframes (timed from each clip's start, as Paste Attributes does),
the curves, LUT references, the section switches and the grade's on / off state. It **replaces** each target's first
grade, keeping that grade's place in the stack and its local masks (a mask is drawn for one shot); a clip without a grade
gets one added. Clips on locked tracks, audio clips and the source clip are skipped; the selection does not change. One
undo step, "Apply Grade", and the panel's status says "Grade applied to 7 clips". Premiere does this with Paste
Attributes or master clip effects, and Paste Attributes stacks a second Lumetri; Wabcut replaces the grade, so there is
never a duplicate (Paste Attributes itself is unchanged).

From the CLI, MCP and the assistant it is `color.grade.copy`: `from` (the clip whose grade is copied), `clips` (the
targets) or `scope: "media"` (every video clip of `from`'s source), `replace` (default true; false appends a second
grade like Paste Attributes) and `sequence`. The result lists each clip as replaced, added or skipped with the reason;
it is refused when `from` has no grade or no target would change. Dry run works.

## CDL interchange

The wheels are exactly an ASC CDL v1.2. `cdl.export` writes `.cc` (one clip), `.ccc` or `.cdl` (several; ids are clip
names); `cdl.import` reads them (with or without the `urn:ASC:CDL` namespace, `SatNode` or `SATNode`), matching by id =
clip name, else by position or index. Out-of-range values are clamped with a warning.

## Colour management and HDR (I.9, back in 0.2)

Colour management came back in 0.2 (it had been parked for the 0.1 gaming release; OpenColorIO is a dependency again).
Each project has a **Color Setup**, Premiere Pro's sequence colour presets
([Configure sequence Color Management](https://helpx.adobe.com/premiere/desktop/correct-color/set-up-color-management/configuring-sequence-color-management.html)).
It is set in the Color panel's **Color Management** dialog (the gear in the panel header) or with
`color.management.set`:

| Color Setup (`mode`) | Working space | The grade works in | Program monitor | Export |
|---|---|---|---|---|
| **Direct Rec.709 (SDR)** (`rec709`, the default) | Rec.709 (display-referred, linear for blends) | BT.709 encoding | Rec.709 | Rec.709 |
| **Direct HLG (HDR)** (`hdr-hlg`) | linear Rec.2020, 1.0 = the graphics white | ACEScct | tone-mapped SDR preview (labelled) | HLG (10-bit) or its SDR version |
| **Direct PQ (HDR)** (`hdr-pq`) | the same | ACEScct | tone-mapped SDR preview (labelled) | PQ (10-bit, HDR10 metadata) or its SDR version |
| **Wide Gamut (Tone Mapped)** (`aces`) | ACES 1.3: ACEScg (linear AP1) | ACEScct | ACES 1.0 SDR Video output transform | SDR: the same; PQ / HLG: the ACES 1.1 HDR Video (1000 nits) output transform |
| **Wide Gamut (No Tone Mapping)** (`wide-gamut`) | ACEScg | ACEScct | Rec.709, clipped above white | Rec.709; PQ / HLG at 1.0 = 203 nits |
| **Disable Color Management** (`off`) | Rec.709 | BT.709 encoding | code values as they are | the same |

- **Direct Rec.709 (SDR)** passes SDR media through byte for byte (the 0.1 pipeline). HDR media (PQ / HLG tags, or a
  PQ / HLG override) is tone-mapped into SDR as it is read, exactly as 0.1 did for HDR game captures (OBS, Game Bar,
  ShadowPlay): BT.2100 decoding, an ITU-R BT.2390 EETF on the Rec.2020 luminance from a 1000-nit master into the SDR
  range, the 203-nit reference white of ITU-R BT.2408 standing for SDR white, Rec.2020 → Rec.709 clipped. Camera log
  clips (an override, or log their metadata names) are tone-mapped on input the same way from their scene light
  (OpenColorIO: camera → ACES2065-1 → scene-linear Rec.2020, the scene's white at the reference white): mid-tones pass
  unchanged (S-Log3's 18 % grey stays mid grey), highlights roll off into SDR white, hue is kept - Premiere's
  "tone-mapped and gamut-compressed on input" (its default "Hue Preservation" method is Adobe's own; this is the
  BT.2390 EETF on the luminance). This is the fast default: nothing else changes for SDR projects.
- **Direct HLG / PQ (HDR)** grade in a linear Rec.2020 working space; SDR clips, titles and captions land at the
  **graphics white** (`referenceWhiteNits`, 80–400 nits, default 203 per ITU-R BT.2408; Premiere's "graphics white").
  The **HDR peak** (`peakNits`, 100–10000 nits, default 1000) is the mastering display's peak in the metadata and the
  knee of the preview.
- **Wide Gamut (Tone Mapped)** is Wabcut's ACES 1.3 pipeline: every camera's gamut and range survive into ACEScg, the
  grade works in ACEScct as colourists grade ACES, and the ACES output transforms tone-map the result. Display-referred
  pictures (Rec.709 clips, titles, captions) go through the inverse of the ACES SDR output transform, so they look as
  designed. *Deviation:* Premiere's own wide gamut working space and tone mapping algorithms (Hue Preservation, By
  Channel, Max RGB) are Adobe's; Wabcut uses ACES for the same job.
- **Wide Gamut (No Tone Mapping)**: the ACEScg working space with a straight conversion to Rec.709 (SDR media comes back
  unchanged; anything above white clips). Premiere's third wide gamut preset, "Minimal Tone Mapping", is not offered.
- **Disable Color Management**: no conversion at all - HDR media shows its code values through the BT.709 curve (washed
  out, as Premiere shows it), log stays flat (grade it by hand, e.g. with the Input LUT), overrides do nothing.

### Media color space and log auto-detection

Each video clip reads its pictures through an **input colour space** - the Color panel's **Media color space** menu at
the top of Basic Correction (Premiere's "Override Media Color Space"), `color.inputSpace.set`, the clip's
`inputColorSpace`. **Auto** (no override) follows the stream's tags: PQ / HLG transfer → Rec.2100 PQ / HLG, BT.2020
primaries → Rec.2020, sRGB and linear transfers, an untagged RGB still → sRGB, anything else → Rec.709 - and, when
**Color manage auto-detected log media** is on (the default; `autoDetectLog`, Premiere's "Color Manage Auto Detected
Log and Raw Media"), the camera log the media's metadata names. The choices: Rec.709, sRGB, Rec.2020 (SDR), Rec.2100 PQ /
HLG, **Sony S-Log3 / S-Gamut3.Cine and S-Gamut3, Panasonic V-Log / V-Gamut, ARRI LogC3 (EI 800) / AWG3, LogC4 / AWG4,
Canon Log 3 and Canon Log 2 / Cinema Gamut, RED Log3G10 / REDWideGamutRGB, Apple Log**, linear Rec.709, ACEScg, ACEScct,
ACES2065-1. Log curves are defined on full-range code values: a file the decoder reports as video range is mapped back
to its code values first.

**Log auto-detection** (`src/media/src/CameraLog.cpp`, at import: `VideoStreamInfo::logColorSpace`): log footage is
never in the colour tags FFmpeg reads, so the probe reads what cameras write instead - Sony's NonRealTimeMeta XML
(`CaptureGammaEquation` / `CaptureColorPrimaries`) in an MP4 / QuickTime file's metadata boxes (`meta`, `uuid`, `udta`
inside `moov`; never the media data; at most 4 MB) or in the clip's `<name>M01.XML` sidecar, and key / value metadata
(QuickTime `mdta` keys, Matroska tags) whose key is about the gamma, transfer or log and whose value names a log curve
(ARRI's `ColorGammaSxS = LOG-C`, ...). *Limits:* verified on synthetic files (Sony XML embedded and as a sidecar); other
vendors' metadata (Panasonic, Canon MP4, Apple Log) is recognised only where it appears as such a key / value tag -
otherwise choose the space in the menu. Camera raw formats (R3D, BRAW, ARRIRAW, ProRes RAW) are not decoded at all.

### How it runs

- **OpenColorIO 2.6** (BSD-3-Clause; vcpkg `opencolorio`, with Expat, Imath, minizip-ng (zlib only), pystring and
  yaml-cpp; listed in `THIRD-PARTY-NOTICES.txt`) provides the camera, ACES and display transforms as its **built-in
  transforms** (`Config::CreateRaw()`, no config file to ship): `SONY_SLOG3-SGAMUT3[.CINE]_to_ACES2065-1`,
  `PANASONIC_VLOG-VGAMUT_to_ACES2065-1`, `ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1`, `ARRI_LOGC4_to_ACES2065-1`,
  `CANON_CLOG3-CGAMUT_to_ACES2065-1`, `CANON_CLOG2-CGAMUT_to_ACES2065-1`, `RED_LOG3G10-RWG_to_ACES2065-1`,
  `APPLE_LOG_to_ACES2065-1`, `ACEScct_to_ACES2065-1`, the ACES 1.0 SDR / 1.1 HDR 1000-nit output transforms and the
  Rec.1886 / PQ / HLG displays. `src/render/color/src/ColorSpaces.cpp` (library `wabcut_colorspaces`, full builds
  only) plans every input and output.
- **On the GPU**: BT.709 / sRGB / BT.2020 / PQ / HLG / linear / ACEScg / ACES2065-1 inputs, the PQ / HLG outputs and
  the tone-mapped SDR preview are short analytic curves plus a 3×3 matrix in `layer.frag` / `output.frag` (HLG with its
  luminance-dependent OOTF, system gamma 1.2 + 0.42 log10(peak / 1000)), exact. The OpenColorIO transforms are **baked
  into 65³ float 3D LUTs** from OpenColorIO's own CPU processor and interpolated **tetrahedrally** (exact on the grey
  axis, 4 texel fetches): camera-log inputs are indexed by code values and store asinh of the working value (linear
  through 0 for saturated colours, logarithmic for the highlights); the ACES output transforms are indexed by a log2
  shaper of the working value (19 stops, 2⁻¹² to 128) and store linear display light in the display's primaries, after
  which the display's clamp and encoding (BT.1886, PQ, HLG) run exactly. *Deviation from the brief:* not
  `GpuShaderDesc` shader text — the compositor's shaders are precompiled for every backend (qsb) and cannot take
  generated code at run time. Measured against OpenColorIO's CPU processor (`tests/color/test_color_spaces.cpp`):
  camera-log inputs within 0.4 % of the colour's largest component (+ 1e-4); the ACES SDR output transform within
  2.4 / 1023 on greys, 13 / 1023 on colours of mild saturation (99 % within 2.3), 27 / 1023 on colours far outside the
  display gamut (99 % within 3.6); the ACES PQ / HLG outputs within 1.5 / 1023 on greys. LUTs are built once per
  transform and cached (CPU and GPU).
- **Rec.709 pictures and graphics in an ACES project** go through the inverse of the ACES SDR output transform, so they
  look as designed: greys (white and grey titles) come back within 2.6 / 1023, colours with code values 0.1–0.9 within
  16 / 1023 of OpenColorIO's own round trip (which is itself within 5 / 1023). *Limit of ACES 1.x:* the most saturated
  display colours (pure primaries, a channel at 0 next to one at 1) are never produced by its output transform, so they
  cannot come back exactly, even through OpenColorIO — a pure red title shifts. Use a display-referred project for work
  that depends on such colours.
- **Direct Rec.709's log** is the camera-log LUT to scene-linear Rec.2020, then the HDR capture tone map in
  `layer.frag` (`TF_LUT_SDR`, analytic): within 1.3 / 1023 of the CPU reference on greys and 1.03 / 1023 for 99 % of
  colours in the output's code values (the worst, 51 / 1023, on colours far outside every display gamut).
- **Out-of-gamut colours**: the layer pass clips working values below 0 (camera colours outside the working gamut, e.g.
  a saturated LED in S-Gamut3 read into ACEScg) to 0 before compositing and grading; no gamut compression (ACES 1.3's
  reference gamut compression is not applied).
- **The grade** (all its display-referred stages, the HSL qualifier, curves, wheels) works on ACEScct values in a
  managed project, as colourists grade ACES; the CPU reference follows `GradeSpec::encoding`.
- **Nested sequences** stay in the working space and in float (values above the reference white survive a nest).
- **The tone-mapped SDR preview** of an HDR project: ITU-R BT.2390 EETF on the luminance (PQ domain), from the master's
  peak into [0, reference white], colour scaled along, Rec.2020 → Rec.709 with out-of-gamut values clipped; analytic in
  `output.frag` (exact). SDR white of the preview stands for the reference white: the mastering peak lands on it, the
  mid-tones pass unchanged below the EETF's knee, and the reference white itself sits a little below SDR white (about
  88 % code at a 1000-nit peak: an SDR clip's white looks slightly grey next to brighter HDR highlights, as on an HDR
  display). The program monitor shows it with the label "HDR · tone-mapped SDR preview" (`Color.monitorLabel`), Wide
  Gamut (Tone Mapped) "ACES 1.0 SDR Video", Wide Gamut (No Tone Mapping) "Wide Gamut · no tone mapping".

### HDR export

`DynamicRange` (`VideoSettings::dynamicRange`): **Auto** (the project's own: an HDR project writes PQ or HLG when the
format has 10 bits — HEVC Main 10, ProRes 422, AV1 10-bit — and its tone-mapped SDR version otherwise), **SDR**, **PQ**,
**HLG** (PQ / HLG need an HDR or Wide Gamut colour setup and a 10-bit HEVC, ProRes or AV1 format). Presets: **YouTube HDR (PQ)**
(`youtube-hdr-pq`: HEVC Main 10, MP4), **YouTube HDR (PQ, AV1)** (`youtube-hdr-pq-av1`), **HDR HLG (HEVC 10-bit)**
(`hdr-hlg`), **ProRes 422 HQ HDR master (PQ)** (`prores-hdr-pq`).

- Colour-managed 10-bit exports render to RGB10A2 and are converted to Y'CbCr 4:4:4 10-bit on the CPU (BT.2020 NCL
  for PQ / HLG, BT.709 otherwise), then subsampled by swscale with the same tags on both sides (no gamut or transfer
  conversion). Display-referred projects keep the 8-bit path (byte-identical exports).
- Tags: `colour_primaries` BT.2020, `transfer_characteristics` SMPTE ST 2084 (PQ) or ARIB STD-B67 (HLG),
  `matrix_coefficients` BT.2020 NCL, limited range.
- **HDR10 metadata (PQ)**: the mastering display (Display P3 primaries, D65, the project's peak down to 0.0001 nits) goes
  to the encoder through `AVCodecContext::decoded_side_data` (libx265 and NVENC write the SEI, SVT-AV1 the metadata OBU)
  and to the container; **MaxCLL / MaxFALL are measured** over every rendered frame (the brightest max(R, G, B) in
  nits, and the brightest frame average of it, CTA-861.3) and written into the MP4 / MOV `clli` box with the movie header
  at the end of the export (MKV: mastering display only). `ExportResult::maxCll / maxFall` report them.
- **AV1**: `av1_nvenc` / `av1_qsv` / `av1_amf` when the GPU has an AV1 encoder (RTX 40-series, Arc, RDNA 3), else
  **SVT-AV1** (`libsvtav1`, BSD-3-Clause-Clear with the AOMedia Patent License 1.0, preset 8, CRF from the quality
  scale). MP4 or MKV.
- HEVC Main 10 needs a hardware encoder here (NVENC, AMF, Quick Sync): vcpkg's x265 is built 8-bit only. AV1 is the
  10-bit HDR path on every machine.

### Scopes in HDR

The waveform of an HDR project reads the working values as **PQ code values with a nits scale** (lines at 1, 10, 100,
203, 400, 1000, 4000, 10000 nits) or as the **HLG signal in percent** (75 % = reference white), not the tone-mapped
preview (`color::ScopeData::scale`). ACES projects' scopes read the working values through the BT.709 curve (not the
output transform).

### Project files

`colorManagement` (project: `mode`, `peakNits`, `referenceWhiteNits`, `autoDetectLog` when off), `inputColorSpace`
(clip) and `logColorSpace` (a media's video stream: the detected camera log) are written only when set, so other
projects save byte-identical apart from the schema number (17); a file that sets them needs the new schema (older
builds refuse it rather than show it unmanaged). Unknown names and out-of-range values fail to load. Merges pick
the side that changed the colour management (a conflict when both did).

### In the app

- **Color Management dialog** (the gear in the Color panel's header; `ColorManagementDialog.qml`): the Color Setup
  presets with what each does, the HDR peak and graphics white (HDR setups), and "Color manage auto-detected log
  media". OK is one undo step (`Color.setColorManagement`, `color.management.set`).
- **Media color space** (Color panel, Basic Correction): Auto (showing what Auto reads, e.g. "Auto (Sony S-Log3 /
  S-Gamut3.Cine)" for detected log) or an override (`Color.setInputColorSpace`, one undo step).
- **Monitor badge** (`Color.monitorLabel`): "HDR · tone-mapped SDR preview", "ACES 1.0 SDR Video", "Wide Gamut · no tone
  mapping" or "Color management off"; nothing for Direct Rec.709.
- **Export**: the four HDR presets in "All presets", a **Dynamic range** row (Auto / SDR / HDR PQ / HDR HLG) in the
  settings drawer whenever the sequence is HDR or Wide Gamut and the codec can carry it (HEVC, AV1, ProRes; PQ / HLG
  switch HEVC and AV1 to 10-bit), the resolved picture in the drawer's details ("HDR · Rec.2100 PQ · HDR10 metadata,
  1000 nits"), and a PQ export's measured MaxCLL / MaxFALL in the job's details. CLI:
  `wabcut-cli render ... --dynamic-range auto|sdr|pq|hlg`.
- **Scopes**: the waveform switches to the nits / HLG % graticule by itself in HDR setups.

## Commands (CLI and MCP)

`color.grade.set`, `color.grade.get`, `color.grade.copy` (0.4), `color.auto`, `color.match`, `color.matchScene`, `lut.import`, `lut.list`, `lut.export`,
`cdl.export`, `cdl.import`, `scopes.read`, and colour management (I.9): `color.management.get`, `color.management.set`,
`color.inputSpace.get`, `color.inputSpace.set` — see [commands.md](commands.md) and [mcp.md](mcp.md) (generated). Pixels for
Auto, Match and `scopes.read` come from the editor's `ColorFrameService` (the sequence frame, or one clip alone without
its grade), so the CLI, MCP and the panel see the same pictures.

## Performance (RTX 3060, D3D11, 1920×1080)

| Measurement | Result | Source |
|---|---|---|
| Whole frame, no grade (GPU) | 0.095 ms | `tests/render/test_grade.cpp` → `test-output/render/grade/perf.md` |
| Full grade: input LUT 65³, look 33³, curves, basic, creative, wheels, HSL, vignette (GPU) | 0.599 ms per layer | same |
| Full grade + HSL denoise, blur and sharpen (GPU) | 1.94 ms per layer | same |
| Scope pass, compute + 1.8 MB readback (wall clock, GPU waited for) | 1.9–2.4 ms median (gradient, flat, paced 33 ms apart) | `tests/render/test_scopes_gpu.cpp` → `scopes-perf.md` |
| Playback, three stacked 1080p60 H.264 layers, full grade (incl. HSL refine) on each, frame time CPU + GPU in series | p50 5.8–9.3 ms, **p99 8.5–13.7 ms** (four runs on a shared machine) | e2e `[color][perf]` → `test-output/e2e/color-playback.md` |
| Same + scopes at ~30 Hz | p50 6.8–9.2 ms, **p99 11.0–14.2 ms**, scope pass p50 1.6–2.4 ms | same |
| First second after a grade is added (LUTs built and uploaded once) | max 25–37 ms | same |

The grade and scope shaders are precompiled to DXBC at build time (`qt_add_shaders ... PRECOMPILE`); compiling
`grade.frag` at its first use took ~0.6 s on D3D11, a visible hitch on the first graded picture.

## Tests

- `tests/color`: .cube parsing and writing, tetrahedral vs trilinear, built-ins (identity points, S-Log3/V-Log round
  trips), grade maths per stage, CDL files, scopes binning and images, Auto and Match on synthetic casts, the commands.
- `tests/render/test_grade.cpp` (every stage GPU vs CPU reference, PSNR ≥ 45 dB min, ~59–61 dB mean; perf),
  `test_scopes_gpu.cpp` (GPU vs CPU bins, colour-bar targets, perf).
- `tests/model/test_effect_data.cpp` (data keys, bounds, assets, round trip), `tests/editor/test_color_editor.cpp`
  (LUT assets relink/collect, colour frames), `tests/export/test_rendered.cpp` (export == preview with a full grade,
  min 58 dB).
- Colour management (I.9, 0.2): `tests/color/test_color_spaces.cpp` (target `wabcut_colorspaces_tests`: every OpenColorIO
  built-in used exists; ACEScct against OpenColorIO; PQ (ST 2084) and HLG (BT.2100) reference values and round trips;
  S-Log3, V-Log, LogC3, LogC4, Canon Log 3 and Apple Log neutral ramps against the makers' published curves; input and
  output plans against OpenColorIO's CPU processor; the ACES round trip of Rec.709 pictures; the default plan; the tone
  map; Auto from stream tags), `tests/model/test_color_management.cpp` (names and bounds, edits, files written only when
  set, an old file loads unchanged, merges), `tests/color/test_color_commands.cpp` (the four commands),
  `tests/render/test_color_management.cpp` (GPU vs CPU references per mode: the default byte for byte, ACES SDR, PQ / HLG
  outputs and the tone-mapped preview, the grade in ACEScct, float above white through a nest, HDR scopes),
  `tests/export/test_hdr_export.cpp` (ffprobe: BT.2020 / PQ / HLG tags, the mastering display, MaxCLL / MaxFALL; the
  format rules; export == monitor per mode), `tests/app/test_scope_view_hdr.cpp` (the nits and HLG % graticules).
- 0.2: `tests/model/test_color_management.cpp` (Premiere's colour setups, managed working spaces, the detected log in
  Auto, `autoDetectLog` and `logColorSpace` in project files), `tests/media/test_camera_log.cpp` (log names, Sony's
  XML embedded in MP4 metadata boxes or as the M01.XML sidecar, key / value tags, bounded reads, the probe),
  `tests/color/test_color_spaces.cpp` (Direct Rec.709's tone-mapped log and HDR, Wide Gamut without tone mapping),
  `tests/render/test_color_management.cpp` (Direct Rec.709's log on the GPU, Disable Color Management, Wide Gamut),
  `tests/color/test_color_commands.cpp` (the six setups, `autoDetectLog`, the "off" warning),
  `tests/export/ui/test_export_ui.cpp` (the dynamic range override) and `tests/app/test_color_management_ui.cpp` (the
  dialog in the shell, one undo step, the monitor badge).
- e2e `[color]`: the workspace (grade, scopes update and draw traces, stop outside the workspace, before/after,
  reference, Auto, Match, a LUT asset imported, moved, listed offline and relinked; screenshots `color_workspace.png`, `color_scopes.png`, `color_panel.png`,
  `color_graded_vs_ungraded.png`, `color_before_after.png`, `color_reference.png`) and the playback measurement above.
