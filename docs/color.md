# Colour grading: the Color workspace, the grade, LUTs and scopes (I.5)

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
  both pictures; per-pixel weights are the seam for a face detector later.
- **Match Scene**: matches every clip of the reference clip's scene: by default the contiguous run of clips on its track
  cut from the same media without gaps (what Split at Shots makes); also "track", "media" or an explicit list. One undo
  step; dry run available.

## CDL interchange

The wheels are exactly an ASC CDL v1.2. `cdl.export` writes `.cc` (one clip), `.ccc` or `.cdl` (several; ids are clip
names); `cdl.import` reads them (with or without the `urn:ASC:CDL` namespace, `SatNode` or `SATNode`), matching by id =
clip name, else by position or index. Out-of-range values are clamped with a warning.

## HDR game captures (I.9, gaming)

Wabcut projects are SDR Rec.709. OBS, Xbox Game Bar and ShadowPlay record HDR games as BT.2020 files with the PQ
(SMPTE ST 2084) or HLG transfer; read through the BT.709 curve they would look washed out with clipped highlights. The
layer pass tone-maps such a clip into SDR as it reads it, automatically from the stream's tags (no setting):

- PQ is decoded to nits, HLG through BT.2100's OOTF for a 1000-nit display.
- ITU-R BT.2390 EETF on the Rec.2020 luminance, in the PQ domain, from a **1000-nit master** into the SDR range,
  where the **203-nit reference white** (ITU-R BT.2408) stands for SDR white: mid-tones pass unchanged below the knee,
  the master's peak lands on SDR white (brighter highlights are held there), the reference white itself sits a
  little below it (about 88 % code); the colour is scaled with its luminance.
- Rec.2020 → Rec.709, out-of-gamut values clipped.

The monitor, exports, review frames and thumbnails all go through the compositor, so they show the same pictures.
CPU reference: `color/Hdr.h` (`hdrCaptureToSdr`, also in `render::referencePixel`); shader: `layer.frag`
`captureToSdr`. No colour management and no OpenColorIO: the ACES / camera-log colour management of I.9 was dropped in
the gaming re-scope (it stays on the branch `i9-colour-archive`).

## Commands (CLI and MCP)

`color.grade.set`, `color.grade.get`, `color.auto`, `color.match`, `color.matchScene`, `lut.import`, `lut.list`,
`cdl.export`, `cdl.import`, `scopes.read` — see [commands.md](commands.md) and [mcp.md](mcp.md) (generated). Pixels for
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
- HDR captures: `tests/color/test_hdr.cpp` (PQ and HLG reference points and round trips, the tone map, the captures),
  `tests/render/test_hdr_capture.cpp` (PQ and HLG frames on the GPU against the CPU reference, 2.5 / 255).
- `tests/render/test_grade.cpp` (every stage GPU vs CPU reference, PSNR ≥ 45 dB min, ~59–61 dB mean; perf),
  `test_scopes_gpu.cpp` (GPU vs CPU bins, colour-bar targets, perf).
- `tests/model/test_effect_data.cpp` (data keys, bounds, assets, round trip), `tests/editor/test_color_editor.cpp`
  (LUT assets relink/collect, colour frames), `tests/export/test_rendered.cpp` (export == preview with a full grade,
  min 58 dB).
- e2e `[color]`: the workspace (grade, scopes update and draw traces, stop outside the workspace, before/after,
  reference, Auto, Match, a LUT asset imported, moved, listed offline and relinked; screenshots `color_workspace.png`, `color_scopes.png`, `color_panel.png`,
  `color_graded_vs_ungraded.png`, `color_before_after.png`, `color_reference.png`) and the playback measurement above.
