# Wabcut design language v3: "Echo"

Owner: lead/CTO. Status: **binding direction** (2026-10-15), replacing v2 "Ultraviolet Cut". The user supplied the
reference boards (Home, Editor, a pill UI kit and the brand sheet with the echo mark) and asked for this to be the style
of the program, with a new logo, keeping the name Wabcut. `docs/design-system.md` lists the tokens and components that
implement it; where the two disagree, this one wins.

## The idea
Wabcut is an editor you play: a **black stage** with rounded **islands** on it, every control a **pill**, and colour
that means something (a panel's hue, a clip's label, the accent on what is chosen). Its brand is the **echo mark**:
five rounded clips stacked like timeline tracks, trimmed short-long-longest-long-short, the longest in lime, with two
fainter echoes trailing behind, as if the edit had just moved.

Five things make a screenshot recognisably Wabcut:
1. **Islands on black.** Panels are rounded islands (radius 22) on a true-black stage, 8 px apart.
2. **Tab strips.** A docked panel opens with a dark strip; its tab rises from it in the panel's colour with flared feet
   and the panel's hue dot. Groups of panels show browser-style tabs there.
3. **Pills.** Buttons, chips, segmented groups, fields, the transport and the track headers are pills; icon buttons are
   circles. Text on the accent (and on every hue) is black.
4. **Violet and lime.** Violet `#9B6CFF` is the accent and the brand; lime `#D6F54C` is the brand's second colour (the
   mark's longest clip, Create, stars, applied effects). Four supporting hues tag panels and clips.
5. **Solid clips.** Clips are solid fills of their label colour with black names and dark waveforms; the playhead is a
   white line with a round knob.

## What changes from v2
- Violet-ink surfaces → neutral near-black islands (`#141415`) on black; the window background stays black.
- The 12° slice (controls, cards, the logo, the selected clip's notch) → gone. `SliceShape` stays for its callers with
  depth 0, so a `SliceRect` is a plain rounded rectangle.
- The UV beam gradient → gone; progress, the playhead and brand moments are flat colour.
- Label-strip clips → solid clips.
- IBM Plex / Bai Jamjuree UI type → Manrope and JetBrains Mono (Plex Sans Thai still sets Thai).
- The W logo → the echo mark. The name stays Wabcut.
- Docked panel headers → tab strips; the slice-mark section header → a dot and a bold title.
v2's ban on pills and lime is lifted: the user chose this direction on 2026-10-15.
Kept: the Premiere keymap, the frameless window, the workspaces and docking, typed undoable actions, and every
accessibility and reduce-motion rule.

## Colour
### Surfaces
| Token | Dark | Light | Use |
|---|---|---|---|
| canvas | `#000000` | `#EDEDF0` | the stage: the window background, shows between islands |
| sunken | `#0B0B0C` | `#E2E2E7` | tab strips, the timeline well, segmented tracks |
| panel | `#141415` | `#FFFFFF` | the islands |
| section | `#1B1B1D` | `#F6F6F8` | cards inside a panel (Motion, Opacity…) |
| raised | `#1D1D20` | `#F0F0F3` | buttons, fields, chips, menus |
| hover | `#26262A` | `#E6E6EB` | hover and the selected row or nav item |
| well | `#34343A` | `#D9D9DF` | stepper buttons, slider tracks, scroll thumbs |
| text primary / secondary / tertiary / disabled | `#F2F2F3` `#B4B4BA` `#8C8C94` `#55555C` | `#0B0B0C` `#4A4A52` `#6B6B74` `#B4B4BA` | text (tertiary ≥ 4.5:1 on panel and raised, tested) |
The monitor well is black in both themes. There is no rim light and no panel shadow; menus and toasts cast a soft one.

### Accent and hues
The accent (Settings > Appearance) fills the chosen thing: the primary button, the active workspace, the selected
segment, checked toggles. Text and glyphs on it are **black**. Each accent keeps the v2 key so saved settings carry over.
| Accent (key) | fill / dark stroke | light stroke | soft | pressed |
|---|---|---|---|---|
| Violet (`ultraviolet`, default) | `#9B6CFF` | `#6A3DE8` | `#C3A8FF` | `#8A5CFF` |
| Pink (`infrared`) | `#FF5FA8` | `#C0256E` | `#FFC2DD` | `#E84A93` |
| Lime (`phosphor`) | `#D6F54C` | `#5E7A00` | `#EEFBB0` | `#BEDC34` |
| Orange (`sodium`) | `#FF7A2F` | `#B54700` | `#FFCAA8` | `#EA661B` |
| Blue (`ice`) | `#4D8DFF` | `#1F5FD6` | `#B8D2FF` | `#3A78EA` |
Black on every fill is 6–17:1; the light strokes are deeper shades so they keep 3:1 on white.

The six **hues** (violet `#9B6CFF`, lime `#D6F54C`, pink `#FF5FA8`, mint `#3EE6B8`, blue `#4D8DFF`, orange `#FF7A2F`) tag
things, never state: a panel's tab dot (Program/Source/Timeline violet, Library blue, Effect Controls and Effects lime,
Color orange, audio panels mint, captions and markers pink), Home's nav dots, project cards. Semantic colours: success
mint, warning amber, danger rose, info blue.

