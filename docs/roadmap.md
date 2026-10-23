# Wabcut roadmap

Wabcut is an **alpha**. This page lists what has shipped and what comes next. It is a plan, not a promise: the order
will change as people use the editor and tell us what matters to them.

**Focus:** a finished Windows editor for **gaming videos** (long and short form), **video essays** and
**documentaries**, edited with Premiere Pro's keyboard and edit rules, with AI that runs locally or with your own key.

## Shipped

### 0.3.1 (23 October 2026)
- Save Preset… in Effect Controls' right-click menu works, so a clip's animation can be saved as a preset and exported.

### 0.3.0 (23 October 2026)
Wabcut works more like a desktop app:
- Home is a start window with Home, Projects and Learn pages, one Import for files, folders and timelines, and a New
  project page with Advanced settings that can start from your media.
- Premiere's menu bar (File to Help) in the title bar, in place of the left rail. The Window menu opens panels in the
  group you clicked last; Maximize Frame and a tab menu for every panel.
- The Effects panel in Premiere's folders, with List, Compact and Icons views, presets imported and exported per bin,
  and effects applied to every selected clip.
- Type a timecode to jump, ripple-delete a gap, add and delete tracks from a track's menu, marker names on the ruler,
  and default transitions you choose.
- Export remembers your last choices, Settings has a search, and Wabcut can reopen the last project when it starts.
- One Wabcut at a time, and the window opens where you left it.

The full list is in the [0.3 release notes](release-notes-0.3.md).

### 0.2.0 (22 October 2026)
Most of what Premiere Pro has for gaming videos, video essays and documentaries ([how it compares](premiere-parity.md)):
- Premiere's editing and sequence commands, Group / Ungroup, fade handles, the Pen on video opacity and Selection
  Follows Playhead.
- Premiere's 41 video transitions and about 70 video effects, Optical Flow slow motion, Morph Cut, Warp Stabilizer and
  effect presets.
- Colour management with ACES / OpenColorIO and HDR masters; Content Credentials on exports made on Windows.
- Move, scale and rotate in the Program Monitor, rulers and guides, loop playback and a full-screen output.
- Quick Export, your own presets, alpha and image sequences, and a Media Encoder-style queue with watch folders.
- Submixes, sends, the Audio Clip Mixer and Premiere's audio effects; AAF and OMF for audio post; nine caption file
  formats in and out.
- The Project panel: metadata with XMP, search bins, the freeform view, productions, several open projects, the Media
  Browser with camera cards and Ingest, Photoshop layers and camera RAW.
- AI that reads through multicam and nested clips, automatic face redaction, and sound effects generated with your
  own key.

The full list is in the [0.2 release notes](release-notes-0.2.md).

### 0.1.3 (16 October 2026)
- A new look ("Echo") and a new logo: rounded panels, pill controls, browser-style panel tabs, violet and lime.
- Workspaces you make yourself, and panels you dock as tabs, side by side, along an edge or in their own windows.
- One Settings window: custom colours and theme files, the AI switch and your own model names, a left rail you can
  reorder and trim.
- The Program Monitor works like Premiere's: click to select, double-click to edit text, double-click again for what
  is behind.
- Drag files from File Explorer straight onto the timeline; drops overwrite (Ctrl inserts) and never cut other tracks.
- A redesigned Export page, a time bar under the Program Monitor and a Tracks toolbar with Premiere's tool groups.
- Preview builds for Linux (AppImage) and macOS on Apple silicon.

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

## Next: toward a beta

Make Wabcut safe to trust with real projects:
- Fix what people find in real projects. 0.1 to 0.3 were tested with a demo project and scripted test media, not yet
  with real users' projects.
- Check AAF, OMF and broadcast caption files in Pro Tools, Media Composer and Resolve.
- Code-signed installers, so Windows SmartScreen stops warning, and automatic updates.
- Measure local AI quality and live cloud providers on real footage.
- Manual screen-reader checks (NVDA) on top of the automated keyboard and accessibility tests.

## Wanted, not scheduled

- macOS and Linux beyond the preview builds: local transcription, audio plug-ins and the managed local assistant
  there, testing on real machines, and Intel Macs. Windows comes first.
- OpenFX video plug-ins (CLAP and VST3 audio plug-ins already work).
- Face-aware Morph Cut, and Warp Stabilizer's mesh warp.
- Live references between open projects (items imported from another project are copies today).
- R3D, BRAW and ARRIRAW decoding (each needs a decoder helper built with the camera maker's SDK).
- Shared review links, cloud projects and stock media (version history, review notes and merge are local today).
- VR / 360 video.

## Not planned for now

- Live co-editing.
- A plug-in marketplace.
- A Fusion- or After Effects-class compositor.
- Publishing straight to social platforms.

Something missing that you need? Open an issue and say what you are trying to make.
