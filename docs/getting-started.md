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
- Windows 10 (22H2) or 11, x64.
- A DirectX 11 GPU. NVIDIA (NVENC/NVDEC), AMD (AMF) and Intel (Quick Sync) hardware decoding and encoding are used when
  present.
- 16 GB of RAM recommended (8 GB minimum; local AI models need more).
- 2 GB of disk for the app, plus room for caches and proxies.

**Two packages, one app:**

| Package | How |
|---|---|
| `wabcut-0.5.1-windows-x64.msi` | Double-click it. It installs **for your user only**, with no administrator rights, to `%LOCALAPPDATA%\Programs\Wabcut` (or a folder you choose). Wabcut appears in the Start menu and in Settings > Apps. The options page has an unticked *Open .wabcut project files with Wabcut* box: tick it to open projects by double-clicking them. |
| `wabcut-0.5.1-windows-x64-portable.zip` | Unzip it anywhere, for example a USB drive, and run `bin\wabcut.exe`. Nothing is installed. |

**SmartScreen.** The packages are not code-signed yet, so Windows SmartScreen may say "Windows protected your PC".
Choose **More info**, then **Run anyway**.

**Uninstalling** (Settings > Apps > Wabcut) removes the program files only. Your projects, autosaves, proxies, caches,
downloaded AI models, settings and API keys stay.

Admins can install silently:
`msiexec /i wabcut-0.5.1-windows-x64.msi /qn [INSTALL_ROOT="D:\Apps\Wabcut"] [WABCUT_ASSOCIATE=1]`.

**Updates** (0.5) are off until you turn them on: **Settings > General > Updates > "Check for updates
automatically"** looks once a day at most and says when a new version is out (**"Skip This Version"** keeps it quiet
about that one), and **Help > "Check for Updates…"** looks at once. The check asks GitHub for the latest release and
sends nothing about you or your projects. When Wabcut was installed with the MSI, **"Download and Install"** fetches the
update and checks its signature and checksum; then **"Install When I Quit"** or **"Quit and Install"** installs it
after Wabcut closes (unsaved changes and running exports ask first, as on any quit) and opens Wabcut again.
**"Later"** keeps the download. The portable ZIP opens the release page instead. 0.5.0 is the first version that can
update itself: from 0.4 or older, install it by hand.

## First run

1. **The Home screen** comes first. It is a start window with three pages in the bar on the left - **Home**,
   **Projects** and **Learn** - and, along the top, a search over your projects, **"Open…"** (Ctrl+O), **"Import…"**
   (Ctrl+I) and **"New project"** (Ctrl+N); Ctrl+, opens Settings, as in the editor. Nothing on it scrolls the whole
   window; long lists scroll inside their own area.

   | Page | What it has |
   |---|---|
   | **Home** | **"Continue"**: the project you opened last, large, with **"Open"** and **"Show in Explorer"**. **"Start"**: New project, Open project…, Import… and a drop well ("Drop files here": media, game recordings, timelines or projects), **"Import folder…"** and a link to the guide for timelines from Premiere, Resolve or Final Cut. **"Recent"**: your latest projects as cards, and **"All projects"**. With no projects yet, Continue and Recent say so in one line, and Recent points to Learn. |
   | **Projects** | Every recent project as a grid or a list, **All** or **Starred**, sorted by **Last opened** or **Name**; Wabcut remembers your choice. Typing in the search shows this page. Click a project to open it; its **"…"** button, a right-click, the Menu key or Shift+F10 opens its menu: Open, Open Next to This One, Show in Explorer, Copy Path, Star / Unstar, **Locate…** (a project whose file moved) and Remove from List. The arrows move between projects, Enter opens, Delete removes it from the list (with **Undo** for a few seconds). The **"…"** in the page's toolbar holds **"Clear Unstarred Projects…"**. Starred projects stay even when the list is full. |
   | **Learn** | **"Start here"**: **"Explore the sample project"**, "Take the tour", "Keyboard Trainer", "What's new". **"User guide"**: the guides on the Wabcut website (getting started, the window, coming from Premiere Pro, moving timelines between editors, keyboard shortcuts, gaming videos, Shorts, video essays, documentaries). **"Support"**: release notes, the roadmap and reporting an issue. |

   **"New project"** (or File > New Project, Ctrl+Alt+N) opens the **New project page**: pick a **format** tile
   (Landscape: 1080p at 23.976 / 25 / 29.97 / 30 / 60 fps, 4K UHD, 1440p60, 4K UHD 60; Vertical & square: 9:16, 1:1,
   9:16 at 60 fps; Custom), check the **name** (a free one is filled in: "Untitled Project", "Untitled Project 2"…) and
   the **location** (**"Browse…"**), then **"Create project"**. **"Advanced settings"** opens the rest: frame size, frame
   rate (up to 240 fps) and drop-frame timecode, pixel aspect, fields, start timecode, audio sample rate, audio master
   (stereo or 5.1), the number of video and audio tracks and their format, colour management and the preview file
   format; **"Reset to defaults"** puts them back. Wabcut remembers your last format and settings for the next project.

   **One Import for everything.** "Import…", "Import folder…" or files dropped anywhere on Home:
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
     or Esc drops it. A dotLottie archive (`.lottie`) or a picture dragged from a web browser says what to do instead.

   Messages (an import that failed, guidance) show in a strip under the top bar; a long one scrolls. **"Back to
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

