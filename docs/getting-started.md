# Getting started with Wabcut

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

> **Wabcut 0.5 is made and tested on Windows** (Windows 10 22H2 or 11, x64); the Linux and macOS downloads are
> previews. See the [release notes](release-notes-0.5.md) for what is in this release and its known issues.

## Install

**What you need.**
- Windows 10 (22H2) or 11, x64 (or Windows 11 on ARM64: [below](#windows-on-arm64-linux-and-macos-preview)).
- A DirectX 11 GPU. NVIDIA (NVENC/NVDEC), AMD (AMF) and Intel (Quick Sync) hardware decoding and encoding are used when
  present.
- 16 GB of RAM recommended (8 GB minimum; local AI models need more).
- 2 GB of disk for the app, plus room for caches and proxies.

**Two packages, one app:**

| Package | How |
|---|---|
| `wabcut-0.7.0-windows-x64.msi` | Double-click it. It installs **for your user only**, with no administrator rights, to `%LOCALAPPDATA%\Programs\Wabcut` (or a folder you choose). Wabcut appears in the Start menu and in Settings > Apps. The options page has an unticked *Open .wabcut project files with Wabcut* box: tick it to open projects by double-clicking them. |
| `wabcut-0.7.0-windows-x64-portable.zip` | Unzip it anywhere, for example a USB drive, and run `bin\wabcut.exe`. Nothing is installed. |

**SmartScreen.** The packages are not code-signed yet, so Windows SmartScreen may say "Windows protected your PC".
Choose **More info**, then **Run anyway**.

**Uninstalling** (Settings > Apps > Wabcut) removes the program files only. Your projects, autosaves, proxies, caches,
downloaded AI models, settings and API keys stay.

Admins can install silently:
`msiexec /i wabcut-0.7.0-windows-x64.msi /qn [INSTALL_ROOT="D:\Apps\Wabcut"] [WABCUT_ASSOCIATE=1]`.

**Updates** (0.5) are off until you turn them on: **Settings > General > Updates > "Check for updates
automatically"** looks once a day at most and says when a new version is out (**"Skip This Version"** keeps it quiet
about that one), and **Help > "Check for Updates…"** looks at once. While a newer version is known (0.6), the **Help**
menu shows a dot and its row names the version; after an update, **What's New** opens once. The check asks GitHub for the latest release and
sends nothing about you or your projects. When Wabcut was installed with the MSI, **"Download and Install"** fetches the
update and checks its signature and checksum; then **"Install When I Quit"** or **"Quit and Install"** installs it
after Wabcut closes (unsaved changes and running exports ask first, as on any quit) and opens Wabcut again.
**"Later"** keeps the download. The portable ZIP opens the release page instead. 0.5.0 is the first version that can
update itself: from 0.4 or older, install it by hand.

### Windows on ARM64, Linux and macOS (preview)

These builds are made and started automatically on GitHub's machines for every release, but nobody has edited a real
project with them yet: expect rough edges and please report what you find on the project's issue tracker. They share the
editor, the Premiere shortcuts (Cmd instead of Ctrl on a Mac), the AI features that use your own key, and smart search.
They do not yet have **local transcription** or **audio plug-ins** (CLAP and VST3), and the **local assistant** is
Windows x64 only.

**Windows on ARM64** (0.7; Snapdragon X and similar). `wabcut-<version>-windows-arm64.msi` or
`-windows-arm64-portable.zip`, used exactly like the x64 packages above. It is a native ARM64 program, not the x64 one
under emulation. Differences: video is decoded by the graphics driver where it offers Direct3D 11 decoding and exported
with the software encoders (there is no NVIDIA, AMD or Intel hardware encoder on these PCs), smart search uses DirectML,
and the update check tells you a version is out and opens the release page instead of installing it (the installer it
could fetch is the x64 one).

**Linux** (x64; Ubuntu 24.04 or a distribution as new; X11 or Wayland). Download the AppImage and run
`chmod +x wabcut-<version>-linux-x64.AppImage && ./wabcut-<version>-linux-x64.AppImage`, or unpack the `.tar.gz` and run
`bin/wabcut`. Qt and the libraries Wabcut needs come with it; the system supplies the graphics drivers, X11 or Wayland,
and the sound server.
- *Window.* Wabcut starts on X11 (through XWayland on a Wayland desktop) and falls back to native Wayland when there is
  no X server; set `QT_QPA_PLATFORM=wayland` to force Wayland.
- *Picture.* Vulkan, then OpenGL, then software drawing. Install your GPU's Mesa or proprietary driver as usual.
- *Hardware video.* Install the VA-API driver for your GPU (`intel-media-va-driver` or `mesa-va-drivers`; NVIDIA uses
  NVDEC/NVENC with its driver): Wabcut then decodes with VA-API and exports with `h264_vaapi` and friends. Without one it
  decodes and encodes in software, only slower. The first-run performance check shows which decoder was picked.
- *Sound.* PulseAudio, which PipeWire provides by default (`pipewire-pulse`), or ALSA, or JACK. If no device opens, Wabcut
  stays silent instead of stopping.
- *Keys.* Your AI provider keys go to the desktop's Secret Service (GNOME Keyring or KWallet). Without one they are kept
  for the session only.
- *Project files.* Packages from a distribution register the `.wabcut` type (Open With); the AppImage and the tar.gz do
  not, but `wabcut project.wabcut` opens a project from any terminal or launcher.

**macOS** (15 or newer, Apple silicon). Open the `.dmg`, drag Wabcut to Applications. When the release is signed and
notarized (the release text says so) it opens like any other app; otherwise the first start is refused once: open
**System Settings > Privacy & Security** and choose **Open Anyway**. Pictures are drawn with Metal, video is decoded and
exported with VideoToolbox (H.264, HEVC, ProRes), sound goes through Core Audio, and your keys live in the Keychain.
Double-clicking a `.wabcut` file, or choosing Open With on a recording, opens it in Wabcut. The first voice-over recording
asks for microphone access.

## First run

1. **The Home screen** comes first. It is a start window with three pages in the bar on the left - **Home**,
   **Projects** and **Learn** - and, along the top, a search over your projects, **"Open…"** (Ctrl+O), **"Import…"**
   (Ctrl+I) and **"New project"** (Ctrl+N); Ctrl+, opens Settings, as in the editor. Nothing on it scrolls the whole
   window; long lists scroll inside their own area.

   | Page | What it has |
   |---|---|
   | **Home** | **"Continue"**: the project you opened last, large, with **"Open"** and **"Show in Explorer"**. **"Start"**: New project, Open project…, Import… and a drop well ("Drop files here": media, game recordings, timelines or projects), **"Import folder…"** and a link to the guide for timelines from Premiere, Resolve or Final Cut. **"Recent"**: your latest projects as cards, and **"All projects"**. With no projects yet, Continue and Recent say so in one line, and Recent points to Learn. |
   | **Projects** | Every recent project as a grid or a list, **All** or **Starred**, sorted by **Last opened** or **Name**; Wabcut remembers your choice. Typing in the search shows this page. Click a project to open it; its **"…"** button, a right-click, the Menu key or Shift+F10 opens its menu: Open, Open Next to This One, Show in Explorer, Copy Path, Star / Unstar, **Locate…** (a project whose file moved), Remove from List and (0.6) **Rename…** (the project's autosaves, backups and render previews take the new name too), **Duplicate…** (a copy next to it), **Versions…** (every saved, autosaved and backup version of it, each openable as a copy) and **Move to Recycle Bin…** (Move to Trash… on macOS: asks first, takes the autosaves, backups and previews along, and **Undo** brings it all back). Rename and Move to Recycle Bin wait until the project is closed, here and in any other Wabcut. The arrows move between projects, Enter opens, Delete removes it from the list (with **Undo** for a few seconds). The **"…"** in the page's toolbar holds **"Clear Unstarred Projects…"**. Starred projects stay even when the list is full. |
   | **Learn** | **"Start here"**: **"Explore the sample project"**, "Take the tour", "Keyboard Trainer", "What's new". **"User guide"**: the guides on the Wabcut website (getting started, the window, coming from Premiere Pro, moving timelines between editors, keyboard shortcuts, gaming videos, Shorts, video essays, documentaries). **"Support"**: release notes, the roadmap and reporting an issue. |

   **"New project"** (or File > New Project, Ctrl+Alt+N) opens the **New project page**: pick a **format** tile
   (Landscape: 1080p at 23.976 / 25 / 29.97 / 30 / 60 fps, 4K UHD, 1440p60, 4K UHD 60; Vertical & square: 9:16, 1:1,
   9:16 at 60 fps; Custom), check the **name** (a free one is filled in: "Untitled Project", "Untitled Project 2"…; names
   Windows keeps for devices - Con, Prn, Aux, Nul, Com1-9, Lpt1-9, with or without an extension - are refused, as are
   renames and duplicates to them) and
   the **location** (**"Browse…"**), then **"Create project"**. **"Advanced settings"** opens the rest: frame size, frame
   rate (up to 240 fps) and drop-frame timecode, pixel aspect, fields, start timecode, audio sample rate, audio master
   (stereo or 5.1), the number of video and audio tracks and their format, colour management and the preview file
   format; **"Reset to defaults"** puts them back. Wabcut remembers your last format and settings for the next project.

   **One Import for everything.** "Import…", "Import folder…" or files dropped anywhere on Home (a folder comes in as a
   bin, its subfolders as bins inside it, as File > Import Folder makes them; hidden files and camera cards are left
   out - import a card from the Media Browser):
   - a Wabcut project opens;
   - a timeline (Final Cut Pro XML, FCPXML, OpenTimelineIO, EDL) becomes a new project with its first sequence open and
     a report of what was carried, approximated or dropped (nested and multicam sequences included);
   - a Premiere `.prproj` first asks whether to use the experimental reader;
   - files Wabcut cannot read (a DaVinci Resolve `.drp`, a Final Cut library, AAF…) say what to export instead, with
     **"How to export"**;
   - media (video, audio, pictures, captions, Lottie JSON, Photoshop) open the New project page in **"Start with
     media"**: the files are listed (remove any with ×), **"Match my media"** is chosen (the first video's size and frame
     rate, as its tile shows), the name is the first file's, and **"Add the clips to the sequence"** puts them on the
     timeline one after another. A game recording with several audio streams also shows the **audio streams**: each on
     its own named track ("Game", "Mic", "Discord"), or left out; the project gets one audio track per stream you keep
     (unless you set the number in Advanced settings). Going to another page and back keeps the page as it was; Cancel
     or Esc drops it. A picture dragged from a web browser says what to do instead (a dotLottie archive imports like Lottie JSON since 0.7).

   Messages (an import that failed, guidance) show in a strip under the top bar; a long one scrolls. A project that
   fails to open (a damaged file) offers **"Recover a version…"** when Wabcut kept autosaves or backups of it: pick one
   and **"Open as copy"**. **"Back to
   <project>"** (or Esc) returns to the project open behind Home, the sample project too when you came to Home from it
   (File > Home; Cancel or Esc on a New project page opened with File > New Project goes back to the editor). When
   Wabcut has just started, the sample project waits behind Home: open it from **Learn > "Explore the sample
   project"**. In **Settings > General**, "When Wabcut starts" can **reopen the last project** instead of showing Home.

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
   **"Keep my settings"**. You can run it again from **Settings > "Media & cache…"**. Its **"Graphics card"** row (0.5)
   names the GPU Wabcut previews, decodes and exports on, and why.

Everything here can be reopened later from the **Help** menu: "Welcome and Setup…", "Take the Tour", "Keyboard
Trainer…", "What's New", "Keyboard Shortcuts…".

**The tour (0.7)** has seven steps, each pointing at a part of the window while the rest dims: the Library, the Timeline,
the menu bar (every command is in a menu, with its shortcut; Window opens a panel as a tab of the group you clicked
last), the Program Monitor (the playback resolution button and clicking the timecode to type a time), the Effects panel
(the step opens it when it is closed or hidden behind another tab), the command palette (Ctrl+Shift+P, ⇧⌘P on macOS: a
card in the middle, no highlight) and Export, where opening the Export workspace finishes it. Right or Return goes on,
Left goes back, Esc or **End tour** stops it.

## The window

**The menu bar** sits in the title bar after the logo, in Premiere's order: **File, Edit, Clip, Sequence, Markers,
Graphics, View, Window, Help**. Click a title (or press **Alt** and its letter: Alt+F, E, C, S, M, G, V, W, H; on
Windows and Linux) to open its menu; while one is open, point at another title to open that one, or use **Left** and
**Right**. If you give one of these Alt+letter keys to a command in Keyboard Shortcuts, the key runs that command and
the menu opens with a click. On macOS there are no Alt (Option) letters for the menus: Option+letter types accented
characters there. Every row shows its shortcut, and rows that cannot run right now are greyed out (no clip selected,
nothing to undo, no In and Out for Lift, no clip in the Source Monitor for Insert); rest the pointer on a greyed row
and a tooltip says why (0.6). A command you choose acts where you were before the menu opened, and whether it can run is judged
there too: Markers > "Mark In" or "Clear In" with the Source Monitor focused works on the source's marks, with the
Timeline focused on the sequence's. The titles are in the Tab order too (Space, Enter or Down opens one).
When the window is too narrow for the titles and the workspace tabs, they fold into one **"Menu"** button with the same
menus inside. The logo opens the File menu as well.

**File:**
- "Home", "New Project…" (Ctrl+Alt+N), "New Sequence…" (Ctrl+N), "New Sequence From Clip";
- "Open Project…" (Ctrl+O), **"Open Recent"** (the last ten projects, and "Clear Recent"), "Open Project Next to This
  One…". A `.wabcut` you double-click in File Explorer while Wabcut runs opens in that window (not in a second
  Wabcut). So does anything else you open with Wabcut (0.7: **Open with** on a recording, a timeline file or several
  files at once): while the editor is shown the media goes into the bin the Library has open and a message says how many
  came in (a timeline file is imported like File > Import does); while Home is shown the New project page starts with
  those files, as when you drop them on Home; a project among them opens. Started from Explorer with Wabcut closed, a
  recording opens Home on the New project page with it. (Explorer's Open with may start Wabcut once per file of a
  selection: with Wabcut running, every file reaches it; with Wabcut closed, launches that start at the same moment may
  each open a window of their own.) `--new-instance` starts a separate Wabcut that takes none of this. Choosing the project you are working in (from Open Recent, the palette or Explorer) only brings it forward:
  it is not read again, so its undo history and unsaved changes stay. "Open Project…", "Import…" or a drop of that
  project's own file does not read it again either (they may ask about unsaved changes first); "Revert" is the way to go
  back to the saved file. A project open next to it comes into the panels;
  one that asks about unsaved changes waits until you have answered the question already on screen;
- "Close Panel" (Ctrl+W: the panel shown in the group you clicked last); **"Close Project"** (Ctrl+Shift+W): with other
  projects open, the current one closes; with only one, it closes and Home shows (the sample project waits behind
  it). Unsaved changes are asked about first;
