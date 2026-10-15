# Wabcut roadmap

Wabcut is an **alpha**. This page lists what has shipped and what comes next. It is a plan, not a promise: the order
will change as people use the editor and tell us what matters to them.

**Focus:** a finished Windows editor for **gaming videos** (long and short form), **video essays** and
**documentaries**, edited with Premiere Pro's keyboard and edit rules, with AI that runs locally or with your own key.

## Shipped

### 0.1.2 (13 October 2026)
- All 27 clip blend modes and the Track Matte Key render on the GPU, so preview matches export.
- Every Premiere default tool key works: Rate Stretch (R), Pen (P) on audio volume bands, Type (T) and Audio Gain (G).
- Discord presets encode a second time when the first pass goes over the size limit.
- Auto colour, colour match and match scene work on multicam clips.

### 0.1.1 (13 October 2026)
- File > Export Timeline: send a sequence to Premiere, Resolve or Final Cut (FCP7 XML, FCPXML, OTIO, EDL).
- Hand (H) and Zoom (Z) tools; Render In to Out (Enter) for smooth playback of heavy sections.
- Green-screen keying (Ultra Key) on the GPU.

### 0.1.0 "First Cut" (13 October 2026)
The first release meant for real projects:
- **Editing:** Premiere's shortcuts and edit rules, nested sequences, multicam, keyframes, speed ramps, version history.
- **Interchange:** one-click import from Premiere, Resolve and Final Cut, with a report of what carried over.
- **Gaming:** named game/mic/Discord tracks, high and variable frame rates, a highlight finder, instant replay,
  facecam and Shorts layouts, 12 templates, creator export presets.
- **Essays and documentaries:** local transcription, text-based editing, animated captions with Thai, voice-over,
  speech enhancement, smart search, colour with scopes, privacy redaction.
- **AI your way:** local models or your own API keys, with a cost shown before every cloud call and a "no cloud" lock.

The full list is in the [release notes](release-notes-0.1.md).

## In progress: 0.1.3 (the workbench is yours)
- Workspaces you make yourself: new, rename, duplicate, reorder, hide or delete; any panel in either column (for
  example the Audio Track Mixer beside Effects), and the monitors or the timeline on or off.
- Dock panels where you want them, or in windows of their own.
- One Settings window: appearance with custom colours and theme files, the AI switch and your own model names, a
  left rail you can reorder and trim, and a quieter title bar.
- A time bar under the Program Monitor; drops onto the timeline snap to the clip in front.
- A Tracks toolbar with Premiere's tool groups and labelled Snap / Linked switches; cursors that show what a press
  will do.
- Smoother panel resizing, and a new icon set (Boxicons).

## Next: toward a beta

Make Wabcut safe to trust with real projects:
- Fix what people find in real projects. 0.1 was tested with a demo project and scripted test media, not yet with
  real users' projects.
- Code-signed installers, so Windows SmartScreen stops warning, and automatic updates.
- Measure local AI quality and live cloud providers on real footage.
- Manual screen-reader checks (NVDA) on top of the automated keyboard and accessibility tests.
- Pen tool for video opacity keyframes (today it edits audio volume only; opacity keys are set in Effect Controls).

## Started, parked for after 0.1

Work begun on side branches and held back from 0.1 to keep the release focused:
- Content Credentials (signed C2PA) on export.
- ACES/OCIO colour management and HDR grading. Today HDR captures are tone-mapped to SDR.
- A warp stabilizer.

## Wanted, not scheduled

- macOS and Linux builds. Windows comes first; the Qt-free core's CI is already set up for all three.
- Optical Flow / frame-blended slow motion (today slow motion uses the nearest frame).
- OpenFX video plug-ins (CLAP and VST3 audio plug-ins already work).
- AAF and OMF export; caption file import and broadcast caption formats.
- Generated sound effects and Morph Cut.
- Shared review links and cloud projects (version history, review notes and merge are local today).

## Not planned for now

- Live co-editing.
- A plug-in marketplace.
- A Fusion- or After Effects-class compositor.
- Publishing straight to social platforms.

Something missing that you need? Open an issue and say what you are trying to make.