### Clips
Premiere's 16 labels, retuned so each carries black text at ≥ 4.5:1 (tested). A clip is a solid fill of its label colour
(radius 9), its name in black, an audio waveform in black at 32%. Clips that show thumbnails keep a 16 px band of the
label colour along the top for the name. Selected: a 2 px white ring outside a 2 px gap. Disabled or offline: hatched.
Transitions are white with a black diagonal.

## Geometry
- **Islands:** radius 22, 8 px gutters (the gutters are the splitters), padding 12. Cards inside: radius 18. Menus and
  popovers: radius 20.
- **Tab strip:** 36 px (compact 32) of `sunken` with the island's top corners. A tab is 30 px (compact 26) high, radius
  12 on top, its feet flared into the panel with two concave corners, a hue dot and a bold 12 px name. A lone panel
  shows its own tab; a group of several shows the group's tabs (`DockTabs`, owned by the slot and laid over the strip).
  The panel's own tools follow the tab; the grip, pop-out and close sit at the strip's right.
- **Pills:** buttons and segmented controls are pills at control height (32, compact 26); the title bar's are 40.
  Icon buttons are circles. Single-line fields are pills; multi-line fields and wells use radius 14.
- **Docking (0.1.3)** is unchanged in behaviour: groups, tabs, drag to join, dock beside, run along an edge or float.
  The header zone that joins a group is its tab strip. UX rework (Premiere's frames): the group clicked last is the
  **active group**, drawn with a 2 px `accentStroke` outline along the island's edge (keyboard focus rings stay the white
  / black rings inside); the Window menu opens closed panels there. A **maximized** group takes the whole work area
  (the others hide, no gutters) until it is restored. A tab's right-click menu is the standard `ContextMenu` with icons.
- Grid 4 px; dense rows 28; dots 8.

## Type
- **Interface and display:** Manrope (SIL OFL 1.1): Medium for body, Bold for labels, buttons and tabs, ExtraBold for
  titles, primary buttons and the wordmark.
- **Numbers, timecode, meters, formats:** JetBrains Mono (SIL OFL 1.1).
- **Thai:** IBM Plex Sans Thai, substituted into both faces (it carries Thai vowels and tone marks; a test checks they
  stack).
- IBM Plex Sans/Mono and Bai Jamjuree stay bundled for titles, caption styles and templates (they are part of the
  picture, not the interface).
- Scale: 11 meta · 12 labels · 13 controls and body · 14 panel titles · 16 dialog and project titles · 22 heroes · 30
  the Home greeting. Sentence case; caps only for 10 px micro labels (CONTINUE, NEW PROJECT, PROXY).

## Chrome
- **Title bar (52 px, compact 44):** the echo mark in a round raised badge (it opens the File menu) with the project's
  name and its frame size and rate in mono; the menu bar; the sequence switcher; the workspace switcher as a segmented
  pill group in the middle (the active workspace on a sliding accent pill); job pills while something runs; round
  command palette, undo and redo; Save (with a dot while there are unsaved changes) and Export (the accent pill); round
  window controls.
