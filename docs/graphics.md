# Motion graphics

Wabcut's motion graphics (C.13) are Premiere's Essential Graphics and After Effects' shape and text layers in one model:
a **graphic clip** holds layers - text with rich runs, shapes, images and groups - each with a keyframable transform,
a blend mode, a drop shadow and masks. Graphics are animated with **presets** that write ordinary keyframes, saved as
**templates** with editable fields (MOGRT-like), themed by the project's **brand kit**, and imported from **Lottie**
files. Rendering: `docs/internal/c13-graphics-handoff.md`. Commands: [commands.md](commands.md)
(`graphic.*`, `template.*`, `brand.*`, `lottie.import`).

## Concepts

- **Graphic clip.** A clip on a video track that plays the project's canvas item for the sequence's frame size and rate
  (a generator media item, `"wabcut.graphic"`) and holds a `Graphic`. Copies are independent graphics.
- **Units.** Pixels of the graphic's **design canvas** (`width` x `height`, y down; the frame size by default, a Lottie
  file's own size when imported). Scales and opacities are percentages, rotations degrees clockwise, colours sRGB 0..1
  with alpha. The design canvas is fitted into the clip's frame (contain, centred); the clip's Motion applies on top.
- **Time.** Keys are in the clip's curve time: they stick to the clip like any keyframe, and a razor cut evaluates
  seamlessly. A graphic with **responsive time** (`responsive`: `duration`, `intro`, `outro`) is designed `duration`
  long: its protected intro plays from the clip's start and its outro ends at the clip's end, whatever the clip's length;
  the middle stretches (Premiere's Responsive Design - Time). Its keys are in that design time.
- **Pins.** A layer pinned to edges of a sibling follows them (Responsive Design - Position): a box pinned to all four
  edges of a name grows with the name; a rectangle, ellipse or image pinned on two opposite edges resizes.
- **Bounds.** A graphic holds at most 256 layers, nested 8 deep, 16 KiB of text per layer, 1024 vertices per path; every
  value is range-checked. Graphics come from project files, templates and Lottie files: untrusted input.

## In the Program Monitor

The selected graphic's layer has handles on the picture: drag inside to move (snapping to the canvas, the safe margins
and the other layers; Ctrl does not snap), the corners to scale (Shift: freely), the round handle to rotate, the cross to
move the anchor point; a paragraph text box resizes from its edges. Arrows nudge (Shift: 10 px), Tab picks the next
layer, Enter edits text, Esc drops the selection. T (the Type tool) adds a text layer where you click.

Picking (0.1.3) works on everything in the picture at the playhead, not only the selected graphic, like Premiere's
selection tool in the Program Monitor:

- **Click** selects the object under the pointer: a graphic's layer, a title, or a clip on a video track (as clicking
  it in the timeline would; a nested clip as one). The topmost one wins, except that the object you picked last stays
  picked while it is under the pointer, so an object behind another can be dragged once picked. A clip selected in the
  timeline does not count as picked. Pointing outlines what a click would pick (dashed); the picked clip or title is
  outlined in the accent.