## The window

**The menu bar** sits in the title bar after the logo, in Premiere's order: **File, Edit, Clip, Sequence, Markers,
Graphics, View, Window, Help**. Click a title (or press **Alt** and its letter: Alt+F, E, C, S, M, G, V, W, H; on
Windows and Linux) to open its menu; while one is open, point at another title to open that one, or use **Left** and
**Right**. If you give one of these Alt+letter keys to a command in Keyboard Shortcuts, the key runs that command and
the menu opens with a click. On macOS there are no Alt (Option) letters for the menus: Option+letter types accented
characters there. Every row shows its shortcut, and rows that cannot run right now are greyed out (no clip selected,
nothing to undo). A command you choose acts where you were before the menu opened, and whether it can run is judged
there too: Markers > "Mark In" or "Clear In" with the Source Monitor focused works on the source's marks, with the
Timeline focused on the sequence's. The titles are in the Tab order too (Space, Enter or Down opens one).
When the window is too narrow for the titles and the workspace tabs, they fold into one **"Menu"** button with the same
menus inside. The logo opens the File menu as well.

**File:**
- "Home", "New Project…" (Ctrl+Alt+N), "New Sequence…" (Ctrl+N), "New Sequence From Clip";
- "Open Project…" (Ctrl+O), **"Open Recent"** (the last ten projects, and "Clear Recent"), "Open Project Next to This
  One…". A `.wabcut` you double-click in File Explorer while Wabcut runs opens in that window (not in a second
  Wabcut). Choosing the project you are working in (from Open Recent, the palette or Explorer) only brings it forward:
  it is not read again, so its undo history and unsaved changes stay. "Open Project…", "Import…" or a drop of that
  project's own file does not read it again either (they may ask about unsaved changes first); "Revert" is the way to go
  back to the saved file. A project open next to it comes into the panels;
  one that asks about unsaved changes waits until you have answered the question already on screen;
- "Close Panel" (Ctrl+W: the panel shown in the group you clicked last); **"Close Project"** (Ctrl+Shift+W): with other
  projects open, the current one closes; with only one, it closes and Home shows (the sample project waits behind
  it). Unsaved changes are asked about first;
- "Save" (Ctrl+S), "Save As…" (Ctrl+Shift+S), **"Save a Copy…"** (Ctrl+Alt+S: the project as it is now goes to another
  file; you keep working in this one), **"Revert"** (back to the version last saved, after a confirmation: the unsaved
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
  to the timeline from there); files dropped with it are edited in as before. Import… and Import Folder… open in the
  folder your last import came from (for a folder: the one around it), else the project's folder;