- "Save" (Ctrl+S), "Save As…" (Ctrl+Shift+S), **"Save a Copy…"** (Ctrl+Alt+S: the project as it is now goes to another
  file; you keep working in this one; ".wabcut" is added unless the name has it already, in any case - a project called
  "Show.WABCUT" saves as "Show.WABCUT", not "Show.WABCUT.wabcut"), **"Save Version…"** and **"Version History…"** (0.7: the
  project's version history lives next to its saved file, so both are greyed out for a project that has not been saved
  - the message says so; "Save Version…" asks for a name and keeps the project as it is now under it, never an undo step;
  "Version History…" opens the Review panel on its Versions page, where a version is renamed, restored, compared or exported;
  they have no keys until you give them one in Keyboard Shortcuts: `file.saveVersion`, `file.versionHistory`. Wabcut also
  keeps a version now and then while you edit: **Settings > General > Projects > Keep versions every** (Off, 2, 5 -
  the default - 10 or 30 minutes; only when something changed), and **File > Recover…**'s "All versions" tab has a
  **"Version History"** button for the open project), **"Save for an Older Version…"** (0.7: a copy for someone who has not updated Wabcut:
  choose **Wabcut 0.6** or **Wabcut 0.2 to 0.5**; the dialog lists what that version has no place for - for 0.6 the
  0.7 additions such as bilingual caption tracks, the new kinetic caption looks, EQ Match curves, LUT folders, custom
  transcription words, track groups, clip notes, locked clips, pinned keyframes and the new 0.7 effects (the effects stay in
  the file as unknown effects that do nothing); for 0.2 to 0.5 also auditions, Master Clip Effects, text strokes and the HDR output settings - and
  the copy is written without them, with everything else unchanged and the project you are working in untouched. Wabcut
  0.1 projects can be opened by this version but a copy for 0.1 is not offered. The command is `file.saveForOlderVersion`,
  with no default key), **"Revert"** (back to the version last saved, after a confirmation: the unsaved
  changes and the undo history go). A project that cannot be opened or saved says why in a message at the bottom
  right (on Home while Home is open); a save that failed offers **"Save As…"** there. Leaving the sample project after
  editing it (opening, creating or importing a project) first asks **"Discard your changes to the sample project?"**:
  "Save As…" keeps them as a project of your own;
- **"Import from Media Browser"** (Ctrl+Alt+I): imports what is selected in the Media Browser (clips, and folders as
  bins); with nothing selected there, or the Media Browser closed, it shows the Media Browser instead;
- "Import…" (Ctrl+I, for media and timelines): the files go into the bin the Library has open, as in Premiere (0.3
  always used the top of the Library);
- **"Import Folder…"**: choose a folder; it comes in as a bin of its name in the bin the Library has open, each
  subfolder as a bin inside it (up to 8 levels deep), with the media files in the bins they were in. Only media come in
  (video, audio, stills, Photoshop files as their merged picture, RAW): timelines, captions, projects, documents,
  hidden files (also those File Explorer hides, such as desktop.ini and Thumbs.db) and links are left out, and so are
  camera cards inside the folder (import those from the Media Browser, which joins their clips). Importing the same
  folder again adds only the files that are new, into the bins it made the first time. The whole import is one step:
  Edit > "Undo Import Folder" removes every item and bin it made. A message says what came in and what was left out
  ("Imported 12 files from Session1 into 3 bins. 2 files were left out (not media)."). The Library's ··· menu has
  **"Import folder…"** too, and folders dropped on the Library from File Explorer come in the same way. A folder dropped
  on the timeline comes in as bins at the top of the Library and is not edited in (the message says so; drag its clips
  to the timeline from there); files dropped with it are edited in as before. (0.7) Files - and a `.wabcut` - dropped on any other place of the
  editor (the Program Monitor, Effect Controls, the title bar, the other panels) are imported or opened the same way: a
  "Drop to import" outline shows while they hover, a message says how many came into the Library, and the Library opens if
  it was closed. Panels in windows of their own do not take drops yet. Import… and Import Folder… open in the
  folder your last import came from (for a folder: the one around it), else the project's folder;