- **Double-click** a text (a graphic's text layer, or a title clip such as "Your Title") to edit it in place, even when
  nothing was selected. Enter keeps the text (Shift+Enter: a new line), Esc cancels; a title's text changes every clip
  of that title, one undo step.
- **Double-click again** to go one level deeper: each double-click on the same spot picks the next object behind the
  picked one, wrapping back to the top. A text met on the way is picked first and edited with the next double-click.
  Picking anything elsewhere (the timeline, Tab, Esc) starts over.

Clicks the other tools take (mask drawing and mask outlines, review marks, object prompts, layout regions, the Type
tool) stay theirs.

## JSON

The same JSON is a clip's `"graphic"` in project files, the `graphic` and `layer` parameters of the commands, and the
graphic inside template files. Keys at their default are left out; readers accept them all.

```json
{
  "name": "Lower third", "width": 1920, "height": 1080,
  "responsive": {"duration": 1270080000000, "intro": 254016000000, "outro": 254016000000},
  "layers": [
    {"id": 1, "kind": "shape", "name": "Box",
     "shape": {"type": "rectangle", "width": 600, "height": 120, "cornerRadius": 16,
               "fill": {"color": {"brand": "Primary", "rgba": [0.2, 0.2, 0.2, 1]}}},
     "pin": {"to": 2, "edges": ["left", "top", "right", "bottom"], "offsets": [-24, -12, 24, 12]},
     "transform": {"x": 500, "y": 900}},
    {"id": 2, "kind": "text", "name": "Name",
     "text": {"runs": [{"text": "Somchai", "size": 64, "brandFont": "heading"}],
              "animators": [{"name": "Typewriter In", "unit": "character", "opacity": 0,
                             "start": {"keys": [{"t": 0, "v": 0}, {"t": 127008000000, "v": 100}]}}]},
     "transform": {"x": 220, "y": 860, "opacity": 100}}
  ]
}
```

- **Numbers that can animate** (transform values, shape sizes, radii, stroke width, trim, selector start/end/offset) are
  a number, or a curve `{"keys": [{"t": ticks, "v": value, "i": "linear" | "hold" | "bezier" | "easeIn" | "easeOut" |
  "easeInOut", "in": {"slope", "influence"}, "out": {...}}, ...]}` (the project file's curves; a bare array of keys is
  read too). Times are ticks, 254,016,000,000 per second.
- **Colours** are `[r, g, b, a]` (0..1), `"#RRGGBB"` / `"#RRGGBBAA"`, or `{"brand": "Primary", "rgba": [...]}`: the kit's
  colour of that name when the project's kit has one, else `rgba`.
- **Layer**: `id` (unique, not 0), `kind` (`text`, `shape`, `image`, `group`), `name`, `visible`, `transform`
  (`x`, `y`, `anchorX`, `anchorY`, `scaleX`, `scaleY` %, `rotation`, `opacity` %), `blend` (`normal`, `multiply`, `screen`,
  `overlay`, `add`, `darken`, `lighten`, `difference`), `shadow` (`enabled`, `color`, `opacity` 0..1, `angle`,
  `distance`, `softness`), `masks` (the project file's masks, [masks.md](masks.md): the layer's space normalised by the
  canvas), `in` / `out` (ticks: when it shows), `pin` (`to`, `edges`, `offsets` left/top/right/bottom), and one of
  `text`, `shape`, `image`, `children` (a group's layers, bottom to top).
- **Text**: `runs` (`text`; `font`, `style`, `brandFont` (a kit font role), `weight` 100..900, `size`, `fill`, `italic`,
  `underline`, `tracking` 1/1000 em, `leading` px, `baselineShift`), `align` (`left`, `center`, `right`, `justify`),
  `boxWidth` / `boxHeight` (paragraph text; 0 = point text), `language` (`"th"`), `background` (`enabled`, `color`,
  `padding`, `radius`), `animators`.
- **Text animators** (After Effects' range selector): `unit` (`character` - grapheme clusters, so a Thai consonant with
  its marks is one -, `word` - the line breaker's words, `line`), `start`, `end`, `offset` (% of the units), `shape`
  (`square`, or `ramp` with `softness` units), and what fully selected units get: `opacity` %, `offsetX`, `offsetY`,
  `scale` %, `rotation`.
- **Shape**: `type` (`rectangle`, `ellipse`, `polygon`, `star`, `path`), `width`, `height`, `cornerRadius`, `points`,
  `outerRadius`, `innerRadius`, `path` (keys `[{"t", "i", "v": [[x, y, inX, inY, outX, outY], ...]}]`, same vertex count in
  every key), `closed`, `fill` (`type`: `none`, `solid`, `linear`, `radial`; `color`; `stops` `[{"offset", "color"}]`;
  `start`, `end`), `stroke` (`enabled`, `color`, `width`, `dashes`, `join`, `cap`), `trimStart`, `trimEnd` %, `trimOffset`.
  Shapes are centred on the layer's origin.
- **Image**: `media` (a media id), `brandLogo` (a kit logo name, used when the kit has it), `width`, `height`, `fit`
  (`contain`, `cover`, `fill`, `none`).
- **Template instance** (`"template"` on a graphic made from a template): `id`, `version`, `fields` with their current
  values.

## Animation presets

`graphic.applyPreset` writes editable keyframes into a layer (nothing opaque: change or delete them like any keys):

| Preset | In / Out | Loop | Writes |
|---|---|---|---|
| `fade` | from / to 0 % opacity | dims to 25 % and back | `transform.opacity` |
| `slide` | from / to `distance` px in `direction` | there and back | `transform.x` or `y` |
| `scalePop` | from 0 past full size (`overshoot` %) / back to 0 | a pulse | `transform.scaleX`, `scaleY` |
| `bounce` | drops in and settles in smaller hops / hops out | hops | `transform.y` |
| `typewriter` | characters appear (disappear) in turn | - | a "Typewriter In/Out" text animator |
| `wordByWord` | words rise into place in turn | - | a "Words In/Out" text animator |
| `wipe` | revealed from (hidden toward) `direction` | - | a "Wipe" rectangle mask |

`duration` (default 0.5 s; one cycle of a loop), `easing` (`linear`, `easeIn`, `easeOut` - the default -, `easeInOut`,
`overshoot`). In presets key the graphic's start, Out presets its end (on a responsive graphic: its design time, so an Out
animation always ends with the clip). Keys a preset writes replace those inside its window.

## Templates

A template is a graphic with **fields**: `text` (a text layer's text), `color` (`fill`, `stroke`, `background` or
`shadow` of a layer), `font` (a text layer's font), `image` (an image layer's media), `duration` (`out`: a layer's out
point; `clip`: the clip's length), `checkbox` (a layer's visibility). `template.save` stores one in the project's library
(saved in the project file) or the user's library folder (one `<id>.wabcut-template.json` each:
`{"format": "wabcut-graphic-template", "version": 1, "template": {"id", "name", "description", "version", "graphic",
"fields", "duration"}}`; a newer format version is refused). Template files are untrusted: read bounded and checked.

`template.instantiate` adds a clip of a template with field values; `template.setField` changes them later, several in
one undo step. The instance keeps the template's id and version and its fields, so it can be edited without the
template.

## Built-in templates (C.16)

Wabcut ships a gaming pack of 12 templates for YouTube, Twitch and TikTok creators. They are ordinary C.13 template files,
compiled into the model (`BuiltinTemplates.h`: `builtinTemplates()` in id order, `findBuiltinTemplate(id)`,
`templatePosterTime`, `templateCategory` - `"gaming"` for `wabcut.gaming.*`), so every host has them without a resource
folder. `template.list` returns them with library `"builtin"`; `template.instantiate` finds an id in the project's
library, then the user's, then the built-ins; `clip.instantReplay` lays `wabcut.gaming.replay` (`kReplayTemplateId`) over its slow-motion segment and sets its `label`
field.

| Id (`wabcut.gaming.`) | Name | Fields (kind) | Length | Intro / outro | Animation |
|---|---|---|---|---|---|
| `arrowCallout` | Arrow callout | `label` (text), `accent` (color: arrow fill), `length` (duration: clip) | 4 s | 0.6 / 0.3 s | slide in (overshoot) + fade; a slide loop keeps it poking at the target; label scale pop; fades out |
| `chatBubble` | Chat bubble | `username`, `message` (text), `accent` (color: name tag background), `avatarColor` (color), `showAvatar` (checkbox), `length` | 5 s | 0.65 / 0.3 s | scale pop from the avatar, name fade, message word by word; slides up and fades out |
| `circleCallout` | Circle callout | `label` (text), `accent` (color: circle stroke), `length` | 4 s | 0.75 / 0.4 s | trim keys draw the ellipse on (end 0 -> 100 %) and off (start); leader line trim; label fade + slide; fades out |
| `countdown` | Countdown 3-2-1 | `count3`, `count2`, `count1`, `go` (text), `accent` (color: ring stroke) | 4.5 s | 3.5 / 0.5 s | each count shows 1 s (in / out points), scale pop in, fade out; GO! pops in, holds through the middle, pops out; ring scale pop + trim |
| `emotePop` | Emote pop | `emote` (image), `caption` (text), `accent` (color: burst fill), `showCaption` (checkbox) | 3 s | 0.75 / 0.35 s | image bounce + fade, burst / glow / sparkles / caption scale pops, burst pulse (scale pop loop); scale pop out |
| `endCard` | End card | `title`, `handle` (text), `accent` (color: bar fill), `video1Thumb`, `video2Thumb` (image), `showChannel` (checkbox), `length` | 20 s | 1 / 0.6 s | backdrop fade, title slide (overshoot), bar and slot scale pops, channel ring trim; fades out |
| `intro` | Gaming intro | `title`, `subtitle` (text), `accent` (color: band fill), `length` | 4 s | 0.9 / 0.4 s | band and stripe wipe in, title slide (overshoot) + fade, channel line word by word, rules wipe; band wipes out, title slides up |
| `killStreak` | Kill streak banner | `title`, `subtitle` (text), `accent` (color: bar fill) | 3 s | 0.7 / 0.4 s | banner scale pop (30 % overshoot), slashes wipe, bar pop, subtitle typewriter + fade; slides right and fades out |
| `lowerThird` | Gamer lower third | `name`, `handle` (text), `accent` (color: bar fill), `length` | 5 s | 0.7 / 0.4 s | slide in from the left (overshoot) + fade, name typewriter, bar pop, handle fade + slide; slides out left |
| `replay` | Replay stinger | `label` (text, "REPLAY"), `accent` (color: tag fill), `stingerColor` (color: band fill) | 8.5 s | 0.75 / 0.45 s | flash fade in / out and band wipe; the tag slams in at 300 % mid-frame and flies to the corner (hand keys); dot fade loop (0.8 s cycles) over the middle (designed at a typical replay's length, so it blinks near 0.8 s in the usual case: the middle stretches with the clip); slides out left |
| `subscribe` | Subscribe, like and bell | `label` (text), `accent` (color: button fill), `showLike`, `showBell`, `showCursor` (checkbox) | 4 s | 1.4 / 0.6 s | the three buttons scale pop 0.2 s apart, the pointer fades and slides up, a click (scale keys), the bell swings (rotation keys); pop out in reverse order |
| `webcamFrame` | Webcam frame | `name` (text), `accent` (color: border stroke), `tagColor` (color: tag background), `length` | 6 s | 0.7 / 0.35 s | the border draws on and off (trim), glow fade, corner brackets pop, name tag fade + typewriter; fades out |

- **Design rules.** A 1920 x 1080 canvas with responsive time; the template's `duration` is the responsive duration.
  Everything animates in from nothing during the intro and out to nothing during the outro; the middle, which stretches
  with the clip, only holds or plays loops. Animations are presets baked with `edit::applyPresetToLayer` (plus a few hand
  keys: the click, the bell's swing, trims, the replay tag's flight): ordinary keys the user can edit. Boxes behind user
  text are pinned to it, so they grow with a longer or translated text. Every colour field binds one layer; the main one
  is always `accent`. Each template has a one-line English `description` (the template browser shows it).
- **Field ids are stable.** They are a public contract: instances, commands (`clip.instantReplay` sets the replay's
  `label`) and the UI refer to fields by id, and an instance keeps working after its template is tuned only while the ids
  stay. A test pins the exact id list of every built-in; never rename or reuse one, add new ids instead.
- **Fonts.** Bundled, with Thai: headings in Bai Jamjuree 600 (the bundle has 500 and 600), body text in IBM Plex Sans
  Thai (400, 500, 600). The default texts are English; every text field takes Thai (the tests set each to Thai and check
  the fonts and grapheme clusters).
- **Artwork.** Original shapes and text only: icons (thumbs-up, bell, pointer, play triangle, arrow, person) are paths;
  no logos, brand-kit references or platform names.
- **Video slots.** A graphic image layer cannot play video (C.13), so the end card's two rounded 16:9 frames are
  placeholders where the platform's end-screen video elements go (YouTube end screens are 5-20 s); each has an optional
  thumbnail image field (`video1Thumb`, `video2Thumb`), empty by default. An empty image slot resolves to an image item
  without a media, which draws nothing.
- **Poster time.** `templatePosterTime`: the end of a responsive template's intro, where every part is in and at rest
  (the library's thumbnail: `resolveGraphic(tmpl.graphic, templatePosterTime(tmpl))`); otherwise half the template's
  length.
- **Files.** `src/model/templates/builtin/<id>.wabcut-template.json`, exactly as `io::templateFileToJson` writes them,
  are the source of truth; the build embeds every file of the folder (`src/model/cmake/EmbedTemplates.cmake`, a generated
  `EmbeddedTemplates.cpp`). They are made by `tests/model/gaming_templates_author.cpp`; to change a template, edit its
  builder, run the model tests once with `WABCUT_UPDATE_TEMPLATES=1` (`wabcut_model_tests "[templates]"`; the files are
  rewritten), rebuild (the embed step re-runs) and run them again without it. The tests fail when the files and the
  builders differ, when a file is not canonical, and on any rule above.

## Brand kit

The project's named colours, font roles (`heading`, `body`, ...) and logos (media of the project). Graphics, templates and
caption styles (`CaptionStyle::brand`) refer to them by name; `brand.set` with another kit re-themes everything that does.
JSON: `{"name", "colors": [{"name", "rgba"}], "fonts": [{"role", "family", "style", "weight"}], "logos": [{"name",
"media"}]}`.

## Lottie

`lottie.import` (and `interchange::importLottie`) reads Bodymovin JSON (After Effects exports, LottieFiles) into a
graphic: keyframe easing becomes Bezier keys, so motion matches. The report lists each layer **carried** or what was
**approximated** (changed - the reason says how) and **dropped**, with the JSON path of the feature (category `graphic`,
`effect`, `media`, `marker`).

| Lottie | Imported as |
|---|---|
| shape layers, groups, transforms (also split position) | groups with their transforms |
| rectangles (rounded, also by Round Corners), ellipses, polystars, paths (animated) | shapes |
| fill, stroke (dashes, caps, joins), linear / radial gradient fill (opacity stops) | fill and stroke |
| trim paths | trim (applied to each path: "individually" is approximated) |
| solid layers | rectangles |
| text layers: the first text document; range selectors (opacity, position, scale, rotation) | text with animators; point text placed by the font's ascent (approximated) |
| precomps (with st, sr) | groups clipped to the precomp's size |
| parenting (null and other parents) | wrapper groups with the parents' transforms (not their opacity, as in After Effects) |
| masks (add, subtract, intersect, inverted, opacity, expansion, animated) | layer masks |
| in / out points, hidden layers, blend modes normal, multiply, screen, overlay, darken, lighten, difference, add | the same |
| Drop Shadow effect | the layer's drop shadow (approximated) |
| several fills or strokes on a path | the nearest of each (approximated) |
| curved motion paths, path easing, skew, 3D, other blend modes, gradient strokes, text strokes | approximated |
| images, other effects, track mattes, merge paths, repeaters, other shape modifiers, markers, cameras, time remapping, expressions (their values are kept) | dropped |

Bounds: 16 MiB, nesting, 10,000 layers per composition, 1,000 assets, 10,000 keys per property and 500,000 in all,
1,000,000 path vertices in all, 256 layers in the graphic (the rest are dropped and reported). The fuzz harness
`fuzz_lottie` (tests/fuzz) runs every Lottie fixture and its mutations.
