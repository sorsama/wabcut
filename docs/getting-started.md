# Getting started with Wabcut 0.1 "First Cut"

This guide has four parts:

- installing Wabcut and the first run;
- a tour of the window;
- four step-by-step workflows:
  - [a long-form gaming video](#a-a-long-form-gaming-video-from-an-obs-recording);
  - [a Short or TikTok](#b-a-short-or-tiktok);
  - [a video essay](#c-a-video-essay);
  - [a documentary](#d-a-documentary);
- [AI with your own key](#ai-with-your-own-key) and [Premiere shortcuts](#keyboard-premiere-shortcuts).

Names in quotes are the exact labels you see in the app. Shortcuts are the Windows defaults.

> **Wabcut 0.1 is Windows only** (Windows 10 22H2 or 11, x64). See the [release notes](release-notes-0.1.md) for what is
> in this release and its known issues.

## Install

**What you need.**
- Windows 10 (22H2) or 11, x64.
- A DirectX 11 GPU. NVIDIA (NVENC/NVDEC), AMD (AMF) and Intel (Quick Sync) hardware decoding and encoding are used when
  present.
- 16 GB of RAM recommended (8 GB minimum; local AI models need more).
- 2 GB of disk for the app, plus room for caches and proxies.

**Two packages, one app:**

| Package | How |
|---|---|
| `wabcut-0.1.0-windows-x64.msi` | Double-click it. It installs **for your user only**, with no administrator rights, to `%LOCALAPPDATA%\Programs\Wabcut` (or a folder you choose). Wabcut appears in the Start menu and in Settings > Apps. The options page has an unticked *Open .wabcut project files with Wabcut* box: tick it to open projects by double-clicking them. |
| `wabcut-0.1.0-windows-x64-portable.zip` | Unzip it anywhere, for example a USB drive, and run `bin\wabcut.exe`. Nothing is installed. |

**SmartScreen.** The 0.1 packages are not code-signed yet, so Windows SmartScreen may say "Windows protected your PC".
Choose **More info**, then **Run anyway**.

**Uninstalling** (Settings > Apps > Wabcut) removes the program files only. Your projects, autosaves, proxies, caches,
downloaded AI models, settings and API keys stay.

Admins can install silently:
`msiexec /i wabcut-0.1.0-windows-x64.msi /qn [INSTALL_ROOT="D:\Apps\Wabcut"] [WABCUT_ASSOCIATE=1]`.

## First run

1. **The Home screen** ("Bring your edit into Wabcut") comes first. It has five cards:

   | Card | What it does |
   |---|---|
   | **"One-click import"** | "Drop a timeline here, or choose a file" from Premiere Pro, DaVinci Resolve or Final Cut Pro: Final Cut Pro XML, FCPXML, OpenTimelineIO, EDL and (experimental) Premiere `.prproj`, nested and multicam sequences included. A report says what was carried, approximated or dropped. |
   | **"Recent projects"** | Your recent projects, and **"Open…"** for others. |
   | **"New gaming video"** | "Drop a game recording here" (or **"Choose a recording…"**). The sequence matches the recording's size and frame rate, and every audio stream gets its own named track. See [A](#a-a-long-form-gaming-video-from-an-obs-recording). |
   | **"New project"** | A name, a location and a sequence preset ("1080p", "4K UHD", "Vertical 9:16", "Square 1:1", "Custom"), then **"Create"**. |
   | **"Learn"** | "Import from other editors", "Keyboard shortcuts" and "What's new". |

   **"Explore the demo project"** (top right) closes Home and opens the demo; Esc does the same.

2. **The welcome** follows, with five short pages. "Skip" or Esc leaves it at any time.
   1. **"Coming from Premiere?"**: Wabcut uses Premiere's keys, Premiere's edits, and your own look. Choose **"Let's go"**.
   2. **"Your keys"**: **"Premiere Pro defaults"**, or **"Import my key set (.kys)…"** for your own Premiere shortcuts.
      In Premiere, choose Edit > Keyboard Shortcuts and save your set; Wabcut reads that `.kys` file.
   3. **"Where you start, and your look"**: the workspace to open in ("Edit", "Color", "Audio" or "Captions"), an
      accent colour, and "Dark" or "Light".
   4. **"AI on your terms"**: every AI feature is optional. Local models run on your computer, and cloud models use your
      own key (see [AI with your own key](#ai-with-your-own-key)).
   5. **"You're set"**: **"Take the tour"**, **"Keyboard Trainer"** or **"Start editing"**.
3. **A short performance check** is offered once. It measures decoding and encoding on your GPU (about ten seconds,
   and nothing leaves the computer) and recommends proxy and preview settings. Choose **"Use these settings"** or
   **"Keep my settings"**. You can run it again from **Settings > "Media & cache…"**.

Everything here can be reopened later from **Help** (bottom of the left rail): "Welcome and setup…", "Take the tour",
"Keyboard Trainer…", "What's new", "Keyboard shortcuts…".

## The window

Wabcut has no menu bar.

**The File menu** opens with **Alt+F** or by clicking the logo, top left. It has:
- "Home", "New Project" (Ctrl+Alt+N), "Open Project…" (Ctrl+O);
- "Save" (Ctrl+S), "Save As…" (Ctrl+Shift+S);
- "Import…" (Ctrl+I, for media and timelines);
- "Relink Offline Media…", "Recover…", "Export Timeline…", "Collect Files…";
- "Settings…" (Ctrl+,), "Keyboard Shortcuts…" (Ctrl+Alt+K);
- "Window" (every panel; a checked one is open in this workspace, choosing a closed one opens it), "Reset Panel
  Layout";
- "Exit" (Ctrl+Q).

**The title bar** has:
- **The sequence pill.** Switches sequences. Its list has "New Sequence…" (Ctrl+N), "Reframe…", "Rough Cut from
  Script…", "Narration from Script…" and "Podcast Workflow…".
- **The command palette** button (**Ctrl+Shift+P**).
- **Undo** (Ctrl+Z) and **Redo** (Ctrl+Shift+Z). Their tooltip names the step.
- **"Save"** and **"Export"** (Ctrl+M). A dot on Save means there are unsaved changes; its tooltip says when the
  project was last autosaved. (The GPU and autosave status are in Settings; Settings > Interface can show them in the
  title bar again.)

**Workspaces.** The tabs in the title bar are your workspaces; none is built in. The first start makes one from the
**Edit** preset. **"+"** after the tabs makes a new one from a preset, or as a copy of the current one:

| Preset | What it lays out | Its key opens (or makes) the first one |
|---|---|---|
| Edit | Library, Source and Program Monitors, Effect Controls, the timeline | Alt+Shift+6 |
| Color | Scopes, Program Monitor, Color, the timeline | Alt+Shift+5 |
| Audio | Audio Tools, Program Monitor, Audio Track Mixer, the timeline with volume lines | Alt+Shift+3 |
| Captions | Transcript, Program Monitor, Caption Style, the timeline | Alt+Shift+4 |
| Export | Program Monitor, the export settings, the timeline | Ctrl+M, the "Export" button |

- **Right-click a tab** for "Rename", "Duplicate", "Move left" / "Move right", "Reset layout" (back to its preset) and
  "Delete workspace" (the last one stays). Double-click a tab to rename it; drag a tab to move it.
- **Each workspace keeps its own layout.** Change it right on the workbench:
  - **Panels sit in groups.** A group with several panels shows them as tabs: click a tab to show its panel, click its
    **×** (or middle-click the tab) to close it. The **×** at the end of a panel's header closes the panel too.
  - **Drag a panel's tab, or a group's grip** (the dotted handle at the end of the header): onto another group's tabs
    or middle to add it there as a tab; near a group's edge to dock beside it (that group gives it half its room); to
    the edge of the window to run along that whole side; outside the panels, or with **Ctrl** held, to give it a
    window of its own. Where it will land lights up while you drag. The pop-out button does the same; closing the
    window docks the panel back.
  - **Closed panels come back** from the left rail, File > "Window", or their focus keys (Shift+1 Library, Shift+2
    Source Monitor, Shift+3 Timeline, Shift+4 Program Monitor, Shift+7 Effects): next to panels like them.
  - **Dragging a gutter** between two groups shares the room between them; a double-click shares it evenly. A guide
    line follows the pointer and the panels resize when you let go (Settings > Interface > "Resize panels live" makes
    them follow the pointer instead).
  - File > "Reset Panel Layout" puts the workspace back the way its preset lays it out.

**The left rail** opens panels:
- "Library" (Shift+1), "Effects" (Shift+7), "Transcript", "Audio Tools", "Essential Sound", **"Callouts"** (titles,
  templates and stickers), **"Highlights"**, "Markers", "Review", "Shorts", "Takes", "Generate".
- Right-click the rail to hide an entry or move it up or down (or use Settings > Interface > "Left rail", which also
  resets it).
- At the bottom: **Help**, and **Settings** (also **Ctrl+,**), one window with these pages:
  - **"General"**: the workspace Wabcut opens in, the native window frame, the save state and how often to autosave;
  - **"Appearance"**: themes (Ultraviolet, Daylight, Graphite, Midnight, Paper, and your own), dark or light, the
    accent, high contrast, reduce motion, the interface size, and **"Custom colours"** for every colour of the
    interface and the clip labels. "Save theme" keeps your colours under a name; "Import…" and "Export…" read and
    write theme files you can share;
  - **"Interface"**: the GPU and save status in the title bar, the left rail, and "Resize panels live";
  - **"Timeline & editing"**: linked selection, the snap line, trim offsets, thumbnails, waveforms and more;
  - **"Playback & performance"**: the GPU and video decoding in use, and "Media & cache…";
  - **"Audio"**: the microphone and voice-over, and the audio plug-ins;
  - **"AI"**: see [AI with your own key](#ai-with-your-own-key);
  - **"Keyboard"**: the shortcut editor and the Keyboard Trainer;
  - **"Plugins & extensions"**.

**The Program Monitor's time bar** sits under its picture: it shows the whole sequence, with its clips, edit points,
markers and the In/Out range. Click or drag it to move the playhead; the wheel steps frames, Ctrl+wheel zooms and
Shift+wheel scrolls.

**The Tracks panel** keeps its tools in the header, in Premiere's groups: a small corner mark on a button means it
holds more tools. Right-click it, or press and hold, to pick another (Ripple holds Rolling, Rate Stretch and Remix;
Slip holds Slide; Hand holds Zoom). **"Snap"** (S) and **"Linked"** are labelled switches at the right end of the
sequence tabs. The pointer shows what a press will do: trim brackets on clip edges (yellow for a ripple trim),
the razor, the pen, and so on. Clips you drop from the Library, the Source Monitor or File Explorer snap to the clip
in front, the playhead and markers while Snap is on. A drop overwrites only where it lands, as in Premiere (hold Ctrl
to insert instead): its picture goes on the track under the pointer and its sound on the matching audio track, or the
next free one, so the clips on the other tracks stay whole. Files dropped from File Explorer are imported first, and
several files go one after another.

### The command palette

**Ctrl+Shift+P** opens the palette ("Type a command, or ask AI…"). Type a few letters of a command's name and press Enter.

- **The form.** Every edit command is there, and a command's parameters appear as a small form:
  - the selected clip and the playhead fill themselves in;
  - optional settings start at their defaults.
- **"Select a clip first".** A command that needs a clip is greyed out with this until you select one.
- **Undo.** Every command is one undo step (Ctrl+Z).
- **The assistant.** The palette's **"Ask AI…"** row opens the **Assistant** panel. Describe an edit in your own words;
  the assistant proposes the commands, you review them, then choose **"Accept"**, **"Refine"** or **"Reject"**.

**The timeline's "Moments" button** (tooltip "Moments: slow-mo, speed ramps and punch-ins at the playhead") holds the
gaming moves: replays, slow motion, speed ramps, punch-ins, shake, dead time and the Shorts/square versions. The same
moves are on a clip's right-click menu under **"Moments"**.

---

## A. A long-form gaming video from an OBS recording

The goal is a 10-20 minute YouTube video at 1440p60 from a long OBS recording with game, mic and Discord audio.

**1. Start from the recording.**
1. On the Home screen, drop the recording on **"New gaming video"** (or **"Choose a recording…"**). MP4, MKV, MOV, FLV
   and TS from OBS, ShadowPlay, Game Bar or any capture tool work.
2. Wabcut reads it and shows:
   - the sequence it will make, matching the recording ("Sequence 2560 × 1440 · 60 fps");
   - one line per audio stream, with a box to leave it out and a **track name** you can change. The first three are
     named "Game", "Mic" and "Discord", OBS's usual order.
3. Choose **"Name"** and **"Location"**, then **"Create"**. The project opens with the recording on its timeline.

- **Frame rates and HDR.**
  - 60, 120, 144 and 240 fps play and export frame for frame.
  - A variable-frame-rate recording ("VFR") gets a steady sequence rate and never drifts.
  - HDR captures are shown and edited tone-mapped to SDR.
- **Big recordings.** 4K, above 60 fps or long ("Long · proxies") recordings get proxies for smooth editing; exports
  use the original.
- **Another sequence size.** To use a different size, make a sequence from a preset: palette (Ctrl+Shift+P) > **"New
  Sequence"** (the one with a **preset** list), for example `1440p60`.

**2. Remove the boring parts.**
1. Click the timeline's **"Moments"** button and choose **"Find dead time"**. This finds loading screens, walking and
   waiting: quiet game audio and little motion for 3 seconds or more. It shows "Measuring…" while it reads the
   recording, then lists what it found. Nothing is changed yet.
2. Choose **"Remove"**. The stretches go in one undo step, keeping a short handle at each end, and markers move with
   the material.

**3. Find the highlights.** Open **"Highlights"** in the left rail.
1. **"Find options"** sets what to listen for:
   - **"Game audio"**: explosions, kills, crowd;
   - **"Your voice"**: you shouting or laughing on the mic track;
   - **"On screen"**: flashes, fast motion, kill feeds;
   - **"Chat"**: **"Import a chat log…"** from a Twitch or YouTube chat replay saved as JSON;
   - **"Stream markers"**: markers you set while streaming.

   It also sets **"How many moments"** and the **"Minimum score"**.
2. Choose **"Find highlights"**. Each moment gets a card with its score and reasons.
   - **A** accepts it and **R** rejects it.
   - **Enter** jumps to the peak and **Space** plays the moment.
3. Choose **"Make highlight reel (N)"**. It makes a new sequence of the accepted moments (when none is accepted, of
   every moment not rejected). **"Reel options"** sets:
   - the **"Order"** ("Best first" or "As it happened");
   - the **"Length"** and the **"Transition"**;
   - **"Music"** from the Library (the cuts land on its beats);
   - **"Slow motion at each peak"**.
4. **"Make a Short"** turns one moment into a vertical Short (see [B](#b-a-short-or-tiktok)).

**4. Rough cut with Premiere's keys.**
- Double-click a clip to open it in the Source Monitor. Mark **I** and **O**, then **,** ("Insert") or **.**
  ("Overwrite").
- Play with **J / K / L**. Cut with **C** ("Razor Tool") or **Ctrl+K** ("Add Edit"). Close gaps with **Q** / **W**
  ("Ripple Trim Previous/Next Edit to Playhead") and **Shift+Delete** ("Ripple Delete").

**5. Streamer voice and game under voice.**
- **Streamer voice.**
  1. Select the Mic clip(s) and open **"Essential Sound"** (left rail).
  2. Choose **"Dialogue"**.
  3. In **"Preset"** pick **"Streamer voice (mic: denoise, gate, EQ, compressor, de-esser, limiter)"**.

  It cleans and levels a gaming mic to -16 LUFS, and every effect stays editable in **"Effect Controls"**.
- **Game under voice.** Ask the Assistant (palette > "Ask AI…") to *"duck the game under my voice"*. The Game track
  dips about 10 dB while you talk and comes back between sentences, as volume keyframes you can edit.

**6. Instant replay, slow motion, punch-ins, shake.** Put the playhead on the moment, select the clip, then use the
**"Moments"** button (or the clip's right-click menu > **"Moments"**). Each move is one undo step and leaves ordinary
keyframes you can edit.

| Move | What it does |
|---|---|
| **"Slow-mo this moment"** | Eases down to 25 % or 50 % around the playhead and back to normal speed. The menus next to it set the speed and how much of the action plays slowly (0.5-2 s). |
| **"Speed ramp in"** | The second before the playhead plays at 300 %, slowing to normal at it. |
| **"Freeze + replay"** | Holds the frame at the playhead, then replays the last 2 seconds at 50 %. |
| **"Fast forward"** | In to Out at 200, 300 or 400 % when both marks are on the clip, else 3 seconds from the playhead. |
| **"Punch in"** | Zooms in to 130 % at the playhead, holds a second and eases back. |
| **"Punch in + hold"** | Zooms in to 130 % at the playhead and stays there. |
| **"Shake"** | A short camera shake at the playhead. |
| **"Instant replay"** | Replays the clip right after itself at half speed, with a REPLAY stinger. |

**7. Callouts and templates.**
1. Open **"Callouts"** in the left rail. **"Gaming"** has 12 animated templates: "Subscribe", "Kill streak", "Lower
   third", "Replay", "Countdown", "Chat bubble", "Emote pop", "Arrow callout", "Circle callout", "Webcam frame", "End
   card" and "Intro".
   - **"Search callouts"** finds one by name.
   - **"Stickers"** holds Lottie animations you import (**"Import Lottie sticker…"**).
2. Drag a card onto the timeline, or select it and press **Enter** (or right-click > **"Add at playhead"**). A graphic
   added without a track goes on the first free track above your footage; nothing is overwritten.
3. Select the graphic and use the panel's **"Edit"** tab to change its text and colours. Thai text works.
   - "Emote pop" takes your own picture (PNG or JPEG).
   - The end card leaves two slots for YouTube's end-screen videos.
4. **"Save as template…"** keeps your own version for **"This project"** or **"My templates"** (every project).

**8. Export for YouTube at 1440p60.**
1. Open the **"Export"** tab (**Ctrl+M**). Under **"For gaming"**, choose **"YouTube 1440p60"**. The card shows the
   estimated file size and export time.
   - "YouTube 1080p60", "YouTube 4K60" and "Twitch upload" are there too.
   - The chips at the top show one destination at a time; **"General"** has the rest (ProRes, audio only...).
   - **"Settings"** shows every option.
2. Under **"Save to"**, set the file (**"Browse…"**), then choose **"Export"**, pinned at the foot of the panel.
3. Exports run in the background. **"Add to queue"** lines up more; they show under **"Queue"**.

---

## B. A Short or TikTok

The goal is a vertical 1080x1920 clip at 60 fps with the gameplay, your facecam and captions.

**1. Make the vertical version.** There are three ways:
- **From the whole edit:** **"Moments"** > **"Make Shorts version"**.
  - This makes a new 1080 × 1920 copy of the sequence; your 16:9 edit stays as it was.
  - When there is a facecam, the gameplay goes on top and the facecam below. A facecam is a track named like
    "Facecam", "Webcam", "Cam" or "Camera", or a facecam layout you made with **"Layout…"**.
  - Without a facecam, the game view goes over a blurred copy of itself.
  - Captions move into the vertical safe area, above the platforms' buttons.
  - **"Make square version"** makes a 1080 × 1080 copy.
- **From one highlight:** in **"Highlights"**, select a moment and choose **"Make a Short"**. **"Short options"** set
  the **"Layout"** ("Auto", "Split" or "Blurred fill"), the **"Facecam"**, **"Captions"** and a **"Hook title"** (the
  moment's title over the first seconds).
- **By hand with the layout picker:**
  1. Right-click the gameplay clip and choose **"Layout…"**.
  2. Pick **"Facecam corner"** (your webcam in a corner, with "Shape", "Corner", "Facecam size", "Border" and
     "Shadow"), **"Vertical split"** or **"Vertical blur fill"**.
  3. Choose the **"Facecam source"**: a **"Clip"** (the webcam clip on a track above the gameplay) or a **"Region"**.
     For a region, choose **"Draw on the monitor"** and drag a box around the webcam inside the capture.
  4. Choose **"Apply"** (one undo step).

  The vertical layouts belong on a 9:16 sequence. On a wide one, **"Create a Shorts sequence"** makes a new 1080 × 1920
  sequence at 60 fps: put the gameplay on it, then open **"Layout…"** again.

**2. Captions.**
1. Open the **"Captions"** tab, or the **"Transcript"** panel's "Captions" tab, and choose **"Generate captions"**. This
   needs a transcript; see [C. A video essay](#c-a-video-essay).
2. In **"Caption Style"**, pick the **"Shorts"** preset. **"Animation"** offers "Pop", "Fade" and "Word by word".
3. Thai captions shape correctly. The **"Thai-friendly"** preset uses a Thai font.

**3. Export.** In the **"Export"** tab, under **"For gaming"**:
- **"Shorts / TikTok / Reels"**: 1080 × 1920 at 60 fps. In **"Settings"**, set **"Captions"** to **"Burn in"** so the
  captions are part of the picture.
- **"Discord 10 MB"**, **"Discord 25 MB"** and **"Discord 50 MB"**. The bitrate is set from the length you export to
  land at about 95 % of the limit. If the clip is too long, the card says how much fits: set In and Out around the
  moment.
- The **"GIF"** and **"WebM"** cards say "Coming soon" in 0.1.

---

## C. A video essay

The goal is a narrated essay: voice-over, B-roll, captions, a title, music under the voice, and the right loudness.

**1. Record the voice-over.**
1. Choose your microphone in **Settings > "Audio…"**: **"Microphone"**, **"Test input"**, **"Count-in"**,
   **"Pre-roll"**, **"Post-roll"**, **"Monitor input"**.
2. Arm a track with the microphone button in its track header (or **R** in the **"Audio Track Mixer"**).
3. Put the playhead where the narration starts and click the Program Monitor's microphone button (**"Record
   Voice-over"**).
4. **Space** stops; **Esc** cancels.
5. Recording again over the same place makes a new take. Pick the take in the **"Takes"** panel.

**2. Transcribe and edit by text.**
1. Open the **"Transcript"** panel. In **"Transcribe with"**, choose **"On this computer"** (local whisper.cpp; the model
   downloads once) or a cloud provider you added. Then choose **"Transcribe sequence"** (or **"Selected clips"**).
2. Click a word to jump there.
3. Select words, then **"Delete"** (or the Delete key) or **"Keep only"**. The timeline is cut to match, in one undo
   step.
4. Fix misheard words with **"Fix…"**.
5. **"Clean up"** finds **"Fillers"** (um, uh; Thai อืม, เอ่อ...) and **"Silences over"** a length, with **"Padding
   around cuts"**. Review, then **"Remove"**.

**3. Captions.** In the **"Captions"** tab:
- **"Generate captions"** (later **"Update from transcript"**).
- **"Caption Style"** presets: "Clean", "Bold Box", "Podcast", "Shorts", "Thai-friendly".
- **"Translate…"**, and **"SRT"** / **"VTT"** files.
- You can also burn them in on export.

**4. Title and lower third.**
- The Library's **"Text"** tab has **"New Title"**.
- For an animated lower third, open **"Callouts"** and add **"Lower third"**: a name and a handle, in a box that grows
  with the name. Change the text in the **"Edit"** tab. **"Intro"** makes an opening title.

**5. Music under the voice.**
1. Put the music on its own track.
2. In **"Audio Tools"**, use **"Auto-duck music under dialogue"** (**"Amount"**, **"Attack"**, **"Release"**,
   **"Hold"**). **"Preview ducking"** shows the keyframes first.
   - Or in **"Essential Sound"**, tag the music as music and pick **"Background music (ducked under dialogue)"**.

**6. Loudness.**
- In **"Audio Tools"**, choose **"Analyze loudness"**, then **"Normalize to target"**:
  - **"YouTube"** (-14 LUFS);
  - **"Podcast"** (-16);
  - **"Broadcast (EBU R128)"** (-23);
  - **"Custom"**.
- **"Preview normalization"** first if you like.
- Voice clean-up: **"Essential Sound"** > "Dialogue" presets. Enhance Speech and stem separation run locally.

**7. Export.** In the **"Export"** tab, choose **"YouTube 1080p"**, or **"ProRes 422 HQ"** for a master. Both are
under **"General"**.

---

## D. A documentary

The goal is interviews shot on two or more cameras, edited by text, graded, with private details redacted, delivered as
a file and as a timeline for Premiere or Resolve.

**1. Multicam synced by audio.**
1. In the **"Library"**, select the camera clips (and a separate audio recorder file if you have one).
2. From the item menu choose **"Create Multi-Camera Source Sequence…"**, and set **"Synchronize by"** to **"Audio"**.
   "Timecode", "In points", "Out points" and "Clip markers" also work.
3. Put the multicam source sequence in your edit.
4. Open the Program Monitor's **"Multi-Camera View"** (**Shift+0**) and switch angles live with **1-9** while playing.
5. **"Auto Switch…"** on a multicam clip cuts to whoever is talking.

**2. Edit by text, through the multicam.**
- Transcribe the sequence (see [C](#c-a-video-essay)). The words of a multicam clip come from its active audio angle and
  sit at the right times; nested sequences work the same way.
- Deleting words inside a multicam clip cuts that clip **in place**: the pieces stay multicam clips, so you can still
  switch angles afterwards. Each deletion is one undo step.

**3. Colour.** Open the **"Color"** tab:
- the **"Color"** panel: **"Auto"**, **"Basic Correction"**, **"Creative"**, **"Curves"**, **"Color Wheels"**,
  **"HSL Secondary"**, **"Vignette"**;
- **"Compare & Match"**: **"Set reference"** on a good shot, then **"Match"** (or **"Match scene"**; **"Skin tone
  priority"** for faces);
- **"Scopes"**: **"Luma"**, **"RGB Parade"**, **"YCbCr"**, **"Vectorscope"**.

Auto and Match work on a multicam clip too (0.1.2): they look at the angle it shows, and the grade goes on that clip.
To grade a camera everywhere it is used, grade its angle in the multicam source sequence instead.

**4. Privacy redaction.**
1. Open the clip in the Source Monitor and choose **"Redact…"**.
2. Pick a look ("Blur", "Mosaic" or "Solid") and/or a sound ("Mute" or "Bleep").
3. Use **"Track an object (AI)"** to follow a face or a licence plate.

A redaction belongs to the **source**, so it follows the footage into every sequence, export and nested clip.
**"Redactions of this source"** lists them. Bleeped words also read as bleeped in transcripts and captions, and redacted
regions are never sent to a cloud service in clear.

**5. Export.**
- **A file:** the **"Export"** tab. Choose "YouTube 1080p", "ProRes 422 HQ" (a master) or "Audio only", and **"Captions"**
  set to "Burn in", "SRT file" or "VTT file".
- **A timeline for Premiere, Resolve or Final Cut.** In 0.1 this is done from the command line (there is no button yet):

  ```powershell
  # Premiere Pro: Final Cut Pro 7 XML (File > Import in Premiere)
  & "$env:LOCALAPPDATA\Programs\Wabcut\bin\wabcut-cli.exe" export-timeline "D:\Docs\film.wabcut" "D:\Docs\film.xml" --format fcp7-xml
  # DaVinci Resolve or Final Cut Pro: FCPXML (or --format otio for OpenTimelineIO)
  & "$env:LOCALAPPDATA\Programs\Wabcut\bin\wabcut-cli.exe" export-timeline "D:\Docs\film.wabcut" "D:\Docs\film.fcpxml" --format fcpxml
  ```

  - `--sequence <name>` picks a sequence.
  - The report lists what was carried, approximated or dropped.
  - Nested and multicam sequences go along.
  - The portable ZIP has `wabcut-cli.exe` in its `bin` folder too.

---

## AI with your own key

Every AI feature is optional.

**Local models** run on your GPU/CPU and send nothing anywhere:
- transcription (whisper.cpp);
- Enhance Speech;
- stem separation;
- smart search;
- object masks;
- the highlight finder.

Some download a model the first time; you are asked first.

**Cloud models use your own key.** Wabcut has no account and no middleman.

- **Where keys go.**
  1. Open **Settings** (the gear at the bottom of the left rail, or **Ctrl+,**) > **"AI"**. Its tabs are
     **"Providers and keys"**, **"Models"**, **"Privacy"**, **"Usage"** and **"Local assistant"**.
  2. In **"Providers and keys"**, pick a provider: Anthropic, OpenAI, Google Gemini, Deepgram, ElevenLabs, AssemblyAI, or
     **"Ollama (this computer)"**.
  3. Paste the key in **"Paste API key"**, then choose **"Save and check"**.
     - **"Get a key"** opens the provider's page.
     - **"Data terms"** shows what the provider does with your data.
  4. Keys are stored in the **Windows Credential Manager**, never in project files, and are masked everywhere. Crash
     reports never contain them.
- **A model by name.** **"Assistant model"** (top of Settings > AI) takes a provider and a model id you type, for
  a model newer than Wabcut's list or one your own server runs. Choose **"Use"**; its chip removes it again.
- **Turning AI off.** The **"AI features"** switch at the top of Settings > AI turns cloud AI off for this computer
  (offline mode) and hides the generative tools: the Generate panel, Rough Cut and Narration from Script. Local
  tools such as transcription keep working. Switch it back on and your previous offline choice returns.
- **Consent.** The first time a feature would send something to a provider, Wabcut asks **"Send to <provider>?"**. The
  dialog shows **"What is sent"**, **"Where"**, **"What the provider does with it"** and the **"Price"**. Choose
  **"Allow once"** or **"Always allow"**; **"Cancel"** sends nothing.
- **Cost.** Every cloud call shows its estimated cost first.
  - **"Usage"** shows what you have spent per provider.
  - **"Cap $"** sets a monthly cap per provider. Calls that would pass the cap are refused (0 means no cap).
- **No-cloud lock.** **"Privacy"** has two switches. Neither consent nor "Always allow" can override them:
  - **"Offline mode"**: no cloud calls at all, for this computer;
  - **"Lock this project to no-cloud AI"**: saved in the project, so it holds wherever the project goes.
- **Local assistant.** The **"Local assistant"** tab downloads and starts a local model ("Download", "Start", "Stop").
  The Assistant then works offline.
- **The Assistant.** Palette > **"Ask AI…"**. It proposes ordinary Wabcut commands, previews them, and changes nothing
  until you choose **"Accept"**. Every accepted change is one undo step.

## Keyboard: Premiere shortcuts

Wabcut uses **Premiere Pro's default shortcuts** (Windows and macOS tables, per panel). The ones you will use most:

| Key | Action |
|---|---|
| **Tools** | |
| V | "Selection Tool" |
| C | "Razor Tool" |
| B | "Ripple Edit Tool" |
| N | "Rolling Edit Tool" |
| Y | "Slip Tool" |
| U | "Slide Tool" |
| A | "Track Select Forward Tool" |
| **Playback** | |
| Space | "Play/Stop" |
| J / K / L | "Shuttle Left" / "Shuttle Stop" / "Shuttle Right" |
| Left / Right | step one frame |
| **Marks and markers** | |
| I / O | "Mark In" / "Mark Out" |
| X | "Mark Clip" |
| M | "Add Marker" |
| Shift+M | "Go to Next Marker" |
| **Editing** | |
| , / . | "Insert" / "Overwrite" |
| ; / ' | "Lift" / "Extract" |
| Ctrl+K | "Add Edit" |
| Ctrl+Shift+K | "Add Edit to All Tracks" |
| Q / W | "Ripple Trim Previous Edit to Playhead" / "Ripple Trim Next Edit to Playhead" |
| Shift+Delete | "Ripple Delete" |
| Delete | "Clear" |
| E | "Extend Selected Edit to Playhead" |
| Shift+T | "Trim Edit" |
| Ctrl+D | "Apply Video Transition" |
| Ctrl+R | "Speed/Duration…" |
| F | "Match Frame" |
| **Timeline view** | |
| S | "Snap in Timeline" |
| = / - | "Zoom In" / "Zoom Out" |
| \ | "Zoom to Sequence" |
| Enter | "Render Effects In to Out" (0.1.1: renders previews of In to Out so they play in real time) |
| **Project** | |
| Ctrl+I | "Import…" |
| Ctrl+M | "Export Media…" |
| Ctrl+S | "Save" |
| Ctrl+Z / Ctrl+Shift+Z | "Undo" / "Redo" |

- **Change them.** **"Keyboard Shortcuts"** (**Ctrl+Alt+K**, also in the File menu and Help) shows a keyboard map of what
  every key does.
  - The **key set** list has "Premiere Pro (default)" and "Wabcut", plus "Save as a new key set…".
  - **"Import…"** reads your Premiere **`.kys`** file (or a Wabcut `.json`).
  - **"Export…"** and **"Reset"** are there too.
- **Learn them.** Help > **"Keyboard Trainer…"** drills shortcuts by category: "Editing", "Clipboard", "Markers",
  "Trim", "Playback", "Tools", "Timeline", "Multicam".
- **Render In to Out** (0.1.1). The sequence menu's **"Render In to Out"** (or **Enter**) renders preview files of
  the range between In and Out (the whole sequence without marks), next to the project in "<project> Preview Files".
  The monitor then plays the rendered file there; an edit in that range makes it play live again until you render
  again. A pill in the title bar shows the progress; click it (or press Enter again) to stop.
- **Not active yet.** These keys are kept for later, as in Premiere, but do nothing yet:
  - R, P, H, Z and T (rate stretch, pen, hand, zoom and type tools);
  - G and Shift+G (audio gain and channels).

## Scripts and agents

Every edit in Wabcut is a typed command. You can drive them from outside the app:

- **`wabcut-cli cmd <project> --json '<request>'`** runs commands on a saved project.
- **`wabcut-cli tools`** prints them as tool definitions for an LLM.
- **`wabcut-cli mcp --project <file.wabcut>`** serves them to Claude, Codex and other agents over MCP. It is read-only
  unless you pass `--allow-edit`.

See [commands](commands.md) and [MCP](mcp.md).
