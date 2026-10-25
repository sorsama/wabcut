# Effects and transitions library

Wabcut carries Premiere Pro's video transitions and its most used video effects (0.2) under Premiere's names, bins,
parameters and defaults. They are ordinary registry effects (`src/model/src/EffectsLibrary.cpp`): `effect.list` lists
them, `clip.addEffect` / `transition.add` apply them, Effect Controls shows their parameters (menus for Premiere's popups)
and the Effects panel groups them in Premiere's bins. The compositor draws them on the GPU for preview and export alike;
each has a CPU twin in `src/render/reference` that the GPU tests compare against.

Premiere 26 moved most classic transitions to its Legacy and Obsolete folders and replaced some with Film Impact versions;
Wabcut keeps the classic bins (Dissolve, Iris, Page Peel, Slide, Wipe, Zoom) under which editors know them, and puts its
creator transitions in Premiere 26's Lights & Blurs and Grunge & Distort bins.

## How it is built

- **Registry.** `EffectsLibrary.cpp` holds the descriptors: stable ids, Premiere's display names and bins (`category`),
  parameters with units, ranges and defaults. A Premiere popup is a parameter with `EffectParamInfo::options`
  (`"Off|Low|Medium|High"`): its values are the whole numbers from its minimum, validated as such; Effect Controls shows
  it as a choice (pills for up to four options, a menu above).
- **Transitions** map in the evaluator (`playback::transitionMixAt`) to a `render::LayerTransition`: the kind and the
  options in the compositor's units (user colours to linear light, Start/End applied to the progress, the transition's id
  as the seed of random patterns). They run in one pass of `shaders/fx_transition.frag`; the CPU twin is
  `render/reference/Transitions.h`, whose header gives every formula.
- **Video effects** map through `render/EffectLibrary.h`: each registry id has a `LibraryOp` and the order of its
  parameters in `LayerEffect::params` (the evaluator fills them; `params[31]` is the curve time for the effects that
  animate by themselves). The compositor runs the op in its family shader (`fx_color.frag`, `fx_distort.frag`,
  `fx_stylize.frag`, `fx_generate.frag`) - one pass for most, a few passes for the ones that blur or key first. The CPU
  twins are `render/reference/LibraryEffects.h` (`LibraryDistort.cpp`, `LibraryStylize.cpp`, `LibraryGenerate.cpp`).