- **Menu bar (UX rework, it replaced the left rail):** Premiere's File, Edit, Clip, Sequence, Markers, Graphics, View,
  Window and Help as Bold 13 px text pills after the brand: `textSecondary` at rest, `hover` under the pointer, `raised`
  while their menu is open (the menu is the same raised card, so title and menu read as one). Menus have no icons: a
  check gutter on every row keeps the labels lined up, and a switch that is on shows the accent dot in it; shortcuts sit
  right in mono `textTertiary`. Short of room the titles fold into one "Menu" pill with a chevron. The work area starts at
  the window's gutter; panels open from the Window menu.
- **Home:** a start window, not a scrolling page. A nav island on the left (the logo, the pages Home / Projects / Learn
  with hue dots, the version and Settings at the bottom); a header with the page's title (the greeting on Home), a
  search pill, Open…, Import… and the accent New project pill; the pages cross-fade below. Home: Continue (the last
  project, large) beside Start (three big action rows with their keys, a dashed drop well), then Recent cards. Projects:
  cards or rows, All or Starred, with a "…" menu per project. Learn: compact rows (a tinted icon circle, a title, one
  line, where it leads). New project: format tiles with the frame's shape (the chosen one on the accent fill) and a
  Details island (name, location, the mono summary, Advanced settings as label + pill rows, the frame drawn at its
  shape, the lime Create pill). Messages are tinted strips under the header.
- **Monitors:** a black well (radius 12), the timecode in an accent pill, round transport buttons around a white play
  button, a pill time bar with the In/Out range in accent and a white playhead.
- **Timeline:** track headers are pills (the id chip in accent when targeted), the tool palette is a segmented pill
  group, the ruler's playhead is a white knob. Track toggles, headers and mixers alike: mute amber (warning), solo green
  (success), record red (danger), clipInk glyphs; the crossed speaker only when muted.
- **Toasts:** a `section` card at the bottom right with a soft shadow, a dot (the accent; `warning` or `danger` for a
  problem) and a DemiBold 13 px message that wraps to three lines. One thing to do is one compact accent pill ("Save
  As…", "Show in Explorer", "Undo") with a round close button after it. Notes leave after 4 s, problems after 7 s, a
  toast with an action after 8 s; a pointer moved onto it holds it.

## Brand: the echo mark
- Geometry (`resources/brand/wabcut-mark.svg`, `components/EchoMark.qml`): a 120-unit box; five 12-unit clips at y 18,
  36, 54, 72, 90, widths 28, 52, 78, 52, 28, violet with the longest lime; the mark at x 34, echoes at x 20 (45%) and
  x 6 (18%). Below 28 px: three thick clips (40, 90, 40), no echoes.
- In light and high-contrast themes the mark is drawn in one ink.
- App icon: the mark on a `#1D1D20` tile (radius 22%), drawn per size: 16–24 px three clips, 32–48 px one echo, 64 px
  and up the full mark.
- **Launch intro** (once per launch, 2.1 s, skipped with reduce motion or in Settings): one thin line splits into five
  tracks, trims to length, then the echoes peel off. A click or Esc skips it.
- **Loaders**, all built from the mark and static under reduce motion: **Equalizer** (audio analysis, loudness),
  **Chase** (opening projects, waiting on the assistant), **Scan** (render, preview and export), **Build** (imports,
  proxies, conforming). With a label they sit in a raised pill ("Analyzing audio").

## Motion
- Snap-out at 140 ms for state and 220 ms for layout; springs only on drag release. Hover changes colour only; press
  scales to 0.98.
- Signature moments: the playhead glows softly while playing; a cut flashes at the edit point; workspace switches
  cross-fade; the launch intro; the loaders.
- Nothing loops at rest (loaders only run while their job does). Every animation honours reduce motion and never blocks
  input. Timeline motion stays in the scene graph.

## Accessibility (unchanged, binding)
WCAG AA: text ≥ 4.5:1, strokes and focus ≥ 3:1, both themes tested. A visible focus ring on every Tab stop (2 px, white on
dark and black on light, with a 2 px gap), keyboard reachability, screen-reader names, reduce motion and a high-contrast
mode (AAA text). Colour never carries meaning by itself.