- numbered stills (an image sequence from a renderer, a game's photo mode or a timelapse) come in as one clip from the
  Library's ··· menu, **"Import image sequence…"** (Premiere's Import with "Image Sequence" ticked): choose the first
  file (PNG, JPEG, TIFF, DPX, Targa, OpenEXR or BMP) and the frame rate (29.97 fps by default, Premiere's Indeterminate
  Media Timebase). That file and the ones numbered after it, up to the first gap, play one still per frame. The clip is
  named after its first file; the stills stay where they are (the first one stands for the clip when Wabcut looks for
  offline media). Command: `media.importImageSequence`. CinemaDNG frames (.dng) import the same way and play
  developed into Rec.709. For ProRes RAW and CinemaDNG items, the Library's right-click **Modify > Source Settings…**
  sets the exposure and white balance they are developed with (As Shot, or the colour temperature and tint of the
  light to balance for); commands `media.rawSourceSettings`, `media.setRawSourceSettings`.

  A Photoshop file (.psd, .psb) opens **Import Layered File**: Merge All Layers, Merged Layers (the ones you tick),
  Individual Layers (a bin with one clip per layer) or Sequence (those clips stacked on tracks of a sequence of the
  document's size), each at Document Size or Layer Size. RGB, grayscale and CMYK files (converted to RGB) of 8, 16 or
  32 bits import, with their blend modes. The clips read the file each time, so what you save in Photoshop comes
  through. Commands: `media.layeredFileInfo`, `media.importLayeredFile`.

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
  when the export queue has made them), or **Create Proxies**; Ingest applies to clips, not to folders. The folder
  button (**Drives and places**) lists Videos and your home folder, then the game capture folders that exist on this
  computer: **"Game Bar captures"** (Videos\Captures), **"NVIDIA ShadowPlay"** (Videos\NVIDIA) and **"OBS recordings"**
  (each OBS Studio profile's recording folder); then **"Recent: …"** for the folders your last imports came from, then
  the drives (a recent folder that is a drive's root is listed as the drive). The Media Browser opens at the folder it
  showed last, once you show it. Commands: `media.browse`, `media.importCameraClips`.
- "Import Lottie…"; **"Export"**: "Media…" (Ctrl+M), "Frame…", "Send to Export Queue", "Timeline (XML, EDL, OTIO)…";
- "Relink Offline Media…", "Recover…", "Collect Files…";
- "Exit" (Ctrl+Q).

**Edit:** "Undo", "Redo", "Cut", "Copy", "Paste", "Paste Insert", "Paste Attributes…", "Remove Attributes…", "Clear",
"Ripple Delete", "Duplicate", "Select All", "Deselect All", "Find…", "Edit Original", "Keyboard Shortcuts…"
(Ctrl+Alt+K), "Media and Timeline Preferences…" and **"Settings…"** (Ctrl+,). While you type in a text field (a
search, a name, a title's text), "Undo", "Redo", "Cut", "Copy", "Paste", "Clear" and "Select All" work on its text,
as their keys do there, not on the timeline.

**Clip:** "Make Subclip…", "Speed/Duration…", **"Scene Edit Detection…"** (0.5, see
[Premiere's sequence and editing tools](#premieres-sequence-and-editing-tools)), "Audio Gain…", "Audio Channels…",
"Video Options" (frame holds, field options, scale and fill), "Insert", "Overwrite", "Enable", "Link", "Group",
"Ungroup", "Merge Clips…", "Interpret Footage…", "Source Settings…", "Modify Timecode…", "Replace Footage…", "Make
Offline", "Reveal in Project", and the gaming moments "Slow-mo at Playhead" and "Punch-in at Playhead".

**Sequence:** "Sequence Settings…", "Render Effects In to Out", "Match Frame", "Reverse Match Frame", "Add Edit", "Add
Edit to All Tracks", "Trim Edit", the ripple trims to the playhead, the transitions, "Lift", "Extract", "Close Gap",
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
"Delete Workspace…"), **"Maximize Frame"** (Shift+`), **"Reset Panel Layout"** (Alt+Shift+0), "History", then every
panel (a dot shows the ones open in this workspace; Generate is listed while AI is on). Choosing one opens it, or brings
it forward where it is, and puts the keyboard in it.

**Help:** "Wabcut User Guide", "Getting Started", "Coming from Premiere Pro" and "Keyboard Shortcuts Guide" (on
[wabcut.zyphite.com](https://wabcut.zyphite.com/guide)), "Keyboard Shortcuts…", "Welcome and Setup…", "Take the Tour",
"Keyboard Trainer…", "What's New", "Release Notes", **"Check for Updates…"** (0.5, see [Install](#install)), **"Open
the Sample Project"**, "Report an Issue" and "About Wabcut".

The **command palette** (Ctrl+Shift+P) has these too, plus **"Open panel: …"** for every panel, **"Workspace: …"** for
every workspace and **"Open Recent: …"** for the recent projects.

**The title bar** has:
- **The sequence pill.** Switches sequences. Its list has "New Sequence…" (Ctrl+N), "Reframe…", "Rough Cut from
  Script…", "Narration from Script…" and "Podcast Workflow…".
- **The command palette** button (**Ctrl+Shift+P**).
- **Undo** (Ctrl+Z) and **Redo** (Ctrl+Shift+Z). Their tooltip names the step.
- **"Save"** and **"Export"** (Ctrl+M). A dot on Save means there are unsaved changes; its tooltip says when the
  project was last autosaved. (The GPU and autosave status are in Settings; Settings > Interface can show them in the
  title bar again.) When autosave cannot write your project (a drive gone, a full disk, no permission), Wabcut says so
  once with **"Save As…"**, the dot turns red and the tooltip gives the reason; it keeps trying, and the next autosave
  or save that works clears it (0.4).
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

| Preset | What it lays out | Its key opens (or makes) the first one |
|---|---|---|
| Edit | Library, Source and Program Monitors, Effect Controls, the timeline with the Audio Meters right of it (0.5) | Alt+Shift+6 |
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
  - Window > **"Reset Panel Layout"** (Alt+Shift+0; Premiere's "Reset to Saved Layout", the name it has in the
    command palette and the Keyboard Shortcuts) puts the workspace back the way its preset lays it out, and the
    message it shows has **"Undo"**.

**Panels** open from the **Window** menu: "Library" (Shift+1), "Effects" (Shift+7), "Transcript", "Audio Tools",
"Essential Sound", **"Callouts"** (titles, templates and stickers), **"Highlights"**, "Markers", "Review", "Shorts",
"Takes", "Generate" and the rest (they replace the former left rail).

**The Audio Meters panel** (0.5, Window > **"Audio Meters"**) shows the level of the mix: a bar per channel (two for
stereo, six for a 5.1 master) with its peak hold, a 0 to −60 dB scale, the highest peak as a readout and a clip light on
top that turns red at 0 dB; click it (or press Space) to clear it. Workspaces made or reset from the Edit preset have it
right of the timeline; a workspace saved before keeps its layout until you add it from the Window menu. Like any
panel it can be a tab, get a window of its own or close.

**Settings** (Edit > "Settings…", **Ctrl+,**, or Home's gear) is one window with these pages:
  - **Search** (the field at the top, ready to type when Settings opens): type what you are after ("proxies",
    "dark", "default folder") and pick a result: its page opens on that setting, which lights up for a moment.
    The command palette lists every setting too ("Settings" rows);
  - **"General"**: what Wabcut shows when it starts (**Home**, or **Reopen the last project**), the workspace it opens
    in, the native window frame; **Projects**: the save state, how often to autosave, the default location for new
    projects (Home's New project page starts there; **Automatic** = the last project's folder), how many recent
    projects Home and File > Open Recent show (a lower number hides the older ones until you raise it again; starred
    ones always show), and project locking; the save state says **"Autosave failed"** (red) with the reason while
    autosave cannot write; **Background work** (0.4): **"Keep the computer awake while exporting"** (on by default;
    Windows only) - while exports, Render In to Out, proxies, transcription, Highlights, Shorts or Collect Files run,
    Windows does not go to sleep on its own (the screen can still turn off); **Updates** (0.5): **"Check for updates
    automatically"** (off by default) and **"Check for Updates…"** (see [Install](#install));
  - **"Appearance"**: themes (Ultraviolet, Daylight, Graphite, Midnight, Paper, and your own), dark or light, the
    accent, high contrast, reduce motion, the interface size, and **"Custom colours"** for every colour of the
    interface and the clip labels. "Save theme" keeps your colours under a name; "Import…" and "Export…" read and
    write theme files you can share;
  - **"Interface"**: the GPU and save status in the title bar, and "Resize panels live";
  - **"Timeline & editing"**: linked selection, the snap line, trim offsets, thumbnails, waveforms, **"Play audio while
    scrubbing"** (0.5, on by default; Shift+S turns it on and off) and more;
  - **"Playback & performance"**: the GPU and video decoding in use, and "Media & cache…". **"Graphics card"** (0.5)
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
  **"Delete Track"** (it asks first when the track has clips), **"Delete Empty Tracks"** and **"Sync Lock"**; an audio
  track's menu also has its channel format. Double-click a header (or press F2 on its name) to rename the track; its
  name shows beside V1 / A1 where there is room. Each is one undo step.
- **Gaps**: click the empty space between two clips to select the gap; **Delete** (while the Tracks panel has the
  focus) or right-click > **"Ripple Delete"** closes it. When another sync-locked track has clips in that time, nothing
  moves and a message says why.
- **Transitions**: right-click one for **"Set Duration…"**, its alignment on the cut, **"Replace with"** the default
  transition, **"Set as Default Transition"** and **"Clear"**. The default video and audio transitions (what Ctrl+D,
  Ctrl+Shift+D and Shift+D add) are also in Settings > Timeline & editing.
- **Markers** show their names on the ruler (cut short before the next marker); point at one for its comment, time and
  length.
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
**Loop**, **Play In to Out**, **Toggle Full Screen** (Ctrl+`) and **Transmit** to a second display; rulers and guides
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
  choose **"Game under my voice"** (it is chosen for you when a track is named for the game, as Start with media names
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
- **From one highlight:** in **"Highlights"**, select a moment and choose **"Make a Short"** (or **"Make Shorts (N)"**
  for every accepted moment at once). **"Short options"** set:
  - the **"Layout"** ("Auto", "Split" or "Blurred fill") and the **"Facecam"**;
  - the **"Length"**: **"Lead-in"** (the build-up before the moment, 3 s by default), **"Tail"** (the reaction after
    it, 1 s) and **"At least"** (Off, 15, 30, 45 or 60 s: a shorter moment gets the missing time, two thirds before
    and one third after, at most 30 s a side; what one side cannot take goes to the other);
  - **"Captions"** and their **"Caption style"** ("Shorts" by default: big outlined words two thirds down, above the
    platforms' buttons; or "Clean", "Bold Box", "Podcast", "Thai-friendly" or one of your project's presets);
  - a **"Hook title"** (the moment's title over the first seconds).

  These options are remembered.
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

**0. Log the footage (0.2).**
- **Metadata.** The **"Metadata"** panel (Window menu) edits the selected clips' **Scene**, **Shot**, **Take**, **Log Note**,
  **Good**, **Client**, **Tape Name**, **Status**, the camera fields (**Camera Angle**, **Label**, **Model**, **Shot
  Date**, **Shot Location**), **Comment** and **Description**, and shows what the file itself says (its XMP and container
  tags). The file's XMP can be edited there too, and **"+"** adds a property it does not have yet. **"Read from file"**
  takes the camera's or another editor's XMP; **"Write XMP"** writes it out. XMP goes into a sidecar `<clip>.xmp` next to
  the file unless you turn on **Preferences > Media > "Write XMP metadata into MP4 and MOV files"**: then those files take
  it themselves (a checked copy replaces the file; if anything fails it stays as it was). In the Library's list view the
  same fields are columns (choose them in the column chooser); a click ticks **Good**.
- **Search Bins.** Type in the Library's search field, then **"Create new search bin"** next to it: the bin keeps
  listing whatever matches (for example Good takes of scene 12), from every bin, as you log more.
- **Freeform view.** The Library's third view: drag thumbnails anywhere, pile alternates into stacks (Ctrl+wheel goes
  through a stack), and keep arrangements with **"Save as New Layout…"** in the canvas menu.
- **Thumbnail size.** In the icon view, the slider under the cards (with its − and + buttons) sizes them from half to
  double, as do Ctrl+wheel, **=** / **-** and **Shift+]** / **Shift+[** while the Library has the focus. **Home**,
  **End**, **Page Up** and **Page Down** move the selection there (Shift extends it), as in Premiere's Project panel.
- **Productions.** For a long project split into several (one per episode or reel), the **"Production"** panel makes a
  production folder and new projects in it. Double-click a project to open it next to the ones you have open: they all
  stay loaded with their own undo history and unsaved changes, and a click in **"Open projects"** switches between them
  at once (**File > Close Project** closes the one shown, asking first if it has unsaved changes). Browse another project
  read-only to bring its sequences or bins over: they come in as copies with their media, which is never copied on disk,
  and remember where they came from (**"Reveal in Source Project"** in the Library's item menu; the **link** button
  offers the copies a relink their source project made). **Production Settings** (the gear) choose where every project's
  previews and media cache go. A project a colleague has open shows a lock and opens read-only for you.

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

- **Frame size.** Right-click a clip > **"Video Options"** > **"Scale to Frame Size"** (fits the picture, Scale stays
  100 %), **"Set to Frame Size"** or **"Set to Fill Frame"** (writes Motion's Scale).
  **"Media and Timeline Preferences…"** (Edit menu, or Settings > Timeline & editing) > **"Media"** >
  **"Default Media Scaling"** applies one of them to everything you import.
- **Sequences.** **"Sequence Settings…"** (Sequence menu, or a sequence's Library menu) changes the frame size, frame rate,
  pixel aspect, fields, audio sample rate, preview format, start timecode and time display, with presets from 23.976 to
  240 fps, vertical 1080x1920 and square 1080x1080. A new frame rate keeps every cut where it was, on the new frames.
  **"New Sequence From Clip"** makes a sequence that matches a clip; **"Duplicate"** copies a sequence or a clip.
- **Replace.** Shift-drag from the Source Monitor (or Alt/Shift-drag from the Library) onto a clip, or right-click >
  **"Replace With Clip"**: the new clip takes the old one's place and keeps its effects. In the Library: **"Replace
  Footage…"**, **"Make Offline"** and **"Edit Original"** (**Ctrl+E**).
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
- **History.** Window menu > **"History"** lists the undo steps; click one to go back to it. While the History panel
  has the focus, **Left** and **Right** step one state back or forward.
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
  **Ctrl+Shift+;** to the previous one ("Next in Sequence" / "Previous in Sequence").
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

## Scripts and agents

Every edit in Wabcut is a typed command. You can drive them from outside the app:

- **`wabcut-cli cmd <project> --json '<request>'`** runs commands on a saved project.
- **`wabcut-cli tools`** prints them as tool definitions for an LLM.
- **`wabcut-cli mcp --project <file.wabcut>`** serves them to Claude, Codex and other agents over MCP. It is read-only
  unless you pass `--allow-edit`.

See [commands](commands.md) and [MCP](mcp.md).