- **Second pictures.** A few effects read a picture besides their layer, `LayerEffect::matte`, a composite on the
  layer's canvas that the evaluator prepares (`playback/src/video/Evaluate.cpp`): Echo's echoes (the clip's other frames,
  each from its own decoder), the text of Timecode / Clip Name / Simple Text (drawn by the caption renderer), Gradient
  Wipe's Gradient Layer and Difference Matte's Difference Layer (another video track's clip at this time, read like a
  Track Matte: hidden groups flagged `matte` with the effect's index), Image Matte Key's still image.
- **Time.** Posterize Time is not a pass: the evaluator holds the clip's time on steps of 1 / Frame Rate from its start,
  so the picture and every effect of the clip step together. Strobe Light's timing and the Transform effect's shutter
  (the transform Shutter Angle / 360 of a frame later, for motion blur) are evaluated from the curve time and the
  sequence's frame rate (`playback::EffectContext`).
- **Tests.** `tests/render/test_transitions.cpp` and `test_library_effects.cpp` render every transition kind and every
  effect with non-default settings at 320 x 180 and compare them with the CPU twin (PSNR >= 40 dB and at most 0.3 % of
  pixels more than 6/255 off; black-and-white results, whose threshold pixels may flip between the GPU's floats and the
  reference's doubles, are judged by the second figure); one test case per family keeps the suite fast.
  `tests/playback/video/test_library_mapping.cpp` checks the evaluator: every registered video transition is
  implemented, options and Start/End reach the compositor, Posterize Time holds the clip, Echo's groups and weights,
  the text pictures, the other-track groups and the Transform shutter, and how the Animations move their clip (Fold's
  hinge edge stays put).

## Video transitions

Common options (Premiere's Effect Controls for transitions): **Start** and **End** (where the transition begins and ends,
0-100 %), **Border Width** (sequence pixels) and **Border Color** (a band on the outgoing clip's side of the edge),
**Reverse** (the pattern plays backwards; Clock Wipe turns counter-clockwise), **Anti-aliasing Quality** (Off, Low,
Medium, High: the edge's softness - Off still has one pixel of coverage, Wabcut never draws aliased edges), **Center**
(irises, Pinwheel, Cross Zoom, Zoom Blur) and **Direction** (Premiere's edge selectors). Dissolves have Start and End only.

| Bin | Transition | Id | Options beyond the common ones |
|---|---|---|---|
| Dissolve | Additive Dissolve | `additiveDissolve` | - (B rises over the first half, A falls over the second: the middle brightens) |
| Dissolve | Cross Dissolve | `crossDissolve` | - (linear light, as the compositor blends) |
| Dissolve | Dip to Black, Dip to White | `dipToBlack`, `dipToWhite` | - |
| Dissolve | Dip to Color | `dipToColor` | Color |
| Dissolve | Film Dissolve | `filmDissolve` | - (a dissolve in linear light, gamma 1.0: the same as Wabcut's Cross Dissolve) |
| Dissolve | Non-Additive Dissolve | `nonAdditiveDissolve` | - (each channel the brighter of the two clips at their levels) |
| Iris | Iris Box, Iris Cross, Iris Diamond, Iris Round | `irisBox`, `irisCross`, `irisDiamond`, `irisRound` | Center |
| Page Peel | Page Peel | `pagePeel` | corner; the back is shaded paper, the clip below is shadowed by the curl |
| Page Peel | Page Turn | `pageTurn` | corner; the back shows the picture mirrored |
| Slide | Band Slide | `bandSlide` | Horizontal / Vertical, Number of Bands (7) |
| Slide | Center Split | `centerSplit` | - |
| Slide | Push | `push` | four directions (Reverse pushes the other way) |
| Slide | Slide | `slide` | eight directions (Reverse: the outgoing clip slides out) |
| Slide | Split | `split` | Horizontal / Vertical |
| Slide | Whip | `whip` | four directions, Blur (an eased push with motion blur) |
| Wipe | Band Wipe | `bandWipe` | Horizontal / Vertical, Number of Bands (7) |
| Wipe | Barn Doors | `barnDoors` | Horizontal / Vertical |
| Wipe | Checker Wipe, CheckerBoard | `checkerWipe`, `checkerBoard` | four directions, Horizontal / Vertical Slices (8 x 8) |
| Wipe | Clock Wipe | `clockWipe` | where the hand starts (From North ...) |
| Wipe | Gradient Wipe | `gradientWipe` | Gradient (built-in ramps: linear left to right or top to bottom, radial, diagonal), Softness (10) |
| Wipe | Inset | `inset` | corner |
| Wipe | Paint Splatter | `paintSplatter` | - |
| Wipe | Pinwheel | `pinwheel` | Number of Wedges (8), Center |
| Wipe | Radial Wipe | `radialWipe` | corner |
| Wipe | Random Blocks | `randomBlocks` | Wide x High (20 x 20) |
| Wipe | Random Wipe | `randomWipe` | four directions (top to bottom) |
| Wipe | Spiral Boxes | `spiralBoxes` | Horizontal x Vertical (6 x 4) |
| Wipe | Venetian Blinds | `venetianBlinds` | four directions, Number of Bands (6) |
| Wipe | Wedge Wipe | `wedgeWipe` | where the wedge opens (From North ...) |
| Wipe | Wipe | `wipe` | Angle, Border Feather |
| Wipe | Zig-Zag Blocks | `zigZagBlocks` | four directions, Horizontal x Vertical (8 x 6) |
| Zoom | Cross Zoom | `crossZoom` | Center |
| Lights & Blurs | Zoom Blur | `zoomBlur` | In / Out, Blur, Center |
| Lights & Blurs | Light Leak | `lightLeak` | Color, Intensity |
| Lights & Blurs | Flash | `flash` | Color, Intensity |
| Grunge & Distort | Glitch | `glitch` | Intensity, Color Split |
| Animations | Block | `animationBlock` | Block Size (64 px) |
| Animations | Fade | `animationFade` | - |
| Animations | Flip | `animationFlip` | Axis: Horizontal (about the vertical axis) / Vertical |
| Animations | Fold | `animationFold` | Hinge: Top / Bottom / Left / Right |
| Animations | Pop, Pull, Spring | `animationPop`, `animationPull`, `animationSpring` | - |
| Animations | Spin | `animationSpin` | Turn: Clockwise / Counterclockwise |
| Animations | Travel | `animationTravel` | Side: Left / Right / Top / Bottom |
| Animations | Surprise Me | `animationSurprise` | - (becomes one of the above, with a random option, when applied) |

**Animations** (0.2, Premiere 25.5's motion animations; `model/Animations.h`) are transitions that move the clip itself
instead of mixing two pictures, so they go where any transition goes: dropped on a clip's In point the clip animates in,
on its Out point it animates out (the In animation backwards; Spin and Flip keep turning the same way), and on an edit
point the incoming clip animates in over the outgoing one (which plays on beneath it, into its handles). The duration
handle, Effect Controls (Start, End, the option), Delete and undo work as for every transition. The evaluator
(`playback::transitionAnimationAt`) changes the animated layer's Motion and Opacity - Fade: opacity; Pop: scale from 0
past 110 % to 100 % with a quick fade; Pull: from 160 % while fading in; Spin: half a turn while growing from nothing;
Travel: from a frame's width or height off that side; Spring: up from below the frame with a damped bounce - and appends
library effects for the rest: Flip is Basic 3D's Swivel or Tilt from 90 degrees (edge-on) to 0, Fold is Basic 3D tilted
or swivelled about the hinge edge plus a Transform that keeps that edge's middle where it was (the same projection as
the shader), Block is Block Dissolve's completion from 100 % to 0. The motion is eased out (cubic) like the Animations
presets; the CPU and GPU paths draw the same because the layer, not a transition shader, moves. **Surprise Me** is
resolved when it is applied: a drop from the Effects panel (or choosing it in Effect Controls) picks at random each
time; `transition.add` and the default-transition commands pick by the edit point, so a script is repeatable. A Surprise
Me left in a project (an older build) animates as its id picks.

Not implemented: Morph Cut (another workstream), Gradient Wipe with an image of the user's (only the built-in ramps), the
Immersive Video (VR) transitions, and Premiere 26's Film Impact library beyond the creator transitions above.

**Cost.** Every transition is one pass over the canvas; the heaviest (Whip's 24 taps, the zooms' 2 x 16, Paint Splatter's
28 splats) take about 6 ms per 1080p frame on the test machine's RTX 3060 including upload and readback
(`[transitions][perf]`).

## Audio transitions

| Transition | Id | Gains (p = progress) |
|---|---|---|
| Constant Gain | `constantGain` | out 1 - p, in p |
| Constant Power | `constantPower` | out cos(p pi / 2), in sin(p pi / 2) (the default) |
| Exponential Fade | `exponentialFade` | out log10(1 + 9 (1 - p)), in log10(1 + 9 p): Premiere's "smooth logarithmic curve", like Constant Power but more gradual (both sides at 0.74, -2.6 dB, in the middle) |

## Video effects

The Color family works on the picture's straight colour encoded with the working space's BT.709 curve (as Premiere's 8-bit
effects see it), clamped to 0..1, and writes back linear light; user colours are display-referred values on that scale.
`render/reference/LibraryEffects.h` has each formula.

| Bin | Effect | Id | Notes |
|---|---|---|---|
| Adjust | Convolution Kernel | `wabcut.adjust.convolutionKernel` | M11..M33 (M11 top left), Offset (0..255 levels), Scale (the divisor), Process Alpha: out = sum of M x the encoded colour of the 3 x 3 neighbours (one canvas pixel apart) / Scale + Offset, clamped (0.2) |
| Adjust | Extract | `wabcut.adjust.extract` | black and white from the luma range, Softness, Invert |
| Adjust | Levels | `wabcut.adjust.levels` | (RGB), (R), (G), (B) input/output black and white and gamma (100 = 1.0); the channel's levels, then the RGB ones |
| Adjust | ProcAmp | `wabcut.adjust.procAmp` | brightness and contrast of luma, hue and saturation of chroma; Split Screen leaves the right part unprocessed |
| Channel | Invert | `wabcut.channel.invert` | RGB or a channel, HLS or a component, YIQ or a component, Alpha; Blend With Original |
| Color Correction | Brightness & Contrast | `wabcut.color.brightnessContrast` | |
| Color Correction | Change Color | `wabcut.color.changeColor` | matched in RGB, by hue or chroma; View shows the mask |
| Color Correction | Change to Color | `wabcut.color.changeToColor` | Setting To Color or Transforming To Color; hue, lightness, saturation tolerances |
| Color Correction | Color Balance | `wabcut.color.balance` | shadow / midtone / highlight balance, Preserve Luminosity |
| Color Correction | Leave Color | `wabcut.color.leaveColor` | |
| Color Correction | Tint | `wabcut.color.tint` | |
| Color Correction | Video Limiter | `wabcut.color.videoLimiter` | Clip Level 100-109 IRE, Compression Before Clipping, Gamut Warning |
| Image Control | Black & White | `wabcut.image.blackWhite` | |
| Image Control | Color Pass | `wabcut.image.colorPass` | |
| Image Control | Color Replace | `wabcut.image.colorReplace` | keeps grey levels unless Solid Colors |
| Image Control | Gamma Correction | `wabcut.image.gammaCorrection` | out = in^(Gamma / 10) |
| Keying | Alpha Adjust | `wabcut.key.alphaAdjust` | |
| Keying | Luma Key | `wabcut.key.luma` | Threshold and Cutoff (Cutoff above Threshold keys the lights) |
| Noise & Grain | Noise | `wabcut.noise.noise` | new grain every frame (from the curve time) |
| Stylize | Posterize | `wabcut.stylize.posterize` | |
| Stylize | Solarize | `wabcut.stylize.solarize` | |
| Stylize | Strobe Light | `wabcut.stylize.strobeLight` | the evaluator decides when it strobes (Duration every Period, Random Strobe Probability per duration-long slot) |
| Stylize | Threshold | `wabcut.stylize.threshold` | |

Basic Correction, Vignette and Color Grade moved to the Color Correction bin with them (Premiere's bin for Lumetri and
Vignette).

The other families sample the layer elsewhere (Distort, Perspective, blurs), look at neighbours (Stylize, Noise & Grain)
or draw over it (Generate, the Transition bin, Video). Positions are fractions of the frame (0.5, 0.5 = the centre),
sizes in sequence pixels, as Premiere's Effect Controls shows them for a frame-size clip.

| Bin | Effect | Id | Notes |
|---|---|---|---|
| Blur & Sharpen | Camera Blur | `wabcut.blur.camera` | a disc (lens) blur of 64 taps, Percent Blur 100 = a 40-pixel radius |
| Blur & Sharpen | Directional Blur | `wabcut.blur.directional` | Direction, Blur Length (sequence pixels) |
| Blur & Sharpen | Unsharp Mask | `wabcut.blur.unsharpMask` | Amount, Radius, Threshold; two passes (blur, then the difference) |
| Distort | Corner Pin | `wabcut.distort.cornerPin` | the four corners (a projective map) |
| Distort | Lens Distortion | `wabcut.distort.lensDistortion` | Curvature, decentering, prisms, Fill Alpha / Fill Color |
| Distort | Magnify | `wabcut.distort.magnify` | Circle or Square, Magnification, Link, Size, Feather, Opacity, Scaling, blending mode |
| Distort | Mirror | `wabcut.distort.mirror` | Reflection Center and Angle |
| Distort | Offset | `wabcut.distort.offset` | Shift Center To, Blend With Original (wraps) |
| Distort | Spherize | `wabcut.distort.spherize` | Radius, Center of Sphere |
| Distort | Transform | `wabcut.distort.transform` | anchor, position, uniform or separate scale, skew, rotation, opacity; with Use Composition's Shutter Angle off, Shutter Angle > 0 blurs the motion over the shutter (16 taps between now and Shutter Angle / 360 of a frame later) |
| Distort | Turbulent Displace | `wabcut.distort.turbulentDisplace` | Turbulent / Bulge / Twist / Horizontal / Vertical displacement, Amount, Size, Complexity, Evolution, Pinning |
| Distort | Twirl | `wabcut.distort.twirl` | Angle, Twirl Radius, Twirl Center |
| Distort | Wave Warp | `wabcut.distort.waveWarp` | Sine / Square / Triangle / Sawtooth / Circle / Semicircle / Uncircle / Noise / Smooth Noise, Height, Width, Direction, Speed (from the curve time), Pinning, Phase |
| Transform | Edge Feather | `wabcut.transform.edgeFeather` | |
| Transform | Horizontal Flip, Vertical Flip | `wabcut.transform.horizontalFlip`, `wabcut.transform.verticalFlip` | |
| Perspective | Basic 3D | `wabcut.perspective.basic3d` | Swivel, Tilt, Distance to Image, Specular Highlight (the back shows the picture mirrored) |
| Perspective | Bevel Alpha | `wabcut.perspective.bevelAlpha` | the edge of the alpha lit from Light Angle; two passes |
| Perspective | Bevel Edges | `wabcut.perspective.bevelEdges` | the frame's rectangle bevelled |
| Perspective | Radial Shadow | `wabcut.perspective.radialShadow` | a point light's shadow, Regular or Glass Edge, Shadow Only, Resize Layer ignored; two passes and a blur |
| Stylize | Alpha Glow | `wabcut.stylize.alphaGlow` | Glow, Brightness, Start / End Color, Fade Out; two passes |
| Stylize | Color Emboss, Emboss | `wabcut.stylize.colorEmboss`, `wabcut.stylize.emboss` | Direction, Relief, Contrast, Blend With Original |
| Stylize | Find Edges | `wabcut.stylize.findEdges` | Invert, Blend With Original |
| Stylize | Replicate | `wabcut.stylize.replicate` | Count (2 to 16 a side) |
| Stylize | Roughen Edges | `wabcut.stylize.roughenEdges` | Roughen, Roughen Color, Cut, Spiky, Rusty, Rusty Color, Photocopy, Photocopy Color (the Rusty and Photocopy kinds approximated), Border, Edge Sharpness, Fractal Influence, Scale, Stretch, Offset, Complexity, Evolution; two passes |
| Noise & Grain | Dust & Scratches | `wabcut.noise.dustScratches` | a median where it differs from the picture by more than Threshold |
| Noise & Grain | Median | `wabcut.noise.median` | the median (by luma) of 9 samples spread over Radius - an approximation of the full window, Operate On Alpha Channel |
| Keying | Color Key | `wabcut.key.colorKey` | Key Color, Color Tolerance, Edge Thin, Edge Feather; three passes (matte, thin, feather) |
| Keying | Difference Matte | `wabcut.key.differenceMatte` | Difference Layer: a video track's clip at this time (a track number, none = nothing keyed); Final Output / Source Only / Matte Only, Matching Tolerance and Softness |
| Keying | Image Matte Key | `wabcut.key.imageMatte` | the matte is a still image item of the project, set as the effect's data `image` = `media:<id>` (`clip.setEffect` with `data`); Matte Alpha or Matte Luma, Reverse |
| Generate | 4-Color Gradient | `wabcut.generate.fourColorGradient` | four points and colours, Blend, Jitter, Opacity, blending mode |
| Generate | Checkerboard, Grid | `wabcut.generate.checkerboard`, `wabcut.generate.grid` | Anchor, Size From (Corner Point / Width / Width & Height sliders), Feather, Color, Opacity, blending mode (Grid: Border, Invert) |
| Generate | Circle | `wabcut.generate.circle` | Radius, Edge (none, thickness, ...), feathers, Invert, Color, Opacity, blending mode |
| Generate | Lens Flare | `wabcut.generate.lensFlare` | Flare Center, Brightness, 50-300 mm Zoom / 35 mm Prime / 105 mm Prime, Blend With Original |
| Generate | Ramp | `wabcut.generate.ramp` | Linear or Radial, Start / End and their colours, Ramp Scatter, Blend With Original |
| Time | Echo | `wabcut.time.echo` | Echo Time (seconds; negative = earlier frames), Number of Echoes (up to 8 here), Starting Intensity, Decay, Echo Operator (Add, Maximum, Minimum, Screen, Composite In Back / In Front, Blend) |
| Time | Posterize Time | `wabcut.time.posterizeTime` | Frame Rate: the clip held on its own rate (no pass) |
| Transition | Block Dissolve | `wabcut.transition.blockDissolve` | Transition Completion, Block Width / Height (Feather and Soft Edges not drawn) |
| Transition | Gradient Wipe | `wabcut.transition.gradientWipe` | Gradient Layer: a video track's clip at this time, or the clip's own luminance; Softness, Invert Gradient |
| Transition | Linear Wipe, Radial Wipe, Venetian Blinds | `wabcut.transition.linearWipe`, `wabcut.transition.radialWipe`, `wabcut.transition.venetianBlinds` | Transition Completion and the wipe's angle / centre / width, Feather |
| Video | Clip Name | `wabcut.video.clipName` | Sequence Clip Name, Project Clip Name or File Name on a box |
| Video | Simple Text | `wabcut.video.simpleText` | the text is the effect's data `text` (Effect Controls' text row, `clip.setEffect` with `data`; default "Simple Text"), Alignment, Size, Opacity, Background Opacity |
| Video | Timecode | `wabcut.video.timecode` | SMPTE, frames or feet + frames (16 mm, 35 mm), Clip / Media / Generate source, Time Display rate, Offset, Starting Timecode, Label Text (CAM1-CAM9) on a box |

Not implemented or partial: Echo's echoes leave out the clip's effects (Premiere echoes what the effects above Echo make
of it); Posterize Time affects the whole clip (in Premiere, the effects before it); Gradient Wipe's Gradient Placement
always stretches the layer to the frame; Difference Matte's If Layer Sizes Differ and Blur Before Difference, Magnify's
Scaling, Wave Warp's antialiasing and the Transform effect's Sampling are accepted and stored but do not change the
picture; Timecode's Field Symbol is not drawn; Image Matte Key has no file picker in the panel (the command's data sets
it).

**Cost.** Most effects are one pass over the layer. The two- and three-pass ones are Unsharp Mask, Alpha Glow, Bevel
Alpha, Roughen Edges, Radial Shadow (a blur in between) and Color Key (matte, thin, feather). Echo adds one decoded
picture per echo; the text effects add one caption raster per distinct text (kept while it shows). The heaviest single
passes are Camera Blur, Median, Turbulent Displace and the Transform effect's motion blur; `[library][perf]` in
`test_library_effects.cpp` reports their 1080p times.

## The Effects panel

The Effects panel (Shift+7) is Premiere's folder tree. `panels/EffectsPanel.qml` draws the rows of the `EffectsTree`
model (`EffectsTreeModel.cpp`: the visible rows, the order, the nested presets bins, the open folders, the search and
the filter), so the list is virtualized and keeps its selection when the library changes.

- **Folders**, in Premiere's order: **Favorites** (when it has items), **Presets**, **Lumetri Presets**, **Audio
  Effects**, **Audio Transitions**, **Video Effects**, **Video Transitions**, **Audio Plug-ins** (the CLAP and VST3
  plug-ins this computer has, by vendor or by category), then each **custom bin**. Inside a folder its sub-folders come
  first: the effect categories (one folder each: "Noise Reduction/Restoration" is one name) and the presets bins, nested
  by their path ("PiPs" > "25%"). A folder shows how many items it holds (its matches while searching). Click a folder,
  or press Left / Right, to open or close it. Wabcut remembers the open folders; at the next start the top-level folders
  are closed except Favorites and the one opened last.
- **Toolbar:** the search (fuzzy over names and categories, plug-ins also by vendor) opens every folder with a match and
  says how many there are; Esc clears it and the folders come back as they were. Then the view switch, the filter menu
  (All, Video, Audio, Transitions, Presets, Favorites, Audio Plug-ins; not kept between runs), New Custom Bin and the
  panel menu (…): New Custom Bin, New Presets Bin, Import Presets…, Export Presets… (all of yours; off while you have
  none), Expand All, Collapse All, Show Audio Plug-ins and Audio Plug-in Settings….
- **Views** (kept in the settings, `effects/view`): **List** (icon and name; the category while searching and on copies
  in Favorites or a custom bin; the description as a tooltip), **Compact** (denser rows) and **Icons** (tiles under each
  folder: the category icon, the name, a bookmark on favourites; the tooltip shows the whole name, then the
  description). A name cut short in a narrow panel shows whole in its tooltip in every view.
- **Applying:** double-click an effect, transition, preset or plug-in, or press Enter, to put it on every selected clip
  in one undo step. A selected clip of the other kind stands for its linked partner, and a transition goes on both ends
  of each selected clip at the default length. Or drag it onto a clip or an edit point in the timeline, onto Effect
  Controls or onto a mixer strip. The status line under the list says what happened and clears after a few seconds.
- **Item menu** (right-click): Apply to Selected Clips, **Set as Default Transition** (transitions; DEFAULT marks the
  current video and audio defaults, which Ctrl+D, Ctrl+Shift+D and Shift+D use), Add to / Remove from Favorites, Add to
  Custom Bin ▸ (or a new one), Remove from *bin* (a copy in a custom bin), Preset Properties…, Export Preset… and
  Delete… (your presets; on a copy in a custom bin, Remove from *bin* takes its place).
- **Folder menu:** on Presets and your presets bins, New Presets Bin, **Import Presets…** (into that bin) and **Export
  Presets…** (that bin's presets and the bins inside it, as "*bin*.wabcut-presets.json"); on custom bins and your
  presets bins, Rename and Delete…; on Audio Plug-ins, Group by Vendor / Category, Rescan and the plug-in settings; on
  every folder, Expand All and Collapse All. The file dialogs open in the folder used last and add the
  `.wabcut-presets.json` suffix to a typed name.
- **Bins:** New Custom Bin (Ctrl+/, the toolbar's folder button, or an item's Add to Custom Bin > New Custom Bin) and New
  Presets Bin make the bin, show it and put its name in an edit field. F2 renames a custom bin or one of your presets
  bins in place (Enter keeps the name, Esc goes back). Drag an effect onto a custom bin to add it, drag one of your
  presets onto another presets bin to move it, and drop a `.wabcut-presets.json` file from Explorer on a presets bin to
  import it there (elsewhere in the list each preset goes back to the bin it was exported from).
- **Deleting asks first.** Delete (and Backspace, the keymap's Delete Custom Item) on one of your presets, an item in a
  custom bin, a custom bin or one of your presets bins opens a confirmation. Built-in items and folders cannot be
  deleted; the status line says so. In the panel Delete belongs to the panel, wherever its keyboard focus is (the
  toolbar's buttons too): it never reaches Edit > Clear, which would delete the selected timeline clips. After a delete
  the selection goes to the row's folder (in the Icons view, to the tile now in its place).
- **Keyboard:** Tab reaches the search, the toolbar buttons and the list. In the list, Up / Down move through the rows,
  Home / End and Page Up / Page Down jump, Left / Right close or open a folder or go to its parent (in the Icons view they
  also move between tiles), Enter applies or opens, F2 renames, Delete deletes and Menu or Shift+F10 opens the row's
  menu. Down in the search goes to the first match; Up on the first row goes back to the search. A rename in place left
  with a click elsewhere keeps the new name and leaves the keyboard where you clicked. Screen readers follow the
  selected row: it is the accessible focus while the list has the keyboard.
- **Problems:** a library file that cannot be read shows as a banner above the list. A plug-in that cannot be used is
  dimmed, cannot be dragged and says why in its tooltip. The Audio Plug-ins folder shows
  its tools while it is open: the grouping, Rescan, the settings, the scan's progress and how many plug-ins were blocked
  because they crashed or hung while scanned.

## Presets, custom bins and favourites

Premiere's effect presets (Adobe's help, "Effect presets": Save Preset, the three preset types, the Presets bin,
Import / Export Presets) are in `EffectPresets.h`; the commands are `effects.*` ([commands.md](commands.md)); the Effects
panel and Effect Controls use them through the `EffectPresets` bridge.

- **Save Preset.** Right-click an effect's header in Effect Controls > **Save Preset…** (or **Save All Effects as
  Preset…**: every effect of the clip, Motion and Opacity included). The dialog takes Premiere's fields: a **Name**, the
  **Type** and a **Description**, plus the presets bin it goes in. A preset keeps the stack effects with their
  parameters, keyframes, data and masks, and the fixed effects (Motion, Opacity with its blend mode, Volume, Panner).
  `effects.savePreset` does the same from the CLI, MCP or the assistant.
- **Preset types** time the keyframes on the clip the preset is applied to. A preset stores its keys as timeline time from
  the In point of the clip it was saved from, and that clip's length:
  - *Scale*: the keys keep their place in proportion to the clip's length (a key 75 % in stays 75 % in);
  - *Anchor to In Point*: they keep their distance from the In point (no scaling);
  - *Anchor to Out Point*: they keep their distance from the Out point.
  Applying maps them into the clip's curve time at its speed and direction (a 200 % clip gets its keys at twice the
  source distance), so the effect animates where it did, whatever the clip's speed.
- **Applying.** Double-click a preset (or press Enter) for the selected clips, or drag it onto a clip in the timeline or
  onto Effect Controls; a video preset dropped on the audio half of a linked clip goes to its video half. Fixed effects
  replace the clip's values (their keyframes too, as in Premiere); stack effects are added after the clip's own. One undo
  step ("Apply Preset"); `effects.applyPreset`. 0.2: a preset anchored to the In or Out point whose fixed-effect values
  are keyed replaces only the clip's keys inside the preset's own keyed time, so an In and an Out animation (the
  Animations bin) both stay on one clip; Scale presets still replace the whole curve.
- **Presets bin.** The Effects panel's **Presets** folder holds Wabcut's built-in bins, then the user's presets bins
  (nested by path) and presets; **Lumetri Presets** the built-in looks ([The Effects panel](#the-effects-panel) for the
  menus, Import / Export Presets of one bin and moving a preset by dragging it onto another bin).
- **Custom bins** (Premiere's New Custom Bin, Ctrl+/ in the Effects panel; "Custom Bin 01", "Custom Bin 02", ...) hold
  shortcuts to effects, transitions, presets and audio plug-ins; **Delete Custom Item** (Backspace in the panel) takes the
  current item out of its custom bin or deletes the current user preset, after asking. **Favorites** is Wabcut's: any
  effect, transition, preset or plug-in can be added.
- **Where they live.** The user's, not the project's (Premiere keeps presets with the user's profile): one JSON file,
  `effects-library.json` in the app's data folder (`WABCUT_EFFECTS_LIBRARY` overrides it; wabcut-cli and MCP sessions
  keep theirs in memory unless the host gives one). Saving a preset or changing a bin is therefore not an undo step.
  Exported presets are `*.wabcut-presets.json` files (`{"format": "wabcut-effect-presets", "version": 1, "presets":
  [...]}`); importing renames a preset whose name is taken ("Glow 2"). Library and preset files are untrusted input:
  bounded (16 MiB, 1,000 presets of at most 64 effects, 256 presets bins nested at most 8 deep, 128 custom bins of
  1,000 items) and checked like a project's effects (ranges, kinds, keyframes), a newer version refused.
- **Portable data.** A LUT a grade refers to is saved as its file path and becomes an asset of the project the preset is
  applied to (the one with that path, or a new one); masks that follow a tracked matte are left out (they belong to one
  media file).

### Built-in presets

Premiere 25 retired its Bevel Edges, Solarizes, Convolution Kernel and Blurs preset folders with the obsolete effects;
Wabcut keeps them on its own effects. PiP positions and pans are made for the sequence's frame size (Premiere's
are fixed pixel values from SD days) and assume a frame-size clip.

| Bin | Presets | What they do |
|---|---|---|
| Bevel Edges | Bevel Edges Thick, Bevel Edges Thin | Bevel Edges at Edge Thickness 0.2 / 0.05 |
| Blurs | Fast Blur In, Fast Blur Out | Gaussian Blur from 5 % of the frame height to 0 over the first second (anchored to the In point), and back over the last second (anchored to the Out point) |
| Mosaics | Mosaic In, Mosaic Out | Mosaic from cells of 10 % of the frame height to 0.1 % over the first / last second |
| PiPs/25%, PiPs/40% | PiP 25% UL, UR, LL, LR (and 40%); PiP 25% UL Scale In / Scale Out ...; PiP 25% UL Spin In / Spin Out ...; PiP 25% Motion UL to LR, LR to UL, UR to LL, LL to UR (and 40%) | Motion: the clip at 25 % or 40 %, 5 % of the frame from the corner's edges; Scale In / Out grow from or shrink to nothing over the first / last second; Spin In / Out do the same turning a full turn (0.2); Motion presets travel between opposite corners over the clip (Scale) |
| Solarizes | Solarize In, Solarize Out | Solarize Threshold 0 to 1 over the first second, and back over the last |
| Twirls | Twirl In, Twirl Out | Twirl (radius 50) unwinding a full turn over the first second, winding over the last |
| Image Pans | Horizontal Image Pan Left to Right / Right to Left, Vertical Image Pan Top to Bottom / Bottom to Top; 0.4: Slow Push In, Slow Pull Out, Push In to Top / to Bottom, Push In Left to Right / Right to Left | Motion at 120 %, Position moving 10 % of the frame across the clip (Scale). The 0.4 Ken Burns moves for stills and archive run over the whole clip (Scale) with eased keys (Ease In and Out) you can edit in Effect Controls: Slow Push In Scale 100 % to 112 %, Slow Pull Out 112 % to 100 %; Push In to Top / Bottom Scale 100 % to 112 % with Position Y moving 5 % of the frame height; Push In Left to Right / Right to Left Scale 108 % to 112 % with Position X moving 3 % of the frame width each way. Position keys share the scale keys' times, so a frame edge never shows, and only the Motion parameters a move uses are set (Rotation and Anchor stay the clip's) |
| Animations (0.2) | Fade In / Out; Pop In / Out; Spin In / Out; Pull In / Out; Travel In from Left / Right / Top / Bottom, Travel Out to Left / Right / Top / Bottom; Spring In / Out | Premiere 25.5's motion animations for any video clip, as editable keys over half a second at the In point (In) or the Out point (Out): Fade - Opacity 0 to 100 %; Pop - Scale from 0 past 110 % to 100 % with a quick fade; Spin - half a turn while Scale grows from 0; Pull - Scale from 160 % to 100 % while fading in; Travel - Position from a frame's width (height) off that side; Spring - up from below the frame with a damped bounce (0.66 s). Out animations run the In ones backwards. Only the parameters an animation moves are set (Pop leaves Position alone), with absolute values as every preset (a PiP at 25 % pops to 100 %) |
| Convolution Kernel (0.2) | Blur, Blur More, Darken, Emboss, Find Edges, Gaussian Blur, Gaussian Sharpen, Lighten, Sharpen, Sharpen Edges, Sharpen More | Convolution Kernel with the classic 3 x 3 kernels under the names of Premiere's Convolution Kernel bin (the kernels are textbook ones, not read from Premiere): the blurs average (2 at the centre, all 1, Gaussian 1-2-1 / 16), the sharpens subtract the neighbours (Sharpen Edges at half strength, Gaussian Sharpen against a Gaussian), Emboss is a grey relief (Offset 128), Find Edges a Laplacian, Lighten / Darken scale the centre (120 % / 80 %) |
| Lumetri Presets: Cinematic, Filmstock, Monochrome, Technical | Teal & Orange, Cool Night; Warm Film, Bleach Bypass; Mono Silver; Sony S-Log3 (S-Gamut3.Cine) to Rec.709, Panasonic V-Log (V-Gamut) to Rec.709 | Color Grade with the built-in look (Creative > Look) or input LUT (Basic > Input LUT) of [color.md](color.md) |

Premiere 25.5 made its motion animations transitions dragged onto a clip's edge; since 0.2 Wabcut has them both ways:
these presets write editable keys, and the Animations transitions (Video Transitions > Animations, above) sit on the
edge with a duration handle, plus Block, Flip, Fold and Surprise Me.

Not there: Lens Distortion Removal (Premiere's presets are
correction profiles of particular GoPro and DJI cameras; Wabcut has the Lens Distortion effect but no measured camera
profiles to ship) and Premiere's own `.prfpset` preset files (a binary-encoded format; Wabcut imports its own preset
files only). Save Preset… saves the effect right-clicked or all of them (Premiere saves the effects selected in Effect
Controls; 0.4 Wabcut selects effects there too, for Cut / Copy / Paste, below); Time Remapping is not saved in presets.

### Effect Controls: select, copy and paste effects (0.4)

As in Premiere's Effect Controls, the section headers (Transform, Opacity, Audio and every effect of the stack) can be
selected and moved between clips:

- **Select.** Click a header to select it (an accent outline). Ctrl+click adds or removes one, Shift+click selects from
  the last one clicked, and Ctrl+A (Edit > Select All with Effect Controls focused) selects every section. Esc clears the
  selection. Clicking a header gives Effect Controls the keyboard. Selecting a section clears the selected keyframes and
  selecting a keyframe clears the sections. A transition shown in the panel is not selectable (Backspace removes it, as
  Delete does on the timeline).
- **Remove.** Backspace (Remove Selected Effect) or Delete removes the selected stack effects and resets the selected
  Transform, Opacity and Audio sections to their defaults (their keyframes go too), as Premiere does: one undo step,
  "Remove Effect", "Remove Effects" or "Reset Effect". Selected keyframes win: with keys selected, Delete and Backspace
  delete those. With nothing selected Backspace says "Select an effect first".
- **Copy and paste.** Ctrl+C (Ctrl+X cuts) with sections selected puts them on an effects clipboard; then select clips
  on the timeline and press Ctrl+V (Edit > Paste works too). The video sections go onto video clips and the audio ones
  onto audio clips (a selected clip of the other kind stands in for its linked partner). Fixed sections replace the
  target's values; stack effects are added after the target's own, keeping their keyframe times from the clip start. One
  undo step, "Paste Effects", and a toast "Pasted 2 effects to 5 clips"; clips on locked tracks are skipped and the toast
  says how many. With no section selected Ctrl+C copies timeline clips as before; the newest copy wins, so a timeline
  copy after an effects copy makes Ctrl+V paste clips again.
- **The header menu.** Right-click a header (an unselected one is selected alone first): **Cut**, **Copy**, **Paste**,
  **Clear** act on the selection; **Effect Enabled** (stack effects), **Reset**, **Move Up** and **Move Down** act on the
  header right-clicked; then **Save Preset…** and **Save All Effects as Preset…**. Every row is one undo step except Copy.
  Reset (and the header's reset button) also works on a collapsed section: it reads the effect's parameters, not the
  rows shown.

Wabcut differs from Premiere in three ways: the effects clipboard is kept for the session apart from the clip clipboard
(the newest copy wins, as in Premiere); Time Remapping is not a section here (it stays in the speed controls above the
list); and Transform, Opacity and Audio have no Effect Enabled switch.