- numbered stills (an image sequence from a renderer, a game's photo mode or a timelapse) come in as one clip from the
  Library's ··· menu, **"Import image sequence…"** (Premiere's Import with "Image Sequence" ticked): choose the first
  file (PNG, JPEG, TIFF, DPX, Targa, OpenEXR or BMP) and the frame rate (29.97 fps by default, Premiere's Indeterminate
  Media Timebase). That file and the ones numbered after it, up to the first gap, play one still per frame. The clip is
  named after its first file; the stills stay where they are (the first one stands for the clip when Wabcut looks for
  offline media). Command: `media.importImageSequence`. CinemaDNG frames (.dng) import the same way and play
  developed into Rec.709. For ProRes RAW and CinemaDNG items, the Library's right-click **Modify > Source Settings…**
  sets the exposure, the ISO (As Shot, or the ISO the clip was shot at and the one to develop it for: a stop per
  doubling, since the file does not carry the camera's own ISO) and the white balance they are developed with (As Shot,
  or the colour temperature and tint of the light to balance for); commands `media.rawSourceSettings`, `media.setRawSourceSettings`.

  A Photoshop file (.psd, .psb) opens **Import Layered File**: Merge All Layers, Merged Layers (the ones you tick),
  Individual Layers (a bin with one clip per layer) or Sequence (those clips stacked on tracks of a sequence of the
  document's size), each at Document Size or Layer Size. RGB, grayscale and CMYK files (converted to RGB) of 8, 16 or
  32 bits import, with their blend modes. The clips read the file each time, so what you save in Photoshop comes
  through. A flattened file (no layers) imports as its picture; a saved selection or spot colour channel in it is not
  transparency, so the still stays opaque. Commands: `media.layeredFileInfo`, `media.importLayeredFile`.

  The **Media Browser** (Window > "Media Browser", Shift+8) browses drives and folders. A camera card (AVCHD, Canon XF,
  P2, XDCAM, XAVC) shows its clips instead of its folders, and a recording the camera split into several files (GoPro
  chapters, an AVCHD span, Canon XF parts) is one clip. Hover a thumbnail to scrub through the clip. Double-click a
  clip to watch it in the Source Monitor without importing it: the first mark, marker or edit you make there imports
  it. Select clips and folders (a click selects, Ctrl-click adds, Space adds the current row) and press **Import**
  ("Import 3" for three rows), or Ctrl+Alt+I: clips come into the current bin, and each folder comes in as a bin with
  its subfolders, as File > Import Folder… does; clips and folders chosen together are one undo step. The list button
  shows the current clip's metadata (format, duration, the video and audio streams). Turn on **Ingest** and open
  **Ingest Settings** beside it to choose what an import does first: **Copy** the clips into a folder (so the card can
  be taken out), **Transcode** them into a folder with an export preset (they import at once and switch to the new files
  when the export queue has made them), **Create Proxies**, or **Copy and Create Proxies** (copies first, then makes the
  proxies of the copies); Ingest applies to clips, not to folders. The folder
  button (**Drives and places**) lists Videos and your home folder, then the game capture folders that exist on this
  computer: **"Game Bar captures"** (Videos\Captures), **"NVIDIA ShadowPlay"** (Videos\NVIDIA) and **"OBS recordings"**
  (each OBS Studio profile's recording folder); then **"Recent: …"** for the folders your last imports came from, then
  the drives (a recent folder that is a drive's root is listed as the drive). The Media Browser opens at the folder it
  showed last, once you show it. Commands: `media.browse`, `media.importCameraClips`.
- "Import Lottie…"; **"Export"**: "Media…" (Ctrl+M), "Frame…", "Send to Export Queue", "Timeline (XML, EDL, OTIO)…";
- "Relink Offline Media…", "Recover…", "Collect Files…";
- "Exit" (Ctrl+Q).

**Edit:** "Undo", "Redo", "Cut", "Copy", "Paste", "Paste Insert", "Paste Attributes…", "Remove Attributes…", "Clear",
"Ripple Delete", "Duplicate", "Select All", **"Select All Disabled Clips"** (0.7), "Deselect All", "Find…",
"Edit Original", "Keyboard Shortcuts…" (Ctrl+Alt+K), "Media and Timeline Preferences…" and **"Settings…"**
(Ctrl+,). "Undo" and "Redo" name the edit they act on on the timeline - "Undo Razor", "Redo Ripple Delete" - as
the toolbar's tooltips do. While you type in a text field (a
search, a name, a title's text), "Undo", "Redo", "Cut", "Copy", "Paste", "Clear" and "Select All" work on its text,
as their keys do there, not on the timeline (Undo and Redo then read just that). Copy a lower third or a graded
clip in one project and paste it into another (0.7): the pictures its graphic shows, an Image Matte Key's picture
and the LUT files its grade uses come along and are found in the other project (by id, else by path) or added to it,
so the pasted clip looks as it did.

**Clip:** "Make Subclip…", "Speed/Duration…", **"Scene Edit Detection…"** (0.5, see
[Premiere's sequence and editing tools](#premieres-sequence-and-editing-tools)), "Audio Gain…", "Audio Channels…",
"Video Options" (frame holds, field options, scale and fill), "Insert", "Overwrite", "Enable", "Link", "Group",
"Ungroup", "Merge Clips…", "Interpret Footage…", "Source Settings…", "Modify Timecode…", "Replace Footage…", "Make
Offline", "Reveal in Project", and the gaming moments "Slow-mo at Playhead" and "Punch-in at Playhead".

**Sequence:** "Sequence Settings…", "Render Effects In to Out", "Render Selection", "Delete Render Files", "Delete
Render Files In to Out", "Match Frame", "Reverse Match Frame", "Add Edit", "Add
Edit to All Tracks", "Trim Edit", the ripple trims to the playhead, the transitions (0.7: **"Remove Transitions from
Selection"** and **"Remove All Transitions"**), "Lift", "Extract", "Close Gap", **"Remove Disabled Clips"** (0.7),
"Join Through Edits", "Replace With Clip", "Make Subsequence", zoom, "Go to Gap", **"Snap in Timeline"**, **"Audio
Scrubbing"** (0.5, Shift+S), "Select Clip at Playhead" and **"Selection Follows Playhead"** (the three switches show a
dot while on).

**Markers:** "Mark In", "Mark Out", "Mark Clip", "Mark Selection", "Go to In" / "Out", the Clear rows, "Add Marker", "Go
to Next" / "Previous Marker", "Clear Selected Marker" and "Clear All Markers", then **"Ripple Sequence Markers"** (0.4,
on by default: the sequence's markers move with insert, extract, ripple delete, ripple trims and transcript cuts, in
the same undo step; a marker inside a removed range moves to the cut; the choice is saved with each sequence).

**Graphics:** "New Layer" (Text, Rectangle, Ellipse; the Callouts panel opens), "Arrange" (Bring to Front, Bring Forward,
Send Backward, Send to Back), "Select Next Layer" and "Select Previous Layer".

**View:** the Program Monitor's "Show Rulers", "Show Guides", "Snap in Program Monitor", "Lock Guides", the guide rows
("Add Guide…", "Clear Guides", "Save Guides as Template…", "Manage Guides…"), "Transparency Grid", "Overlays", "Show
Dropped Frame Indicator", "Show Audio Time Units" and "Full Screen".

**Window:** **"Workspaces"** (switch to one, "New Workspace" from a preset, "Duplicate Workspace", "Rename Workspace…",
"Delete Workspace…", and since 0.6 **"Save Changes to This Workspace"**, **"Save as New Workspace…"**, **"Reset to
<Preset> Preset…"** and **"Lock Workspace"**), **"Maximize Frame"** (Shift+`), **"Reset to Saved Layout"**
(Alt+Shift+0), then every panel in alphabetical order (0.7; the order follows the language of the interface) - History
among them since 0.6 (a dot shows the ones open in this workspace; Generate
is listed while AI is on). Choosing one opens it, or brings
it forward where it is, and puts the keyboard in it.

**Help:** "Wabcut User Guide", "Getting Started", "Coming from Premiere Pro" and "Keyboard Shortcuts Guide" (on
[wabcut.zyphite.com](https://wabcut.zyphite.com/guide)), "Keyboard Shortcuts…", "Welcome and Setup…", "Take the Tour",
"Keyboard Trainer…", "What's New", "Release Notes", **"Check for Updates…"** (0.5, see [Install](#install)), **"Open
the Sample Project"**, **"Report a Bug…"** and "About Wabcut". **Report a Bug…** (0.7; also in the command palette) shows the
system information a bug report needs - Wabcut's version, the operating system, the graphics card and driver, the graphics API
it draws with, the video decoders, Qt's version, the interface size and the number of screens - exactly as it is copied:
no paths, no user name, nothing of your project. **Copy System Info** puts it on the clipboard; **Open on GitHub** opens the
bug report form in your browser with the version and the system information filled in (Wabcut sends nothing itself: you
read, complete and send the report); **Show Crash Reports** opens the folder with the reports of earlier crashes. About
Wabcut has a **Copy** button for the version.

The **command palette** (Ctrl+Shift+P) has these too, plus **"Open panel: …"** for every panel, **"Workspace: …"** for
every workspace and **"Open Recent: …"** for the recent projects.

The palette's edit commands list only what it can run (0.7): a command that needs an effect, a transition or another id it
cannot ask for, and the ones that belong to a panel (the version history, review notes, the export queue, the effects
library), are not listed - agents and the CLI still have them all ([commands.md](commands.md)).

**The title bar** has:
- **The window's own menu and Windows 11 Snap Layouts** (0.7, Windows): right-click an empty part of the title bar (or
  press **Alt+Space**) for the system menu - Restore, Move, Size, Minimize, Maximize, Close, the ones that fit the window's
  state - and hover the maximize button for Windows 11's snap layouts (pick a spot to tile the window). Dragging the title
  bar to a screen edge and double-clicking it already snapped and maximized the window. On Linux and macOS the title bar has
  none of these; **Settings > General > "Use native window frame"** gives the system's own title bar everywhere.
- **A "Read-only" badge** (0.7) beside the project's name when another session has the project open (project locking):
  Save is refused and nothing is autosaved, so Wabcut says so once when it opens, with **"Save As…"**; hover the badge
  for who has it. Save As makes the project yours and the badge goes.
- **The sequence pill.** Switches sequences. Its list has "New Sequence…" (Ctrl+N), "Reframe…", "Rough Cut from
  Script…", "Narration from Script…" and "Podcast Workflow…".
- **The command palette** button (**Ctrl+Shift+P**).
- **Undo** (Ctrl+Z) and **Redo** (Ctrl+Shift+Z). Their tooltip names the step (greyed out: why, "Nothing to undo").
  Since 0.6 every button's tooltip also shows when the keyboard reaches it (a key hides it), and a greyed button's
  tooltip says why it cannot be used.
- **"Save"** and **"Export"** (Ctrl+M). A dot on Save means there are unsaved changes; its tooltip says when the
  project was last autosaved. (The GPU and autosave status are in Settings; Settings > Interface can show them in the
  title bar again.) When autosave cannot write your project (a drive gone, a full disk, no permission), Wabcut says so
  once with **"Save As…"**, the dot turns red and the tooltip gives the reason (it names your project, never the
  temporary file the autosave wrote to); it keeps trying, and the next autosave or save that works clears it (0.4).
- **The Background tasks pill** (0.4, after the sequence pill) shows while exports, Render In to Out, proxies,
  transcription, Highlights, Shorts, Collect Files or an extension's AI command run or wait: "Exporting 38 % · 0:42
  left", "Rendering 60 %", "2 exports waiting", with **+N** when more run (only the percentage when the title bar is
  short of room). When nothing runs, **"1 problem"** with a warning sign means an error message went by while you were
  not looking. Click it (or run **"Background Tasks"** from the command palette) for the popover: every export with its
  own buttons (pause / resume / start, cancel, **Open**, Show) and **Start Queue**, **Clear Finished** and
  **Export Queue…**; a row for each other job with its progress and a stop (proxies: **Settings…**); and **Recent
  messages** - every toast, also those Home's message strip showed in its place (not Home's own open and import
  messages), newest first, with its action (a click on a long one shows all of it). An error toast is never covered
  by a later message: those wait their turn behind it (on Home's message strip too). Errors stay in Recent messages
  however many notes follow.
- **Closing Wabcut while exports or Render In to Out run or wait asks first** (0.4): **Keep Wabcut Open**, **Quit
  When Done** (Wabcut closes by itself once the running work is done) or **Cancel Exports and Quit**. The answer is for
  that close only: if you then cancel at "Save changes?", the next close asks again. When the work ends while Wabcut
  asks, the question goes and Wabcut closes (it stays open when the work failed). A finished
  export's message offers **Open** with **Show in Explorer**; a failed one says why (with **Relink…** when media is
  offline). While an export runs, Windows does not go to sleep on its own and the taskbar button shows the progress
  (Settings > General > Background work).

**Workspaces.** The tabs in the title bar are your workspaces; none is built in. The first start makes one from the
**Edit** preset. **"+"** after the tabs makes a new one from a preset, or as a copy of the current one:

| Preset | What it lays out |
|---|---|
| Edit | Library, Source and Program Monitors, Effect Controls, the timeline with the Audio Meters right of it (0.5) |
| Color | Scopes, Program Monitor, Color, the timeline |
| Audio | Audio Tools, Program Monitor, Audio Track Mixer, the timeline with volume lines |
| Captions | Transcript, Program Monitor, Caption Style, the timeline |
| Export | Program Monitor, the export settings, the timeline |
| Gaming (0.7) | Highlights (with Shorts and the Library as tabs), Program Monitor, Effect Controls (with Essential Sound as a tab), the timeline |

- **Hide the tabs** (0.7): **Window > Workspaces > "Show Workspace Tabs"** (or Settings > Interface > "Workspace tabs")
  takes the tabs out of the title bar and gives their room to the menus and the title. The workspaces stay in Window >
  Workspaces, on their keys (Alt+Shift+1 to 9) and in the command palette; "Rename Workspace…" needs the tabs.

- **Keys (0.7).** **Alt+Shift+1 … Alt+Shift+9** open the workspaces by their place in the tab bar, as Premiere's keys do:
  dragging a tab to another place moves its key. A tab's tooltip and **Window > Workspaces** show its key. With fewer
  tabs than the number, the key keeps its first-run meaning and opens (or makes) a workspace from a preset: 3 Audio,
  4 Captions, 5 Color, the others Edit. **Next Workspace** and **Previous Workspace** (the command palette; no default
  key: assign one in Keyboard Shortcuts) step through the tabs and wrap round the ends. The Export workspace is also
  **Ctrl+M** and the "Export" button.
- **Right-click a tab** for "Rename", "Duplicate", "Move left" / "Move right", "Save Changes to This Workspace",
  "Save as New Workspace…", "Reset to Saved Layout", "Reset to <Preset> Preset…", "Lock Workspace" and "Delete
  workspace" (the last one stays). Double-click a tab to rename it; drag a tab to move it.
- **Each workspace keeps its own layout.** Change it right on the workbench:
  - **Panels sit in groups.** A group with several panels shows them as tabs: click a tab to show its panel, click its
    **×** (or middle-click the tab) to close it. The **×** at the end of a panel's header closes the panel too.
  - **Drag a panel's tab, or a group's grip** (the dotted handle at the end of the header): onto another group's tabs
    or middle to add it there as a tab; near a group's edge to dock beside it (that group gives it half its room); to
    the edge of the window to run along that whole side; outside the panels, or with **Ctrl** held, to give it a
    window of its own. Where it will land lights up while you drag. The pop-out button does the same; closing the
    window docks the panel back.
  - **The group you clicked last is the active one:** a violet outline runs around it (a click anywhere in it, one of
    its tabs, a panel dropped there or a panel's focus key makes a group active). The Window menu opens a closed panel
    there, as a tab; a panel that is open already comes forward where it is (in a narrow window, where the Source and
    Program Monitors share one place, it goes next to panels like it instead). **Ctrl+W** closes the panel the active
    group shows.
  - **Closed panels come back** from the Window menu (into the active group), or their focus keys (Shift+1 Library,
    Shift+2 Source Monitor, Shift+3 Timeline, Shift+4 Program Monitor, Shift+5 Effect Controls, Shift+7 Effects):
    next to panels like them.
  - **Maximize a group** to give it the whole work area: press **`** with the pointer over it, **Shift+`** for the
    active group, double-click one of its tabs, or choose "Maximize Frame" (Window menu, tab menu); the keyboard focus
    goes into it. The same again brings the other panels back, and so does a panel docked back from its own window.
  - **Right-click a panel's tab** for "Close Panel", "Close Other Panels in Group", "Undock Panel" (that panel in a
    window of its own), "Undock Panel Group" ("Dock Panel Group" while it has a window) and "Maximize Frame" /
    "Restore Frame". On a group's tabs the Menu key or Shift+F10 opens it too, and so it does on a lone panel's tab
    (0.4: Tab reaches it). A lone panel's tab drags like a group's.
  - **Crowded groups keep readable tabs** (0.4): only the shown tab keeps room for its **×** (another tab shows it
    under the pointer); tabs shrink but keep their dot and a few letters; the tabs that do not fit go into the **tab
    list** (the **⌄** at the end of the tabs), which lists every panel of the group with its focus key - choose one to
    show it (a tab in the list is dragged once it is shown). Every tab's tooltip gives the panel's full name and its
    focus key ("Library [Shift+1]").
  - **Header tools that do not fit** (0.4) move into **More tools** (the chevron button **›** before the grip; its
    tooltip says "More tools") instead of disappearing: the leading tools go first, the trailing row's last tool (the
    Library's Import) last. Its menu runs the button you choose as a click would (a toggle, such as the Media
    Browser's metadata view, turns on or off); badges, switches and the Timeline's tool palette just hide until there
    is room, and Tab skips what is hidden.
  - **Dragging a gutter** between two groups shares the room between them; a double-click shares it evenly. A guide
    line follows the pointer and the panels resize when you let go (Settings > Interface > "Resize panels live" makes
    them follow the pointer instead).
  - Every change applies at once. **Window > Workspaces > "Save Changes to This Workspace"** (0.6) keeps the layout as
    it is now; **Window > "Reset to Saved Layout"** (Alt+Shift+0) puts the workspace back to the layout you saved
    (none saved: the way its preset lays it out), and the message it shows has **"Undo"**. **"Reset to <Preset>
    Preset…"** asks first, then gives the preset's layout back (your saved layout stays). **"Save as New Workspace…"**
    makes a copy and lets you name it.
  - **"Lock Workspace"** (0.6) keeps every panel where it is: no dragging, closing, undocking or resizing (the grips,
    the tabs' × and the gutters are off, and the workspace tab shows a lock). Opening a panel from the Window menu
    still works. Choose it again to unlock.
  - **Ctrl+F6** / **Ctrl+Shift+F6** (0.6) move the keyboard to the next / previous panel group (the docked ones in
    layout order, then the windows of their own, round). **Ctrl+F** searches the panel you are in when it has a search
    (Effects, Markers, Metadata, Transcript, Callouts, Review), else the Library.

**Panels** open from the **Window** menu: "Library" (Shift+1), "Effects" (Shift+7), "Transcript", "Audio Tools",
"Essential Sound", **"Callouts"** (titles, templates and stickers), **"Highlights"**, "Markers", "Review", "Shorts",
"Takes", "Generate" and the rest (they replace the former left rail).

**The Audio Meters panel** (0.5, Window > **"Audio Meters"**) shows the level of the mix: a bar per channel (two for
stereo, six for a 5.1 master) with its peak hold, a 0 to −60 dB scale, the highest peak as a readout and a clip light on
top that turns red at 0 dB; click it (or press Space) to clear it. Workspaces made or reset from the Edit preset have it
right of the timeline; a workspace saved before keeps its layout until you add it from the Window menu. Like any
panel it can be a tab, get a window of its own or close.

**The Loudness panel** (0.7, Window > **"Loudness"**) opens a slim column between the timeline and the Audio Meters and
shows the loudness of what plays: two bars for the momentary and short-term loudness on the EBU R128 scale around your
target (YouTube −14, Podcast −16, Broadcast −23 LUFS or custom), the integrated loudness, the true peak and the loudness
range. Its **Radar** view draws the short-term loudness round a clock face (one turn of 1 to 60 minutes while you play,
or the whole mix after **Analyze mix**), with the target as a ring and the loudness range shaded. See [audio.md](audio.md).

**The Graph Editor** (0.7, Window > **"Graph Editor"**, or the curve button in Effect Controls' header) draws the animated
parameters of the selected clip as coloured curves in a panel of any size: a legend with an eye per curve, a value graph
and a speed graph, box select, dragging keys in whole frames and in value, Bezier handles, typed Time and Value, the
interpolation buttons and thirty-two easing presets (docs/effects.md, "Graph Editor"). It shows the same keyframes as
Effect Controls' lanes - one selection, one undo step per gesture.

**Settings** (Edit > "Settings…", **Ctrl+,**, or Home's gear) is one window with these pages:
  - **Search** (the field at the top, ready to type when Settings opens): type what you are after ("proxies",
    "dark", "default folder") and pick a result: its page opens on that setting, which lights up for a moment.
    The command palette lists every setting too ("Settings" rows);
  - **"General"**: what Wabcut shows when it starts (**Home**, or **Reopen the last project**), the workspace it opens
    in, the native window frame; **Projects**: the save state, how often to autosave, **Keep versions every** (0.7: how
    often the version history keeps a version while you edit; Off keeps only the ones you save), the default location for new
    projects (Home's New project page starts there; **Automatic** = the last project's folder), how many recent
    projects Home and File > Open Recent show (a lower number hides the older ones until you raise it again; starred
    ones always show), and project locking; the save state says **"Autosave failed"** (red) with the reason while
    autosave cannot write; **Autosaves and backups** (0.7): where a saved project's autosave and its earlier saved
    versions (`<project>.bak1`, ...) are kept - **Next to the project** (how every version before 0.7 did it; the
    default for an install that already existed), the **Auto-Save folder** (a `Wabcut Auto-Save` folder next to the
    project; the default of a new install) or **A folder** you choose (one folder per project below it) - **Backups
    to keep** (0 to 10, default 5) and **Autosave versions to keep** (1 to 10, default 3); File > Recover and
    "Versions…" look in all three places, so changing the setting never hides the copies already written; **Show
    recent projects in the taskbar jump list** (0.7, Windows, on by default): right-click Wabcut's taskbar button for
    your starred and recent projects (a click opens one in the running Wabcut; items you remove from the list stay
    off); **Backup and reset** (0.7): **"Export settings…"** saves your settings, keyboard shortcuts, export and effect
    presets, graphic templates and personal dictionary to one file (API keys are never in it: they stay in the system
    keychain; neither are the cloud locations), **"Import settings…"** shows what such a file holds before it replaces yours
    (untrusted input: names, sizes and secrets are checked, and a file never sets where keys or media go: the AI providers'
    addresses and custom providers, the export watch folders, the cloud locations and any network path are left out and
    counted in the preview), and **"Reset all settings…"** goes back to the defaults, keeping your recent
    projects and AI/update opt-ins if you want; both save a backup file of your current settings first (named in the
    confirmation, `backups/settings-<date>-<time>.json` in Wabcut's settings folder; the newest 20 are kept) and restart
    Wabcut to apply the change (after the unsaved-changes question). Holding **Shift** while Wabcut starts (Windows)
    offers the default settings too; **Background work** (0.4): **"Keep the computer awake while exporting"** (on by default;
    Windows only) - while exports, Render In to Out, proxies, transcription, Highlights, Shorts or Collect Files run,
    Windows does not go to sleep on its own (the screen can still turn off); **Updates** (0.5): **"Check for updates
    automatically"** (off by default) and **"Check for Updates…"** (see [Install](#install));
  - **"Appearance"**: themes (Ultraviolet, Daylight, Graphite, Midnight, Paper, and your own), dark or light, the
    accent, high contrast, reduce motion, the interface size, and **"Custom colours"** for every colour of the
    interface and the clip labels. "Save theme" keeps your colours under a name; "Import…" and "Export…" read and
    write theme files you can share. **Interface size** (0.6: 90, 100, 125 or 150 %) applies when Wabcut restarts:
    **"Restart now"** closes and reopens it with your project (asking about unsaved changes first). Sizes that would
    leave less than 1100 x 700 for the window on your screen are greyed ("Too large for this screen"); 90 % fits a
    1080p laptop at 150 % Windows scaling. The window never opens larger than the screen;
  - **"Interface"**: the GPU and save status in the title bar, the workspace tabs (0.7: hide them for room), and "Resize panels live";
  - **"Timeline & editing"**: linked selection, the snap line, trim offsets, thumbnails, waveforms, **"Play audio while
    scrubbing"** (0.5, on by default; Shift+S turns it on and off) and more;
  - **"Playback & performance"**: the GPU and video decoding in use, and "Media & cache…" (0.6: **"Save proxies"**
    next to the project or in a folder you choose, such as a fast drive, from the next proxy on; the **media cache**
    (thumbnails and waveforms) and **AI models and analysis results** folders, used from the next start with
    **"Restart now"**; each shows the free space on its drive. Files already made stay where they were: clean them from
    the cache rows. **"Background rendering"** (0.7) turns on the rendering of effect-heavy stretches while Wabcut is
    idle and sets when it starts and what it waits for (see Render In to Out below). The **"Render files (this project)"** row shows the size of the project's preview files; its
    **"Clean"** deletes them all. **"Keep the caches within their limits automatically"** (on by default) applies the
    thumbnail and waveform limits shortly after Wabcut starts, after each batch of proxies and soon after a limit
    changes, without opening this dialog; it waits while you play, render or export. Only those two caches have a limit:
    proxies and analysis results may still be used by a project (one open later, or one sharing the proxy folder), so
    nothing deletes them automatically; their **"Clean"** does, when you ask. With the switch off, nothing is deleted
    unless you press a Clean button; opening the dialog only measures. **Clean only ever deletes files Wabcut made**:
    a proxy folder's row counts and cleans only the proxies Wabcut recorded there, never your own videos or other files
    saved in that folder, and never the proxies the open project uses. Clean on a proxy row, on **"Analysis results"**
    and **"Clean regenerable data"** ask first, with how many files and how much space; other projects may still use
    those proxies, and Wabcut makes them again when a project needs them. **"AI masks and chat logs"** (AI object mask
    mattes and imported chat logs) cannot be rebuilt from the media: they have their own row, "Clean regenerable data"
    never deletes them, and their own Clean says so before it deletes them). **"Graphics card"** (0.5)
    chooses the GPU on a computer with two, such as a laptop with an integrated GPU next to an NVIDIA or AMD one:
    **"Automatic"** (the high-performance GPU, unless the Windows Graphics settings choose one for Wabcut; the
    **"Windows settings"** button opens them) or one GPU, for example the integrated one to save battery. The row says
    which GPU is in use and why. A new choice reaches exports at once and the preview when Wabcut restarts;
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

More of the Tracks panel, as in Premiere:

- **Snap** stays the way you left it the next time Wabcut starts (so does the time remapping view).
- **Type a time**: click the timecode left of the ruler and type a time, frames or an offset like `+15` or `-1:00`;
  Enter goes there, Esc cancels ([the monitors](monitors.md#type-a-time-drop-a-clip-read-a-marker) work the same way).
- **Track headers**: right-click one (or press the Menu key on its name) for **"Rename…"**, **"Add Video Track
  Above"** / **"Add Audio Track Below"**, **"Add Tracks…"** (how many of each kind, where, and the audio format),
  **"Delete Track"** (it asks first when the track has clips), **"Delete Empty Tracks"**, **"Sync Lock"** and **"Label"**
  (the track's colour, see "Timeline: gaps, labels and the keyboard menu (0.7)"); an audio
  track's menu also has its channel format. Double-click a header (or press F2 on its name) to rename the track; its
  name shows beside V1 / A1 where there is room. Each is one undo step.
- **Gaps**: click the empty space between two clips to select the gap; **Delete** (while the Tracks panel has the
  focus) or right-click > **"Ripple Delete"** closes it. When another sync-locked track has clips in that time, nothing
  moves and a message says why. 0.7: the same menu has **"Close Gaps on V1"** (every gap of the track you clicked, in one
  undo step) and **"Close Gaps in Sequence"** (Sequence > Close Gap with nothing selected).
- **Transitions**: right-click one for **"Set Duration…"**, its alignment on the cut, **"Replace with"** the default
  transition, **"Set as Default Transition"** and **"Clear"**. The default video and audio transitions (what Ctrl+D,
  Ctrl+Shift+D and Shift+D add) are also in Settings > Timeline & editing.
- **Sources and citations (0.7).** For video essays and documentaries: **Window > Sources** lists the claims of the
  sequence by time with the source behind each. **+** cites a source for a claim: write the claim, choose where it is (at the
  playhead, from In to Out, over the selected clips, or at the words selected in the Transcript tab) and fill in what you know - the address, title, author, publisher, date,
  an archive link, a screenshot (a file name or address) and a note. Leave the title and address out and the claim is
  marked **Needs a source** (a red dot): the panel counts those, **Needs a source** shows only them, and every export names
  them. Click a row to move the playhead to the claim, double-click (or the pencil) to edit it; each change is one undo step.
  The **export** button copies or saves the list as a **YouTube description block** (`0:42 claim - title, author, publisher,
  date address`, ready to paste: YouTube reads lines that start with a time as chapters when the description has enough of them),
  a **numbered list**, **Markdown**, an **APA bibliography** (one entry per distinct source, alphabetical) or a **PDF**; the
  **lower-third** button puts each cited source on screen over its claim (`Source: title - author`, on new "Sources" video
  tracks, as long as the claim or 4 s) - graphics you can restyle like any. A source is a marker of type Source (it ripples with
  the edit and shows on the ruler and in the Markers panel). Commands: `sources.add`, `sources.update`, `sources.remove`,
  `sources.list`, `sources.export` (text), `sources.lowerThirds`. A copy saved for Wabcut 0.6 or earlier keeps them as plain markers
  named after the claim. With words selected in the **Transcript** tab, **+** takes them as the claim and puts it
  where they are spoken (**Transcript line** in the form). Not yet: a bibliography style other than APA.
- **Import markers (0.7).** The Markers panel's **Import** button (next to Export) reads a marker list from another tool or
  a spreadsheet: **"Import markers…"**, **"Import markers at the playhead…"** (the file's times count from the
  playhead) or **"Replace the markers with a file…"**. In the **Sequence** tab the markers join the sequence's, in the
  **Clip** tab they go onto the selected clip's media, in its own time. One undo step; a toast says how many came in and
  how many lines were skipped. A file read twice adds nothing (exact copies are left out). Three shapes are
  recognised from the content: a **CSV** (or tab- or semicolon-separated table) with a header row - Premiere's marker
  export and Wabcut's own read back; `Marker Name` / `Name` / `Title`, `Description` / `Comment` / `Notes`, `In` / `Start` /
  `Time`, `Out`, `Duration`, `Marker Type`, `Color` are known, and without a header the columns are time, name,
  comment; a **text file** with one marker a line (YouTube chapters like `0:00 Intro`, `00:01:23:10 - Name`, tab-separated
  Audacity labels); and **JSON** (an array, or an object with a `markers` array; the `marker.list` command's output
  reads back). A time is `HH:MM:SS:FF` at the sequence's frame rate (the clip's own in the Clip tab), a clock
  `H:MM:SS.fff` or `M:SS`, seconds (`12.5`, `12.5s`) or `1500ms`; they land on the frame grid. Colour names are
  Premiere's eight. Files in Windows-1252 (a spreadsheet's CSV) are read too. Commands `marker.import` and
  `source.importMarkers` (`path`, `format`, `offset`, `replace`) do the same for scripts, `wabcut-cli` and the assistant.
- **Markers** show their names on the ruler (cut short before the next marker); point at one for its comment, time and
  length. 0.6: with the playhead stopped on a marker, **M** opens it instead of adding a second one; **Markers** >
  **"Edit Marker…"** opens the marker at the playhead and **"Add Chapter Marker…"** adds a chapter marker there and
  opens it. Right-click a marker (its flag on the ruler, the Program Monitor's time bar or a row of the Markers panel)
  for **"Edit Marker…"**, **"Marker Colour"**, **"Marker Type"**, **"Go to Marker"** and **"Delete Marker"**.
- **Several markers at once (0.7).** In the Markers panel **Ctrl-click** a row to add it to the selection (or take it out),
  **Shift-click** to select the rows between, **Ctrl+A** for every listed row; on the ruler click a flag (Ctrl-click adds
  it, Shift-click selects the flags between) or **Ctrl-drag** over the ruler to draw a marquee (Ctrl+Shift adds to the
  selection). Then **Delete**, **Ctrl+X**, **Ctrl+C** and **Ctrl+V** act on all of them: Paste puts the copied markers at
  the playhead (clip scope: at the frame the selected clip shows there) with the first one on it and the others keeping
  their distances, and works in another sequence or project too. **Alt+Left / Right** (with Shift: ten frames) moves the
  selection; dragging one selected flag on the ruler moves them all by the same distance, stopping where the first
  reaches 0. A bar under the colour filters counts the selection and has the same actions as buttons, and the
  right-click menu of a marker in the selection acts on the whole selection ("Delete 3 markers"). **Clear Selected
  Marker** (Ctrl+Alt+M) clears the selected markers when there are some, else the marker at the playhead. Each action is one
  undo step. Commands `marker.removeMany`, `marker.moveMany` (`indexes`, `by`) and `marker.duplicate` (`indexes`,
  `offset`) do the same for scripts, `wabcut-cli` and the assistant.
- **Shift-click** (0.6) a track header's eye, mute, solo, sync lock or lock (or its V1 / A1 target chip) to set it on
  every track of that kind at once, in one undo step. The **sync lock** toggle before the lock shows which tracks a
  ripple edit shifts. Ctrl+0 / Ctrl+9 target every video / audio track (again: none), Ctrl+Alt+0 / Ctrl+Alt+9 switch
  every source patch of that kind.
- **Edit points** (0.6): with edit points selected (click a clip's edge, or Shift+T), Ctrl+D and Ctrl+Shift+D put the
  default transition on them, a transition from the Effects panel goes on them, and **E** moves them to the playhead
  with the current trim type. Without edit points, **Shift+Q** / **Shift+W** roll the edit before / after the
  playhead to it and **Ctrl+Alt+Q** / **Ctrl+Alt+W** trim the clip under the playhead to it, leaving a gap (Q and W
  ripple).
- **Out of sync** (0.6): a clip whose linked picture or sound slid against it shows a red tag with the offset in frames
  ("+5", "-12") at the start of its name; right-click > **"Move into Sync"** moves it back, **"Slip into Sync"** keeps
  it where it is and slips its media.
- **Clip mismatch** (0.6): the first clip you drop into an empty sequence with another frame size or frame rate (a
  1440p 60 fps recording in a 1080p 30 fps sequence) asks whether the sequence should take the clip's settings; tick
  **"Always do this"** to stop asking (Settings > Timeline & editing changes it back).
- **Square corners** (0.6): Settings > Appearance > **"Timeline corners"** > Square draws clips, lanes and the track
  headers with square corners, as Premiere does.
- **Clips**: right-click > **"Reveal in Project"** selects the clip's item in the Library and brings the Library
  forward (assign it a key in Keyboard Shortcuts: `clip.revealInProject`); **"Reveal in File Explorer"** shows its
  file. A Library item with an In and Out set in the Source Monitor shows the marked range as a band along its card,
  and dragging it to the timeline edits in just that range.
- **Caption tracks** (0.5) are rows above the video tracks, one per caption track (C1 on top), with each caption as a
  block that shows its first line; the display settings (the sliders button) > Show > **"Caption tracks"** hides them.
  Click a caption to select it (the playhead goes to its start and the Captions page shows it); double-click it to type
  its text in the Captions page. With the Selection tool, drag an edge to trim it or its middle to move it (it snaps
  while Snap is on, stays between its neighbours unless the track's style has **"Overlapping captions"** on, and Esc
  cancels); the Razor splits it, Delete removes it, and a right-click has **"Split at Playhead"**, **"Delete"**,
  **"Edit Text"** and **"Select in Captions"**. Each is one undo step. In the row's header, the **C1** chip targets the
  row for Ctrl+K: with nothing selected, Ctrl+K splits the targeted rows' captions under the playhead along with the
  clips, and a selected caption is split on its own. The eye shows or hides the track.
- **Nested sequences, multicam and merged clips** (0.5) show the waveform of the mix they play, with its gains,
  effects, faders and pan. It is worked out in the background; until it is ready the clip shows the loudest of the
  clips inside it, shaded.

### The command palette

**Ctrl+Shift+P** opens the palette ("Type a command, or ask AI…"). Type a few letters of a command's name and press Enter.

- **The form.** Every edit command is there, and a command's parameters appear as a small form:
  - the selected clip and the playhead fill themselves in;
  - optional settings start at their defaults.
- **"Select a clip first".** A command that needs a clip is greyed out with this until you select one.
- **Undo.** Every command is one undo step (Ctrl+Z).
- **The assistant.** The palette's **"Ask AI…"** row opens the **Assistant** panel. Describe an edit in your own words;
  the assistant proposes the commands, you review them, then choose **"Accept"**, **"Refine"** or **"Reject"**.

**The monitors** work like Premiere's: double-click a clip in the Program Monitor to move, scale and rotate it there;
**Loop**, **Play In to Out**, **Toggle Full Screen** (Ctrl+`) and **Transmit** to a second display (**Video Output
Settings** in the gear: which display, Fit / Fill / 1:1, safe lines, sync offset, Match Refresh Rate); rulers and guides
(Ctrl+R, Ctrl+;); the **Transparency Grid**; and **Export Frame** (Ctrl+Shift+E) in DPX, JPEG, PNG, TIFF, Targa, BMP,
GIF or OpenEXR. The gear in each monitor holds their settings. See [monitors.md](monitors.md).

**The timeline's "Moments" button** (tooltip "Moments: slow-mo, speed ramps and punch-ins at the playhead") holds the
gaming moves: replays, slow motion, speed ramps, punch-ins, shake, dead time and the Shorts/square versions. The same
moves are on a clip's right-click menu under **"Moments"**.

---

## A. A long-form gaming video from an OBS recording

The goal is a 10-20 minute YouTube video at 1440p60 from a long OBS recording with game, mic and Discord audio.

**1. Start from the recording.**
1. Drop the recording anywhere on the Home screen (or **"Import…"**, Ctrl+I). MP4, MKV, MOV, FLV and TS from OBS,
   ShadowPlay, Game Bar or any capture tool work. The New project page opens in **"Start with media"**. Home's
   "Import…" and "Import folder…" open in the folder your last import came from.
2. Wabcut reads it and shows:
   - **"Match my media"** chosen, and the sequence it will make ("2560 × 1440 · 60 fps · 48 kHz stereo · …");
   - the recording's size, frame rate, length and codec, with chips for what it needs (4K, high fps, long: proxies,
     VFR, HDR);
   - one line per audio stream, with a switch to leave it out, a **track name** you can change and the track it lands
     on ("→ A1"). The names come from the stream titles your capture tool wrote, the same way later recordings dropped
     on the timeline find their tracks: "Mic/Aux" becomes **Mic**, "Desktop Audio" **Game**, "Discord" stays
     **Discord**, other titles are used as written, and a title that differs from its name shows in quotes beside it.
     Streams without titles are **Game** and **Mic** when there are two (ShadowPlay, Game Bar) and **Track 1**,
     **Track 2**, ... when there are three or more. OBS often records the full mix on Track 1: when the first of three or
     more streams has no title, a hint suggests leaving it out if the other tracks hold the game and your mic, so
     nothing plays twice. Each track also gets the role its name implies (Mic and Discord dialogue, Game effects, Music
     music), so Essential Sound and "Game under my voice" ducking work at once.
3. Check the **name** and **location**, then **"Create project"**. The project opens with the recording on its
   timeline.

- **Chapter markers** (0.5). Marks you set while recording with OBS Studio's **Add Chapter Marker** hotkey (and the
  chapters other apps write into MP4 and MKV files) come in as cyan clip markers on the recording, named after the
  chapter, whichever way you import it; a message says how many. **"Media and Timeline Preferences…"** > **"Media"** >
  **"Chapter markers in recordings become clip markers"** turns this off.

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
   waiting: quiet game audio and little motion. The **"Dead time"** review opens over the Highlights panel (its scissors
   button opens it too, and so does **"Review…"** in Moments) and shows "Measuring…" while it reads the recording.
   Nothing is changed yet. **"Sensitivity"**, **"Shortest stretch"**, **"Keep at each end"** and **"Only In to Out"**
   set what counts; after a change, choose **"Find again"**.
2. Each stretch gets a row with a picture from it, its time, its length and a switch: on means it goes. The switched-on
   stretches show as red bands on the timeline. Click a row (or press Enter) to jump there; **Up** and **Down** move,
   **K** keeps a stretch, **Delete** removes it, **Space** flips it, and **"All"** and **"None"** set every row.
3. Choose **"Remove N stretches"**. Only the switched-on stretches go, on every unlocked track, in one undo step,
   keeping the handle at each end, and markers move with the material. If the clips changed since the find, the review
   asks you to find again first.

**3. Find the highlights.** Open **"Highlights"** from the Window menu.
1. **"Find options"** sets what to listen for:
   - **"Game audio"**: explosions, kills, crowd;
   - **"Your voice"**: you shouting or laughing on the mic track;
   - **"On screen"**: flashes, fast motion, kill feeds;
   - **"Chat"**: **"Import a chat log…"** from a Twitch or YouTube chat replay saved as JSON;
   - **"Stream markers"**: markers you set while streaming.

   It also sets **"How many moments"** and the **"Minimum score"**.
2. Choose **"Find highlights"**. Each moment gets a card with its score and reasons.
   - **A** accepts it and **R** rejects it. A ripple edit before a moment (Ripple Sequence Markers moves its marker)
     keeps its accept or reject.
   - **Enter** jumps to the peak and **Space** plays the moment.
3. Choose **"Make highlight reel (N)"**. It makes a new sequence of the accepted moments (when none is accepted, of
   every moment not rejected). **"Reel options"** sets:
   - the **"Order"** ("Best first" or "As it happened");
   - the **"Length"** and the **"Transition"**;
   - **"Music"** from the Library (the cuts land on its beats);
   - **"Slow motion at each peak"**.
4. **"Make a Short"** turns one moment into a vertical Short (see [B](#b-a-short-or-tiktok)); its tooltip says about
   how long it will be. **"Make Shorts (N)"** makes a vertical Short of every accepted moment (when none is accepted,
   of every moment not rejected) in one go, with the same **Short options**. It runs in the background ("Short 2 of
   6"); **"Cancel"** stops after the Short being made and keeps the ones already made. The Shorts are one undo step
   ("Make Highlight Shorts") and the timeline stays on your recording. Then **"Export all (N)"** queues each of them for
   export at 1080x1920 (H.264, or **"HEVC"** with its switch on).

**4. Rough cut with Premiere's keys.**
- Double-click a clip to open it in the Source Monitor. Mark **I** and **O**, then **,** ("Insert") or **.**
  ("Overwrite").
- Play with **J / K / L**. Cut with **C** ("Razor Tool") or **Ctrl+K** ("Add Edit"). Close gaps with **Q** / **W**
  ("Ripple Trim Previous/Next Edit to Playhead") and **Shift+Delete** ("Ripple Delete").
- **Hear where you are** (0.5). While playback is stopped, dragging the playhead in the ruler or on the Program
  Monitor's time bar, and stepping frames with the arrow keys, play a short piece of the sound there (dragging back
  plays it backwards); the Source Monitor does the same with its clip. **Shift+S** ("Toggle Audio Scrubbing") turns it
  off and on again.

**5. Streamer voice and game under voice.**
- **Streamer voice.**
  1. Select the Mic clip(s) and open **"Essential Sound"** (Window menu).
  2. Choose **"Dialogue"**.
  3. In **"Preset"** pick **"Streamer voice (mic: denoise, gate, EQ, compressor, de-esser, limiter)"**.

  It cleans and levels a gaming mic to -16 LUFS, and every effect stays editable in **"Effect Controls"**.
- **Game under voice.** Open **"Audio Tools"** (the Audio workspace, or the Window menu). In the **"Auto-duck"** card
  choose the **"Game"** tab (Game under my voice; it is chosen for you when a track is named for the game, as Start with media names
  them). A line says which tracks it uses ("Mic → ducks Game, Music"): the voice is a track named Mic, Microphone,
  Voice or Commentary (else a dialogue-role track), the game a track named Game, Desktop Audio or Gameplay (else an
  effects-role track), and with **"Include music"** on the music tracks dip too. Each mode keeps its own amount,
  attack, release and hold (the game: -10 dB, 0.12 / 0.8 / 0.35 s). **"Preview ducking"** shows the planned volume keys
  on the timeline as dashed ghosts; **"Apply"** makes them one undo step ("Duck Game Under Voice"). The Game track dips
  about 10 dB while you talk and comes back between sentences, as volume keyframes you can edit. You can also ask the
  Assistant (palette > "Ask AI…") to *"duck the game under my voice"*.

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
| **"Instant replay"** | Replays In to Out when both marks are on the clip (every clip of the media in that range, linked audio and facecam too), else the 5 seconds before the playhead on a clip longer than 20 seconds, else the whole clip, right after itself. Under the button: the speed (**"25 %"** or **"50 %"**), **"Sound"** (the replay plays its sound too, slowed) and **"Stinger"** (the REPLAY title), remembered for next time and used by the clip menu's "Instant Replay" too. |

**7. Callouts and templates.**
1. Open **"Callouts"** from the Window menu. **"Gaming"** has 12 animated templates: "Subscribe", "Kill streak", "Lower
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
   - **"Check before export…"** (0.7) lists what is likely to go wrong first: offline media, gaps, flash frames, mixed
     frame rates, a size that does not suit the sequence, captions, loudness. Each row has **"Go to"** and, where Wabcut
     can, a fix. **"Export"** runs the same check and shows the list (with **"Export anyway"**) when it finds a problem
     or a warning; the switch is in the list.
   - **"Queue…"** > **"History"** (0.7) keeps the last 100 finished exports with **"Show"**, **"Open"** and
     **"Export Again"** (the same preset and settings on the project as it is now).
   - **"Smart Render"** (Settings > Render) copies untouched ProRes / DNx frames, and from 0.7 whole GOPs of H.264 and
     HEVC sources that match the export, so a cut-only edit of a file Wabcut exported before is not encoded again.

---

## B. A Short or TikTok

The goal is a vertical 1080x1920 clip at 60 fps with the gameplay, your facecam and captions.

**1. Make the vertical version.** There are three ways:
- **From the whole edit:** **"Moments"** > **"Make Shorts version"**.
  - This makes a new 1080 × 1920 copy of the sequence; your 16:9 edit stays as it was.
  - When there is a facecam, the gameplay goes on top and the facecam below. A facecam is a track named like
    "Facecam", "Webcam", "Cam" or "Camera", a facecam layout you made with **"Layout…"**, or the facecam region you
    drew in the layout picker for this recording (when there is no facecam track). The split and the facecam's side are
    the ones the layout picker applied last.
  - Without a facecam, Auto Reframe keeps the action in the vertical frame (it finds the subjects of each recording
    first; that analysis is kept, so a second version is quick).
  - It runs in the background: the Moments popover shows each step ("Finding the subject 1 of 2 · 40 %") with
    **"Cancel"**, and a toast follows it when you started it from the menus. Nothing changes until it is done, and the
    version is one undo step. If you edit the project meanwhile, it stops and asks you to run it again.
  - Captions move into the vertical safe area, above the platforms' buttons.
  - **"Make square version"** makes a 1080 × 1080 copy.
- **From one highlight:** in **"Highlights"**, select a moment and choose **"Make a Short"** (or **"Make Shorts (N)"**
  for every accepted moment at once). **"Short options"** set:
  - the **"Layout"** ("Auto", "Split" or "Blurred fill") and the **"Facecam"**;
  - the **"Length"**: **"Lead-in"** (the build-up before the moment, 3 s by default), **"Tail"** (the reaction after
    it, 1 s) and **"At least"** (Off, 15, 30, 45 or 60 s: a shorter moment gets the missing time, two thirds before
    and one third after, at most 30 s a side; what one side cannot take goes to the other);
  - **"Captions"** and their **"Caption style"** ("Shorts" by default: big outlined words two thirds down, above the
    platforms' buttons; or "Clean", "Bold Box", "Podcast", "Thai-friendly" or one of your project's presets);
  - a **"Hook title"** (the moment's title over the first seconds).

  These options are remembered, and **"Make a Short from In to Out"** uses them too.
- **By hand with the layout picker:**
  1. Right-click the gameplay clip and choose **"Layout…"**.
  2. Pick **"Facecam corner"** (your webcam in a corner, with "Shape", "Corner", "Facecam size", "Border" and
     "Shadow"), **"Vertical split"** or **"Vertical blur fill"**.
  3. Choose the **"Facecam source"**: a **"Clip"** (the webcam clip on a track above the gameplay) or a **"Region"**.
     For a region, choose **"Draw on the monitor"** and drag a box around the webcam inside the capture. The region is
     remembered for that recording: it comes back whenever the same file is the gameplay, in any project.
  4. Choose **"Apply"** (one undo step). The picker opens with these options next time; **"Reset"** puts the defaults
     back.
  If no gameplay clip is selected, the picker says so once and offers **"Use the clip under the playhead"** (or the
  nearest video clip). On a wide sequence a vertical layout shows a hint under the cards, and **"Create a Shorts
  sequence"** becomes the main button. Controls that cannot be used say why in their tooltip (Apply, the facecam
  clip). The picker fits the smallest window (it scrolls when the window is short) and opens beside the Program Monitor
  when there is room.

  The vertical layouts belong on a 9:16 sequence. On a wide one, **"Create a Shorts sequence"** makes a new 1080 × 1920
  sequence at 60 fps: put the gameplay on it, then open **"Layout…"** again.

**2. Captions.**
1. Open the **"Captions"** tab, or the **"Transcript"** panel's "Captions" tab, and choose **"Generate captions"**. This
   needs a transcript; see [C. A video essay](#c-a-video-essay).
2. In **"Caption Style"**, pick the **"Shorts"** preset. **"Animation"** offers "Pop", "Fade" and "Word by word".
   Shorts made by Highlights or by the **"Shorts"** panel already have their captions in the style chosen in their
   options (**"Caption style"** there; "Shorts" by default), in short lines of a few words.
3. Thai captions shape correctly. The **"Thai-friendly"** preset uses a Thai font.

**3. Export.** In the **"Export"** tab, under **"For gaming"**:
- **"Shorts / TikTok / Reels"**: 1080 × 1920 at 60 fps. In **"Settings"**, set **"Captions"** to **"Burn in"** so the
  captions are part of the picture.
- **"Discord 10 MB"**, **"Discord 25 MB"** and **"Discord 50 MB"**. The bitrate is set from the length you export to
  land at about 95 % of the limit. If the clip is too long, the card says how much fits: set In and Out around the
  moment.
- The **"GIF"** card makes a looping GIF for chats (480×270, 15 fps, no sound) and the **"WebM"** card a VP9 WebM for
  the web.

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
- The captions also show as a row above the video tracks (0.5), where you trim, move, split and delete them like clips
  (see [the Tracks panel](#the-window)).
- **"Caption Style"** presets: "Clean", "Bold Box", "Podcast", "Shorts", "Thai-friendly".
- **"Translate…"**, and **"SRT"** / **"VTT"** files.
- Caption files from elsewhere (SRT, WebVTT, SCC, MCC, EBU STL, TTML / DFXP, iTT): **File > Import** them or drop them on
  the Library. Each becomes a new caption track of the current sequence, and a report lists what could not be kept
  (styles, exact positions).
- You can also burn them in on export, write them next to the file in any of those formats, or embed them as closed
  captions in the video (H.264 / HEVC).

**4. Title and lower third.**
- **Graphics > New Layer > Text** adds a text layer at the playhead; style it in the **"Essential Graphics"** panel
  (**"Edit"** tab). The command palette's **"New Title"** adds a simple title clip too.
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

**0. Log the footage (0.2).**
- **Metadata.** The **"Metadata"** panel (Window menu) edits the selected clips' **Scene**, **Shot**, **Take**, **Log Note**,
  **Good**, **Client**, **Tape Name**, **Status**, the camera fields (**Camera Angle**, **Label**, **Model**, **Shot
  Date**, **Shot Location**), **Comment** and **Description**, and shows what the file itself says (its XMP and container
  tags). The file's XMP can be edited there too, and **"+"** adds a property it does not have yet. **"Read from file"**
  takes the camera's or another editor's XMP; **"Write XMP"** writes it out. XMP goes into a sidecar `<clip>.xmp` next to
  the file unless you turn on **Preferences > Media > "Write XMP metadata into MP4 and MOV files"**: then those files take
  it themselves (a checked copy replaces the file; if anything fails it stays as it was). In the Library's list view the
  same fields are columns (choose them in the column chooser); a click ticks **Good**. While a cell is open for
  editing, **Tab** / **Shift+Tab** move to the next / previous editable cell (along the row, then on to the next row) and
  **Enter** / **Shift+Enter** to the same column one row down / up, keeping what you typed; **Esc** cancels, and in the Name
  column **Enter** just renames (Premiere's Next / Previous Column Field and Row Field; the keys are the keymap's).
- **Date created, column layouts (0.7).** The list view has a **Date created** column (from the column chooser) that
  shows when a sequence was made; media and bins leave it blank, and sequences from projects saved before it show nothing.
  The chosen columns, their order and widths are remembered; the column chooser's **Saved layouts** keep a named set
  (**Save**, click a name to apply it, the bin icon to delete it) for logging, delivery or review.
- **Where nests go (0.7).** Right-click a bin > **"File Nested Sequences Here"**: **Nest…** then puts "Nested Sequence 01"
  in that bin instead of beside the sequence it came from (one bin at most; choose it again to stop; one undo step each;
  Make Subsequence is not a nest and stays beside its parent).
- **Search Bins.** Type in the Library's search field, then **"Create new search bin"** next to it: the bin keeps
  listing whatever matches (for example Good takes of scene 12), from every bin, as you log more.
- **Freeform view.** The Library's third view: drag thumbnails anywhere, pile alternates into stacks (Ctrl+wheel goes
  through a stack), and keep arrangements with **"Save as New Layout…"** in the canvas menu.
- **Thumbnail size.** In the icon view, the slider under the cards (with its − and + buttons) sizes them from half to
  double, as do Ctrl+wheel, **=** / **-** and **Shift+]** / **Shift+[** while the Library has the focus. **Home**,
  **End**, **Page Up** and **Page Down** move the selection there (Shift extends it), as in Premiere's Project panel.
- **Thumbnails in the list (0.7).** The ··· menu's **"Thumbnails in list view"** puts each media item's and sequence's
  picture (16:9) in the Name column; the rows grow to hold it, and the same slider, Ctrl+wheel and keys that size the
  cards size the rows (twice the height of a text row at 100 %). The choice is saved. Bins keep their folder icon, and
  items without a picture (sound, titles) their kind's icon.
- **Reorganise bins (0.6).** Drag a bin onto another bin, a breadcrumb or a bin tab to move it there (the bin under
  the pointer lights up and a chip shows what moves). Or right-click > **"Move to Bin"** and pick a bin from the tree
  (**"New Bin…"** makes one from the selection), or **Ctrl+X** in the Library (the items dim) and **Ctrl+V** with a bin
  selected, or inside the bin you want; **Escape** forgets the cut. Each move is one undo step.
- **Sizes and properties (0.6).** The list view has **File Size** and (from the column chooser) **Data Rate** columns
  that sort by bytes, and the line under the Library says how many items there are, how many are selected and their
  total size and length. **File > Get Properties for Selection…** (**Ctrl+Shift+H**, or **"Properties…"** on a Library
  item) shows a clip's format, codec and profile, frame rate (variable or not), bit depth, chroma, field order, every
  audio stream with its title, file size, data rate, proxy and how many clips use it; **"Copy all"** copies it as text.
- **Collect Files (0.7).** File > **Collect Files…** copies the project and what it uses into one folder. **Media**: whole
  files, or **Used ranges + handles** with a **Handles** field of 0 to 10 s (steps of half a second, remembered; it
  applies to used ranges only and the size estimate follows it; what is keyed to a file's own time - keyframes, masks, marks,
  redactions and Spectral Repair edits - moves with the cut, so it stays on the same picture and sound). **Folders**: **All in one folder** (the default: every
  file in `Media`), **Project bins** (the Project panel's bins become folders, so the capsule reads like the panel: `Media/
  Interviews/Day 1/...`; items in no bin stay in `Media`) or **Original folders** (the folders the files came from, below
  the folder they all share: `Media/Day 1/Cam A/...`; files on different drives get a folder named after the drive).
  The choice is remembered. Folder names are made valid on Windows, macOS and Linux (no `:` `?` `*`, no reserved device
  names), names that differ only by case share one folder, and nothing can leave `Media`; a path more than 8 folders or
  120 characters deep keeps its last folders and the result says so. **Sequences**: tick the ones to archive (all by
  default; **All** / **None**) - the sequences a chosen one nests or plays come with it, and media only the others use
  stays out, so archiving the final cut does not copy every abandoned version's footage. The manifest lists the
  sequences it holds and the layout.
- **Consolidate Duplicates (0.7).** When the same file was imported more than once, **Edit > Consolidate Duplicates…**
  (also Library ··· > **"Consolidate duplicates…"**) merges each set into one item. The confirmation lists, per set, the
  item that is kept - the one that is online, then the one with most clips, then the oldest - and the ones that go; every
  clip, audition take, graphic image, AI object mask, redaction, subclip, brand logo and provenance that named a removed
  item names the kept one, transcript corrections move over when the kept item has none, its empty comment, description,
  label, metadata and rights are filled from the others, and their markers are added (a marker with the same start and
  name once). Items read differently - another Interpret Footage, timecode, audio channel map, frame scaling, camera RAW
  setting or Master Clip Effects - stay and are listed with the reason. One undo step; the files stay on disk.
  **Edit > Remove Unused…** is in the Edit menu too. Command: `project.consolidateDuplicates` (a dry run shows the plan).
- **Rights and clearance (0.7).** For archival footage, stock and music: right-click a clip > **"Rights and
  Clearance…"**, or **File > Export > Rights and Clearance Log…**. Each source keeps a **status** (cleared, pending, fair
  use, not required), where it came from, its licence and reference number, the territory, the day it expires, what it
  cost (with a currency), the credit line as it must appear, and notes. The log lists the sources the sequence uses in
  the order they first appear (or every file of the project), says how often and where each is used, and flags what needs
  attention on the day the film goes out (**Judge on**: not cleared yet, status never set, licence expired, licence runs
  out within 30 days); a source with no record is the editor's own and is not flagged. Pick rows (click, Ctrl or Shift+
  click, the arrow keys, Ctrl+A) to edit them together: only the fields you change are written, in one undo step.
  **Export log…** writes a spreadsheet (CSV, or JSON) with the totals per status and the cost per currency; **Make credits
  roll** puts a rolling credits graphic of the credit lines of the sources the sequence uses (each distinct line once)
  after the last clip, where it can be edited like any graphic. A subclip uses its master's record. File > Export >
  Media Usage Report… gains the same columns and a **Rights Flags** column. Commands: `media.setRights`,
  `project.rightsLog`, `sequence.creditsRoll`.
- **Channel names of a poly WAV (0.7).** Field recorders name the tracks of a multichannel WAV ("Boom", "Lav A", "Mix L")
  in its iXML chunk (some also in the Broadcast Wave description). Wabcut reads them when the file is imported:
  Properties shows **Audio channels** ("1 Boom · 2 Lav A"), the **Audio Channels** dialog names each source channel
  above its level, and **Split to Mono** names the new clips after the microphones. A mono WAV with a name uses it as the
  stream's name, like an OBS track name. Names are plain text of at most 128 bytes; a file's names that are not valid
  UTF-8 are ignored, and the iXML is read without following anything it refers to.
- **Proxies for many clips (0.6).** Select clips and right-click > **Proxy > "Create proxies (12)"**, or right-click a
  bin > **Proxy > "Create proxies for all media in this bin"**: a toast says how many were queued and how many already
  had one.
- **Offline media (0.6).** Right-click an offline item > **"Link Media…"** (or double-click it, or right-click its clip
  on the timeline) to relink just that one. **"Locate…"** on one file relinks the others too when they moved with it
  (the same folders under a new root, or beside it): "Relinked 1 file and 37 others." A recording remuxed into another
  container (an OBS **.mkv** saved again as **.mp4**) is offered as **"different container (.mkv → .mp4)"** after a
  search or a Locate, and **"Relink these"** links them in one undo step. A timeline import that left media offline
  has **"Relink…"** in its report.
- **Where a clip is used (0.6).** A card shows how many times sequences use it (a badge at the bottom left; a linked
  video and audio clip count once) or a dim **"Unused"** mark. Click the badge, or right-click > **"Find in
  Timeline"**, to list every place (sequence, timecode, tracks): a row opens that sequence with the playhead on the
  clip and the clip selected. In the list view, **Video Usage** and **Audio Usage** columns (from the column chooser)
  count the clips on video and audio tracks and sort by them.
- **Delete media in use (0.6).** Deleting an item (or a bin) that sequences use asks first: "“Hero.mp4” is used by 14
  clips in 3 sequences." **"Delete and Remove Clips"** removes it and those clips (the gaps stay) in one undo step.
  **Ctrl+Delete** (Delete with Options…) always asks; turn off "Remove their clips from sequences" there and
  **"Delete Unused Items"** deletes only the selected items no sequence uses (the used ones stay).
- **Masters of subclips (0.7).** A subclip plays the Master Clip Effects of the item it was made from, so that master
  counts as used while a subclip of it is on a timeline: **Find > Unused** and **Remove Unused** leave it alone. Deleting
  such a master asks first and says how many clips of its subclips would play without its effects.
- **Twirl-down bins (0.6).** In the list view, a bin's arrow (or **Right**) shows what it holds under it, indented;
  **Alt+click** (Alt+Right) opens every bin inside too, and **Left** closes it or goes up to the bin a row is in. Rows
  inside bins select, rename, drag and sort like the others (each level sorted on its own). Wabcut remembers which
  bins were open, per project. A search lists everything flat, as before.
- **Productions.** For a long project split into several (one per episode or reel), the **"Production"** panel makes a
  production folder and new projects in it. Double-click a project to open it next to the ones you have open: they all
  stay loaded with their own undo history and unsaved changes, and a click in **"Open projects"** switches between them
  at once (**File > Close Project** closes the one shown, asking first if it has unsaved changes). Browse another project
  read-only to bring its sequences or bins over: they come in as copies with their media, which is never copied on disk,
  and remember where they came from (**"Reveal in Source Project"** in the Library's item menu; the **link** button
  offers the copies a relink their source project made). **Production Settings** (the gear) choose where every project's
  previews and media cache go (each project gets a folder of its own there, "<name> Preview Files" and a short code of its
  location, so two projects called "Edit" in different folders keep their rendered previews apart). A project a colleague
  has open shows a lock and opens read-only for you. A production
  refuses the names Windows keeps for devices (Con, Prn, Aux, Nul, Com1-9, Lpt1-9, with or without an extension) for a new
  project or folder, as New project does.
- **Search the whole production (0.7).** With a production open, the **"Production"** panel has a search field above its
  project list: type a few words and it finds sequences and footage in **any project of the production**, not only the one
  you are editing. A result shows the item's name, the project it is in, its bin, its size and length, and where your
  words were found (the name, a comment, a tape name, the file path, a marker...). Quote words (`"river at dusk"`) to find
  them together; every word must be found. The row of tabs narrows the search to **Sequences**, **Video**, **Audio** or
  **Stills**, and **"Search in"** limits it to one thing: the name, the comment, the description, the file path, the tape
  name, the log note, the scene, the markers or the bins. Choose a type with no words to list everything of that type.
  Each result has three buttons: **Import into this project** (a copy comes in with what it plays, one undo step, the
  media never copied on disk), **Browse the project read-only** (it opens next to the project you are editing with the item
  pointed at; a result in the project you are editing is shown in the Library) and **Open the project to edit**. **Enter**
  or a double click on a result browses it; **Down** moves from the field into the list, **Esc** clears the field. The
  projects are read in the background, with a progress bar and **Stop**: one you have open is searched as it is now,
  unsaved changes included, and a file that has not changed since your last search is not read again, so a second search is
  quick. A project that cannot be read is named in the summary and the others are still searched.

**1. Multicam synced by audio.**
1. In the **"Library"**, select the camera clips (and a separate audio recorder file if you have one).
2. From the item menu choose **"Create Multi-Camera Source Sequence…"**, and set **"Synchronize by"** to **"Audio"**.
   "Timecode", "In points", "Out points" and "Clip markers" also work.
3. Put the multicam source sequence in your edit.
4. Open the Program Monitor's **"Multi-Camera View"** (**Shift+0**) and switch angles live with **1-9** while playing.
   (0.7) If one camera is a few frames off, right-click its tile (or focus it with Tab and press the Menu key): **Sync 1
   Frame Earlier / Later**, **Sync 10 Frames Earlier / Later**, **Reset to Measured Sync** and **Open Multi-Camera
   Source**. A nudge is one undo step (an angle cannot start before 0: the others move later instead), and a tile that
   is off the sync Wabcut measured wears a "+2 f" badge.
5. **"Auto Switch…"** on a multicam clip cuts to whoever is talking.

**2. Edit by text, through the multicam.**
- Transcribe the sequence (see [C](#c-a-video-essay)). The words of a multicam clip come from its active audio angle and
  sit at the right times; nested sequences work the same way.
- Deleting words inside a multicam clip cuts that clip **in place**: the pieces stay multicam clips, so you can still
  switch angles afterwards. Each deletion is one undo step.
- **Pick soundbites from a source clip** (0.5). In the Transcript panel's **"Transcript"** tab, switch **"Sequence"**
  to **"Source"**: it shows the words of the clip in the Source Monitor, with source timecodes. Click a word to move the
  Source Monitor there; select words (a click, Shift-click, a drag or a match of **"Find in this clip"**) to set the
  source In and Out around them, then **"Insert"** (`,`) or **"Overwrite"** (`.`) edits that soundbite into the
  sequence. **"Copy"** copies the words. A recording with several audio streams shows one stream at a time. A clip with
  no transcript yet offers **"Transcribe this clip"**.

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
  set to "Burn in", "Sidecar file" (then the format: SRT, WebVTT, SCC, MCC, EBU STL, TTML, DFXP, SMPTE-TT or iTT) or
  "Embed".
- **For the sound mix (Pro Tools, Nuendo, Fairlight):** **File > Export Timeline…**, format **AAF** or **OMF**. Wabcut
  renders each clip's audio to new files (with handles), inside the file or in a folder next to it, and keeps clip
  volume, fades, crossfades, track volume and pan, markers (AAF) and the start timecode. The report says what changed.
- **A timeline for Premiere, Resolve or Final Cut.** **File > Export Timeline…**, or from the command line:

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

## Premiere's sequence and editing tools

These work as in Premiere Pro; each is also a command for scripts and agents ([commands.md](commands.md)).

- **Frame size.** Right-click a clip > **"Video Options"** > **"Auto Fit to Frame"** (fits the picture, Scale stays
  100 %), **"Set Scale to Fit Frame"** or **"Set Scale to Fill Frame"** (writes Motion's Scale). Since 0.7 the names say
  what each does; Premiere calls them Scale to Frame Size, Set to Frame Size and Fill Frame (the command palette and
  the shortcut list still find them under those names, and hovering a menu row explains it). **"Nearest Neighbor Scaling"**
  (same menu, also under Clip > Video Options) resizes the clip without smoothing, so pixel art and low-resolution game
  captures keep hard pixel edges when you enlarge them; choose it again to go back to smooth scaling.
  **"Media and Timeline Preferences…"** (Edit menu, or Settings > Timeline & editing) > **"Media"** >
  **"Default Media Scaling"** applies one of them to everything you import.
- **Sequences.** **"Sequence Settings…"** (Sequence menu, or a sequence's Library menu; 0.7: it shows the sequence's **work
  area** too, with **Clear**; Alt+[ and Alt+] set the work area's In and Out at the playhead, and Export's Range can use
  it) changes the frame size, frame rate,
  pixel aspect, fields, audio sample rate, preview format, start timecode and time display, with presets from 23.976 to
  240 fps, vertical 1080x1920 and square 1080x1080. A new frame rate keeps every cut where it was, on the new frames.
  **"New Sequence From Clip"** makes a sequence that matches a clip; **"Duplicate"** copies a sequence or a clip.
  With several clips selected in the Library, **"New Sequence per Clip"** (File menu or right-click) makes one sequence
  for each of them in one step (one undo), and **"New Sequence From All Selected Clips"** makes one sequence that
  matches the first clip and holds them all one after another.
- **Replace.** Shift-drag from the Source Monitor (or Alt/Shift-drag from the Library) onto a clip, or right-click >
  **"Replace With Clip"**: the new clip takes the old one's place and keeps its effects. In the Library: **"Replace
  Footage…"**, **"Make Offline"** and **"Edit Original"** (**Ctrl+E**). When another program re-exported or changed a
  clip's file, **Clip > "Reload Footage"** (or right-click > **"Reload Footage"**) reads it again where it is: the new
  length, streams and frame size show up and the clips, marks, markers and effects stay. Items that share the file reload
  with it, a proxy made of the old file is dropped, and clips that now run past the end of a shorter file are named in the
  message. A file that reads the same as before changes nothing; a clip that lost a stream a clip plays is refused.
- **Timeline.** Right-click > **"Make Subsequence"**, **"Join Through Edits"** (on a razor cut), **"Video Options"** >
  **"Frame Hold Options…"**; the palette has **"Next in Sequence"** / **"Next in Track"** (gaps) and **"Close Gap"**.
- **Scene Edit Detection** (0.5). Select video clips (an exported edit, a clip with several shots), then Clip >
  **"Scene Edit Detection…"** (also in the clip's right-click menu and the palette). Choose **"Apply a cut at each
  detected edit"**, **"Add a marker at each cut"** or both, the **"Detector"** ("Cuts, fades and dissolves", "Hard cuts
  only", "Hard cuts in fast motion", "Fades through black") and the **"Sensitivity"**, then **"Analyze"**. It runs on
  this computer (**"Cancel"** changes nothing); the cuts, with the linked audio, and the markers are one undo step,
  and a message says what it did. Nested and multicam clips are cut but get no markers.
- **Footage.** In the Library: **"Merge Clips…"** (a camera clip with separately recorded sound, synced by In points,
  Out points, timecode, markers or audio), **"Modify"** > **"Interpret Footage…"** (play a 120/144/240 fps capture as
  30 or 60 fps slow motion; pixel aspect, fields, alpha) and **"Timecode…"**.
- **History.** Window menu > **"History"** opens the History panel (0.6: a panel you can dock, tab or float like any
  other; choose it again to close it). It lists the undo steps; click one to go back to it. While it has the focus,
  **Left** and **Right** step one state back or forward, and **Backspace** deletes the current step and every step
  after it; its **"···"** menu has **"Delete"** and **"Clear History"** (every step goes, the project stays as it is).
  Both ask first: they cannot be undone.
- **Preferences > Timeline** sets the default still duration and the default video and audio transition durations.
  **Keyboard Shortcuts > Export…** saves a Premiere `.kys` key set when the file name ends in `.kys`.

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
  1. Open **Settings** (Edit > "Settings…", or **Ctrl+,**) > **"AI"**. Its tabs are
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

## Cloud projects, review links and stock media (0.7)

Keep a project on storage you own (a folder your sync service mirrors, WebDAV, an S3-compatible bucket) and edit it from more than one computer:
**File > Cloud Project…**, then **Sync now**. Send a draft to a client as a web page where they comment on exact frames, and read their comments
back as review notes: **File > Share Review Link…**. Search free videos and photos from Pexels and Pixabay and add one with its licence and credit
recorded: **Window > Stock Media**. Nothing is uploaded before a button that says so, and offline mode stops all of it. All of it, with
what the storage must be able to do, is in [cloud.md](cloud.md).

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
| Shift+S | "Toggle Audio Scrubbing" (0.5: plays the sound under the playhead while you drag it) |
| = / - | "Zoom In" / "Zoom Out" |
| \ | "Zoom to Sequence" (press again to go back) |
| Enter | "Render Effects In to Out" (0.1.1: renders previews of In to Out so they play in real time) |
| **Project** | |
| Ctrl+I | "Import…" |
| Ctrl+M | "Export Media…" |
| Ctrl+S | "Save" |
| Ctrl+Z / Ctrl+Shift+Z | "Undo" / "Redo" |

- **Change them.** **"Keyboard Shortcuts"** (**Ctrl+Alt+K**, also in the Edit and Help menus) shows a keyboard map of what
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
- **Render Selection, Delete Render Files** (0.6). Sequence > **"Render Selection"** renders only the selected video
  clips (no In and Out needed; clips far apart render separately, not the time between them). **"Delete Render
  Files"** deletes the current sequence's preview files, **"Delete Render Files In to Out"** only those that meet In
  to Out (a missing mark counts as the sequence's start or end). Both first ask with the size ("Delete 1.2 GB of
  render files?"); the edit does not change, and those segments turn red or yellow again on the render bar. The rows
  are greyed, with the reason, while there is nothing to render or delete. They have no default key (as in Premiere);
  give them one in Keyboard Shortcuts.
- **Background rendering** (0.7). Wabcut renders the red and yellow stretches of the open sequence by itself while
  nothing else happens, so they turn green without a visit to Render In to Out. It starts after you have not touched
  the keyboard, mouse or pen for 10 seconds (Settings > Media & cache > Background rendering: 5 s, 10 s, 30 s, 1 min or
  5 min), renders one stretch at a time (red ones first, from the playhead on) on half of the processor cores, with Wabcut's
  render, decode and conversion threads at low priority (the encoder's own threads are limited to those cores, not slowed),
  and **pauses at once** when you use the keyboard or mouse, play, drag or trim, start an export or a render of
  your own, or when other programs use the processor (and, on NVIDIA graphics cards, the graphics card) or the
  computer is on battery power ("Wait while on battery power", "Wait while other programs are busy": both on by
  default). It goes on where it stopped when you are idle again; a stretch left paused for two minutes is given up and
  started again later. An edit that changes a stretch's picture makes its render file unused at once (the monitor
  plays live, the bar turns red or yellow) and the stretch is rendered again when you are idle; an edit elsewhere
  keeps it. Colour management changes count: a render file made under another setup is never used. It only renders the
  sequence you have open, never past the render files' size limit (20 GB) or onto a nearly full drive. **Sequence >
  Background Rendering** turns it on or off (the command palette has it too); the title bar's **Background tasks**
  popover shows what it is doing ("3 segments left", "Paused: you used the keyboard or mouse.") with a **Stop** that
  holds it until your next edit. **Delete Render Files** holds it the same way, so the files you deleted are not made
  again behind your back. Linux and macOS do not read the processor, graphics card or battery load yet: input,
  playback, exports and your own renders still pause it there.
- **The other tools.** R (Rate Stretch), P (Pen), H (Hand), Z (Zoom) and T (Type) work as in Premiere, and so do G
  and Shift+G (Audio Gain, Audio Channels).

### Timeline: groups, fades, pen and zoom (0.2)

- **Group / Ungroup** (Premiere's Clip > Group, **Ctrl+G**; Ungroup **Ctrl+Shift+G**; also in a clip's right-click
  menu). Grouped clips can sit on any tracks. Clicking any of them selects the whole group (and the clips linked to its
  members), so the group moves, nudges, copies and deletes together; **Alt-click** selects one clip of the group, as
  Premiere does. A razor cut keeps both pieces in the group; pasting a copied group makes a new group. Ungroup dissolves
  the whole group of the selected clips. Commands: `clip.group`, `clip.ungroup`.
- **Selection Follows Playhead** (Premiere's Sequence menu toggle; no default key: run it from the command palette or
  bind a key). While it is on, moving the playhead selects the clips under it on the targeted tracks (all tracks when
  none is targeted), with their groups and links.
- **Go to Gap.** **Shift+;** moves the playhead to the next gap in the sequence (a stretch where no track has a clip),
  **Ctrl+Shift+;** to the previous one ("Next in Sequence" / "Previous in Sequence"). **Next in Track** and **Previous in
  Track** (0.7, below) also select the gap.
- **Track Select Forward and the markers** (0.7). Drag with the **Track Select Forward Tool (A)** and the sequence
  markers at or after the first clip you move go with it by the same distance, in the move's undo step; with
  **Markers > Ripple Sequence Markers** off they stay. Markers before the first moved clip never move.
- **Clean up before delivery** (0.7). **Sequence > "Remove Transitions from Selection"** deletes the transitions on and
  between the selected clips (fades at a clip's edge too) and **"Remove All Transitions"** every transition of the
  unlocked tracks: the edit points become plain cuts, one undo step. **Edit > "Select All Disabled Clips"** selects every
  disabled clip of the sequence (then Delete, Ripple Delete or Enable act on them) and **Sequence > "Remove Disabled
  Clips"** deletes them in one step, leaving gaps; a disabled picture's enabled sound stays. Locked tracks are left
  alone. Commands: `sequence.removeTransitions`, `sequence.removeDisabledClips` (`ripple` closes the gaps).
- **Select Next Clip / Select Previous Clip** (**Ctrl+Down** / **Ctrl+Up** in the Timeline panel): the clip after / before the
  selected one on its track, with its links and group; with nothing selected, the clip at or after the playhead.
- **Pen on video opacity.** With the **Pen tool (P)** video clips show their **Opacity** as a line (100 % at the top of
  the clip, 0 % at the bottom) with their opacity keyframes, like the volume line on audio clips: click (or Ctrl-click)
  the line to add a keyframe, drag a keyframe to move it in time and level (Shift: level only), drag the line between two
  keyframes to move both, Alt-click a keyframe to switch it between linear and Bezier. Each drag is one undo step.
- **Zoom tool (Z).** Click zooms in at the click, **Alt+click** zooms out, and **dragging a rectangle** zooms so the time
  range you dragged over fills the timeline.
- **Fade handles.** A selected clip shows a small handle in each top corner. Drag the left one into the clip for a
  fade in, the right one for a fade out; the length shows in the timeline's readout while you drag, and dragging back to
  the edge removes the fade. One drag is one undo step. A fade is a one-sided transition inside the clip at that edge: a
  Cross Dissolve from or to black on video, Constant Power from or to silence on audio, so it renders, exports and is
  edited like any transition (Effect Controls, its edges on the timeline, Delete). Command: `clip.setFade` (a length of
  0 removes the fade). An edge that already has a transition with the next clip keeps it.

### Timeline: copy, rearrange and clip facts (0.4)

- **Alt-drag copies a clip**, as in Premiere: drag a clip and hold **Alt** when you drop it. The original stays, the copy
  lands where you drop it (over what is there) with its linked audio or video, and the copies become the selection. One
  undo step, "Duplicate". Press **Alt before clicking** to copy only the video or only the audio of a linked clip; let
  go of Alt before the drop to move just that side instead. With **Alt+Shift** you can drag a copy straight up to V2 at
  the same time (a blur or zoom layer); its audio copy goes along only if the track below is free.
- **Ctrl+Alt-drag rearranges** shots (macOS: ⌘⌥): the gap the clip leaves closes and the clips at the drop move over.
  Drop a shot at the cut after its neighbour to swap the two. Only the tracks of the dragged clips and the tracks you
  drop on change: a music bed or a facecam track below stays whole, and captions and markers stay where they are.
  Shots you select apart (with other shots between them) land side by side at the drop, in time order, and their
  sound stays in sync. If closing the gap would cut a clip you did not drag (a dragged clip's sound runs past its
  picture: an L or J cut), or a clip's linked sound sits apart from its picture, the drag says so and the drop changes
  nothing.
- **Shift while dragging** keeps a clip at its time when you move it to another track (with or without Ctrl or Alt).
- **Clip badges.** A clip that does not play at 100 % shows its speed after the name: **[50%]**, **[−100%]** (reversed),
  **[Variable speed]** (time remapping), **[Hold]**. An **fx** badge says the clip was changed: **violet** when it has
  effects, **amber** when Motion, Opacity, Volume or Pan were changed or keyframed, or the clip is time-remapped.
- **Clip tooltip.** Rest the pointer on a clip for its start, end, duration, speed and source (In and Out), its effects
  and whether it is disabled or offline.
- Turn the speed, the fx badges and the tooltip off in Settings > **"Timeline & editing"** > Tracks, or in the Tracks
  panel's display settings (the sliders button).
- **The playhead snaps** to cuts, markers, In and Out while you drag it in the ruler with Snap on (**S**); **Shift**
  snaps it with Snap off. Settings > "Timeline & editing" > **"Snap the playhead"** turns that off.
- **Playback auto-scroll.** During playback the timeline turns a page when the playhead runs off it (**Page**, as in
  Premiere), keeps the playhead in the middle while the clips scroll (**Smooth**), or does not move (**No scroll**):
  Settings > "Timeline & editing" > **"Playback auto-scroll"**, also in the display settings. Playing backwards (**J**)
  works the same way in the other direction.
- **The ruler menu.** Right-click the timeline ruler for Mark In / Out / Clip, Go to In / Out, Clear In / Out / In and
  Out, Add Marker, Go to Next / Previous Marker, Clear All Markers, Ripple Sequence Markers and Show Audio Time Units.
  The playhead does not move. On a marker's flag the menu also has **"Edit Marker…"** and **"Clear Marker"**.
- **Trim readouts** show the clip's new duration while you drag (Rate Stretch also its new speed; a roll both clips).
- **\\ goes back.** Zoom to Sequence (**\\**) fits the sequence; press it again to go back to your zoom.
- **The clip menu** has **"Clear"** (Delete: the clips go and leave a gap) and **"Audio Gain…"** (G) on clips with
  sound. Its **"Paste"** is the same as Ctrl+V: effects you copied in Effect Controls go onto the selected clips.
- **Track headers.** The speaker is crossed only when a track is muted; mute is amber and solo green, as in the mixer.

### Timeline: navigation, track keys and clip labels (0.7)

- **Pan with the middle button.** Hold the mouse wheel's button and drag on the timeline: it scrolls in both directions
  with any tool and edits nothing. **Hold H or Z** for the **Hand** or **Zoom** tool for as long as the key is down; when
  you let go, the tool you had comes back. A quick tap of H or Z still chooses the tool, as before.
- **Solo a video track.** Video track headers have a solo button (a crosshair) next to the eye. While any video track is
  soloed, only the soloed video tracks show in the Program Monitor and in exports, as only soloed audio tracks are heard.
  Shift-click soloes every video track; the project keeps the solo. MCP: `track.setState` with `solo` works on video tracks.
- **Track keys.** They act on the tracks of the selected clips (a linked picture and sound: both), or on the targeted tracks
  when nothing is selected, in one undo step: **Alt+Shift+O** "Toggle Track Output" (the eye; video), **Alt+Shift+U**
  "Toggle Track Mute" (audio) and **Ctrl+Alt+Shift+S** "Toggle Track Solo" (macOS: Opt for Alt, ⌘ for Ctrl).
  **Alt+Shift+S** "Solo Selected Clip's Track" solos the track of the selected clip alone (the other tracks of its kind
  are unsoloed); press it again to end the solo, to audition one element and come back. They are in the Sequence menu and
  in Keyboard Shortcuts; a key that cannot run says why.
- **A range marker from In and Out.** Markers > **"Add Marker from In/Out"** (**Shift+X**) makes a marker as long as the
  range between the In and Out marks (one undo step). The marks stay. Greyed with "Set In and Out first" until both are set.
- **Overlay, Insert Before, Insert After and Append at End on keys** (Clip menu), for the clip loaded in the Source Monitor, at
  the playhead and the targeted tracks, as the Program Monitor's edit overlay does when you drag: **Ctrl+Alt+O** Overlay (a
  free video track above, or a new one), **Ctrl+Alt+,** Insert Before and **Ctrl+Alt+.** Insert After the clip under the
  playhead (it goes in at that clip's start or end and moves the rest later), **Ctrl+Alt+P** Append at End (macOS: Opt+⌘
  with the same keys). Each is one undo step named after the edit.
- **Snap to.** The Tracks panel's display settings (and Settings > "Timeline & editing") choose what Snap sticks to: **clip
  edges** (cuts, transitions, gaps), the **playhead**, the **In and Out marks**, **markers** and **beats**. All are on by
  default; **S** stays the master switch. Drags, drops, ruler scrubs and caption cue drags follow the choice.
- **Clip notes (0.7).** Right-click a clip > **"Clip Note…"** to write a free-text note on it (Ctrl+Enter or **Save**; **Clear**
  removes it; up to 2,000 bytes). The note belongs to that clip: it is saved with the project, both halves of a razor cut keep
  it and a copy carries it, while other clips of the same file do not have it. With **"Clip notes"** on (Timeline Display and
  Settings > "Timeline & editing"; on by default) the first lines of the note are printed on the clip, at its foot; the whole
  note is in the clip's tooltip and in `clip.details`. One undo step; `clip.setNote` does it for scripts and the assistant.
- **Pacing strip (0.7).** Turn on **"Pacing strip"** (Timeline Display, or Settings > "Timeline & editing") for a thin strip
  between the ruler and the tracks that colours the stretches of the edit where a viewer may get bored: amber when the pace
  eases off, strong amber when it is slow, red when it is likely to drag. It grades what is measured on this computer, with no
  upload and no model: how long the same shot has run without a cut (a cutaway over a longer shot counts as a cut), how
  little moves in the picture (a still screen, a locked-off shot), how long the sound stays quiet and, when the sequence has a
  transcript, how slowly the words come (under 100 a minute). A moment's score adds those up, so two weak reasons count as
  one strong one. Point at a stretch for why, in numbers, and what to try (a cut, a cutaway or B-roll; trim or speed up the
  pause); **click** it to put the In and Out marks around it, **Shift-click** to select the clips that play in it, or right-click
  for **"Add a Marker for This Stretch"**. The footage is measured in the background, one file after another, and the strip fills
  in as each file is done; a file that cannot be measured is left ungraded rather than called silent. Cuts through the same
  media (a through edit) do not count as cuts.
- **Clip duration** (display settings > "Clip duration", off by default) prints each clip's length after its name, in the
  sequence's time format: `Interview [50%] · 00:00:04:12`. **Audio track names** (on by default) shows the track name a
  recording with several audio tracks carries (OBS, ShadowPlay, Game Bar, field recorders) after an audio clip's name:
  `Gameplay · Mic`. A file's single audio track shows nothing extra.
- **Label colours.** Settings > Appearance > **"Label colours"** chooses Echo's bright labels or the softer **Classic** set
  (older Premiere's look, not its exact values); single labels can still be changed under Custom colours, and "Reset labels"
  comes back to the choice.
- **Add tracks when editing clips in** (Settings > "Timeline & editing", on by default, as Premiere 26.3's preference).
  On: an edit that needs a track the sequence lacks adds it (a recording's second audio stream, a paste that spans more
  tracks than exist, an Overlay with no free track). Off: those edits use the tracks that are there, leave out what has
  no track, and say so when nothing fits. Add Track, Add Tracks… and a drag held below the last track still add tracks.
- **Track Select Backward carries markers**, like Track Select Forward: dragging with it moves the markers at or before the
  end of the last clip moved, in the same undo step, while Markers > "Ripple Sequence Markers" is on.

### Timeline: gaps, labels and the keyboard menu (0.7)

- **Go to Gap in a track selects the gap.** Sequence > Go to Gap > **"Next in Track"** / **"Previous in Track"** (also in
  Keyboard Shortcuts and the command palette; no default key) move the playhead to the next or previous gap of the targeted
  tracks (every track when none is targeted) and **select that gap**: Delete, Backspace and Shift+Delete then close it, as
  they do for a gap you clicked, in one undo step. "Next in Sequence" / "Previous in Sequence" (**Shift+;**) only move the
  playhead. Sequence > Gaming > **"Auto Reframe Sequence…"** opens the Reframe dialog.
- **Context menus from the keyboard.** With the focus in the timeline (Tab reaches the lanes and the clip list), the **Menu
  key** or **Shift+F10** opens the right-click menu of what is selected: the selected transition's, else the selected gap's,
  else the clip's. The menu opens where the clip, gap or transition is (the view scrolls to it first). With nothing
  selected it is the menu of the first targeted track's header. In the Timeline list the keys open the clip menu of the
  current row, selecting its clip first. A lone panel's tab opens its tab menu the same way.
- **Named label colours.** A clip's right-click menu names its sixteen label swatches (Premiere's names) and rings the colour
  the selected clips share. **Edit > Label** lists the sixteen names for the selected clips (the shared one is checked), then
  **"Select Label Group"**, which selects every clip of the sequence with the label colour of the selected clips (their
  linked partners too while Linked Selection is on). It changes the selection only: no undo step. The command
  `edit.selectLabelGroup` has no default key; the same row ends the clip menu's swatch area. The Library item menu and the
  sequence tab's Label menu check the current label too.
- **Track label colours.** Right-click a track header > **"Label"** gives the track one of the sixteen label colours (or **"No
  Label"**): a bar down the header's left edge, a faint wash over the track's lane, and the same colour on its mixer strip.
  Use them for telops, B-roll, music or game audio so a busy timeline reads at a glance. One undo step "Track Color", saved in
  the project (older versions ignore it). MCP / CLI: `track.setState` with `colorIndex` (0-15, -1 for none); `sequence.summary`
  lists it.
- **Slip from trim mode (Avid style).** In trim mode **Shift-click the other edge of a clip whose edge is selected** (or press
  **Sequence > "Slip Trim"** with clips selected, or on the clip under the playhead; or choose **Slip** in the Trim view's
  type tabs): both edges of the clip are the edit points and the trim slips its content. The Trim view shows the clip's first
  frame (left) and last frame (right) with their source timecodes and how many frames were slipped; **+1 / +5**, **−1 / −5**,
  Ctrl+arrows and J/K/L dynamic trimming move the source In and Out together (later source frames with +) while the clip keeps
  its place and length; the linked sound slips with the picture while Linked Selection is on. It stops at the media's ends.
  One undo step "Slip" each; Toggle Trim Type goes back to the roll.

### Timeline: insert a copy, lock clips, move tracks, match a nest (0.7)

- **Alt-drag, then Ctrl: insert the copy.** Start a drag with **Alt** (the copy cursor), then press **Ctrl** (macOS: ⌘)
  before you drop: the copy is **inserted** where you drop it, so the clips after it move later (and so do the sync-locked
  tracks), instead of covering what is there. The readout says "Insert Copy", the cursor shows two plus signs, and the
  undo step is "Insert Duplicate". Let go of Ctrl before the drop to get the plain copy again. The order of the keys
  matters, as in Premiere: **Ctrl first and then Alt**, or both from the start, is still the **rearrange** drag above.
- **Show a clip's keyframes.** Click the **fx badge** on a clip: the clip is selected and a menu lists its properties,
  Motion and Opacity on a picture, Volume and Panner on a sound, then the parameters of each effect; a diamond marks the
  ones that have keyframes. Choose one to draw its **value line and keyframes on the clip** (the line is ranged to the
  keys, so a Position or a Blur reads the same way; a property without keys shows a flat line). **Previous / Next
  Keyframe** in the same menu move the playhead between the shown keys, **Hide Clip Keyframes** (or choosing the shown
  property again) removes the line. It is only a view: nothing is edited and there is no undo step; the keys themselves
  are edited in Effect Controls. One property per clip.
- **Lock Clip.** Select clips and choose Clip > **"Lock Clip"** (or the clip's right-click menu; **"Unlock Clip"** when they
  are all locked). A locked clip wears a padlock and **cannot change at all**: not moved, trimmed, split, retimed,
  renamed, deleted, overwritten or pushed by a ripple or an insert (an edit that would says "clip 'Name' is locked" and
  changes nothing, in the lanes as a readout while you drag). It can still be selected, copied (the copy is not locked)
  and played, and the razor leaves it whole. Unlike a locked track, the clips next to it stay editable. Locking a linked
  picture does not lock its sound unless it is selected too (Linked Selection selects both). One undo step; saved with the
  project (older versions open it unlocked). MCP / CLI: `clip.lock` with `clips` and `locked`.
- **Reorder tracks by dragging.** Drag a track header's **name chip** (V1, A2 ...), or the free space of the header around its buttons, up or down: a line shows which header it
  takes the place of, and on release the track moves there, with its clips, links, targeting and effects, in one undo step
  "Move Track". Video stacks upward (a higher track is in front), audio downward. A click on the chip still targets the
  track. The header menu has **"Move Track Up"** and **"Move Track Down"** for the keyboard. A locked track can move
  (its clips are not touched). MCP / CLI: `sequence.moveTrack` with `track` and `position`.
- **Track groups.** Group tracks that belong together (Dialogue, SFX, Music) and work on them as one. Target the tracks (the
  name chips; Shift-click targets every track of a kind) and choose **"Group Targeted Tracks"** in a track header's menu (or
  the palette), or **"New Group from This Track"** and then **"Add to Group"** on the others. The group gets a **header row**
  above its tracks: a chevron that folds the tracks into the row (a collapsed group is that row alone, and its lane shows
  where its tracks have clips; the tracks still play and export), the name (double-click renames it), how many tracks it
  holds, and Mute (video: output), Solo and Lock for **all** of them at once - lit while every track has the switch, ringed
  while only some do. Right-click the row for the group's menu: Rename Group, **Label** (a colour bar on the row and a wash on
  its lane), Collapse / Expand, **Track Height** (the group's tracks at the minimum, the default or large), Mute / Solo /
  Lock / **Target Group**, an audio group's **Output** (the Mix, or **New Submix for This Group**: a submix track named after
  the group, so one fader and one effect stack apply to the whole group - that is how you put a level or effects on a group)
  and **Ungroup Tracks**. A group keeps its tracks next to each other: dragging a track out of the group takes it out, dropping
  one between two of its tracks puts it in, and a track header's menu has **"Remove from Group"**. The palette has **Group
  Targeted Tracks**, **Collapse All Track Groups** and **Expand All Track Groups** (no default keys). In the **Audio Track
  Mixer** a group's strips carry a tag with its name; click it and the strips fold into one group strip (name, number of
  tracks, Mute, Solo, where it goes, **Expand**) - the same collapsed state as the timeline's. Every change is one undo step
  (Group Tracks, Collapse Group, Mute Group ...); groups are saved with the project (older versions open it with the tracks
  ungrouped) and a copy of a sequence keeps them. MCP / CLI: `trackGroup.create`, `trackGroup.set`,
  `trackGroup.setTracksState`, `trackGroup.addTracks`, `trackGroup.removeTracks`, `trackGroup.route` and
  `trackGroup.ungroup`; `sequence.summary` lists the groups.
- **Match Frame into a nest.** With the playhead over a **nested or multi-camera clip**, **F** (Match Frame) opens its source
  sequence in a tab with the playhead on the frame the clip shows there (the breadcrumb leads back), the same as
  double-clicking the clip. On any other clip it loads the media into the Source Monitor as before. `sequence.matchFrame`
  answers a nest with `"nested": true`, its `sequence` and the `sequenceTime`, and stores nothing.

### Auditions: several takes at one cut (0.6)

An **audition** keeps alternatives for the same place in the edit - other performances of a line, two readings of a
voice-over, a music cue or versions of a lower third - and plays the one you pick. It is Final Cut Pro's Auditions and
DaVinci Resolve's Take Selector; Premiere has nothing like it.

- **Make one.** Select the takes on the timeline (stacked on V1, V2, V3 or laid out one after another; their linked
  sound comes along) and choose **Clip > Auditions > "New Audition"** (also in the clip's right-click menu). The
  earliest clip stays where it is and becomes take 1; the others are lined up with its start at their own In points
  and leave the timeline (nothing ripples). Every take needs the same picture and sound clips as the first (a picture
  with its sound, or picture only, or sound only). One clip alone gives **"Duplicate as Audition"**: the clip and a copy
  of it, the copy active, to try another effect on it.
- **Add takes.** **"Add Selected Clips to Audition"** adds the other selected clips; **"Add Source Monitor Clip to
  Audition"** adds the clip open in the Source Monitor, from its In.
- **Pick a take.** The timeline shows **"Take 2 of 4"** after the clip's name (and in its tooltip).
  **Clip > Auditions > "Take Selector"** (also Window > Take Selector) lists the takes of the selected audition - or of
  the audition under the playhead - with their first frame, name, source and length. **Use This Take** (the check
  button, Enter or a double-click) plays it from now on; the playhead does not move. **Play** auditions a take in
  place: the audition's stretch plays once with that take, then the take you had comes back - unless you chose Use This
  Take while it played. Play is only a preview: it adds no undo step, leaves Redo as it was, and saving (or an autosave),
  exporting, rendering or any other command while it plays uses the take you had. The Take Selector keeps that take
  marked Active, and Next Take / Previous Take step from it. **"Next Take"** and **"Previous Take"** step through them; bind keys to them in Keyboard
  Shortcuts. **"Show Takes in Program Monitor"** puts the takes as cards along the bottom of the picture: click one to
  use it.
- **Different lengths.** All takes share the audition's place. A take that runs out of media sooner says how short it
  is ("1:12 short"); while it plays, the rest of the audition stays empty, and choosing a longer take fills it again.
- **Edit it like a clip.** Move, trim, ripple, slip, cut (a razor cut gives both halves every take), copy and paste an
  audition like any clip; every take follows. Effects and Motion you change belong to the take that plays: each take
  keeps its own name, effects and settings. A take can be a nested sequence or a multicam clip.
- **Tidy up.** In the Take Selector: **F2** renames a take, **Alt+Up / Alt+Down** reorder, **Delete** removes one (the
  next take plays if it was the active one), and the row menu (right-click) has all of these. **"Flatten Audition"**
  keeps the take that plays and drops the others. Every change is one undo step.
- **Playback and export** show only the take that plays, exactly as a plain clip of it. FCPXML keeps the audition (Final
  Cut's `<audition>`, the active take first) and OpenTimelineIO keeps every take for Wabcut; FCP7 XML and EDL write the
  take that plays and the export report lists the others. See [interchange](interchange.md#auditions-06).
- Scripts: `audition.create`, `audition.addTakes`, `audition.addSource`, `audition.setActive`, `audition.removeTake`,
  `audition.moveTake`, `audition.renameTake`, `audition.duplicateTake`, `audition.flatten`, `audition.describe`.

### Render and Replace a nest, and the keys that did nothing (0.7)

- **Render and Replace.** A nested sequence is composited again at every frame, so a heavy nest (many layers, effects) can
  stutter. Select its clip (the picture and sound halves of one nest count as one) and choose **Clip > Render and
  Replace…** (also in the clip's right-click menu and the command palette). The window lists the nest, the stretch the clip
  plays and the stretch that is rendered - what the clip plays plus the **handles** (extra seconds either side, 1 s unless
  you change it, so a later trim still has picture) - and asks for the **format**: ProRes 422 HQ, DNxHR HQ, H.264 or ProRes
  4444, which keeps transparency (the window picks it when the nest's lowest track has a gap or a graphic, and warns when
  you choose a format that would fill it with black). The files go to a folder named after the project next to it
  ("My Film Rendered Media"; for a project that is not saved yet, under the default export folder), or any folder you
  choose with **Folder…**.
- **It runs in the export queue**, off the editing window: **Background tasks** shows its progress, and the window shows it
  too, with **Stop**. **Hide** closes the window and the render goes on. When a file is done, the clip plays it from then on -
  same place, In and Out, speed, links, keyframes and effects; picture and sound of one nest come from one file - as one
  undo step named "Render and Replace", and a message says so. Nothing is replaced when the render fails or is stopped, when
  the nest (or anything it plays) was edited while it rendered, or when the clips changed; the clips keep playing the live
  nest. The file stays where it was made.
- **Restore Unrendered** (Clip menu, or right-click a rendered clip) plays the nest again, exactly as it was, as one undo
  step. The rendered file stays in the project's bin beside the nest (Remove Unused deletes it from the project). While a
  clip plays the file, edits to the nest do not show; restore it first.
- **Not rendered:** multi-camera clips (they show one angle of their source), auditions (flatten first), locked clips,
  clips with Time Remapping or an object mask. The menu rows grey out with the reason.
- **The keys that did nothing now act (card 024).** In the **Media Browser**: **Shift+O** opens the current clip in the
  Source Monitor (without importing it), **Shift+Right** selects the list of clips and **Shift+Left** opens its Drives and
  places menu (the browser has one list, so the directories are the places). **Ctrl+L** in **Effect Controls** and the
  **Audio Track Mixer** turns the Program Monitor's **Loop** on or off, with a message, so Link keeps Ctrl+L everywhere
  else. **Alt+[** and **Alt+]** set the **work area**'s In and Out at the playhead; it shows as a thin bar at the bottom of
  the ruler, **Sequence > Clear Work Area** removes it (one undo step), and Export's **Work Area** range is greyed out
  until there is one.
- **Two Premiere keys are unbound** because Wabcut has no feature behind them: the Metadata panel's **Loop** and **Play**. The
  commands are still listed in Keyboard Shortcuts, and a Premiere key set that binds them still imports. The Audio Track
  Mixer's **Show/Hide Tracks** (Ctrl+Alt+T) and **Meter Inputs Only** (Ctrl+Shift+I, which clears the In mark everywhere
  else) act in the mixer. See [keymap](keymap.md).

## Scripts and agents

Every edit in Wabcut is a typed command. You can drive them from outside the app:

- **`wabcut-cli cmd <project> --json '<request>'`** runs commands on a saved project.
- **`wabcut-cli tools`** prints them as tool definitions for an LLM.
- **`wabcut-cli mcp --project <file.wabcut>`** serves them to Claude, Codex and other agents over MCP. It is read-only
  unless you pass `--allow-edit`.

See [commands](commands.md) and [MCP](mcp.md).
