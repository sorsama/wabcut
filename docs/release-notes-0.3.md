# Wabcut 0.3: release notes (0.3.0)

Windows 10/11 x64, with preview builds for Linux x64 and macOS on Apple silicon. Free and open source
(GPL-3.0-or-later). No account, no subscription.

Wabcut 0.3 changes how the app is laid out more than what it can edit. Home is now a start window with pages and one
Import for everything. The editor has Premiere's menu bar and Window menu in place of the left rail, and the Effects
panel uses Premiere's folders. Many everyday steps in the timeline, the monitors, the Library, Export and Settings take
fewer clicks, and several shortcuts that did nothing in 0.2 now work.

The project file format has not changed since 0.2 (schema 18): Wabcut 0.3 and 0.2 open each other's projects, and
Wabcut 0.3 opens 0.1 projects.

## 0.3.0 (23 October 2026)

### Home
- **A start window with pages:** **Home**, **Projects** and **Learn** in the bar on the left, and a **New project**
  page. The window itself does not scroll; long lists scroll inside their own area. Along the top: a search over your
  projects, **Open…** (Ctrl+O), **Import…** (Ctrl+I), **New project** (Ctrl+N or Ctrl+Alt+N) and **Back to …**
  (with the project's name) when a project is open behind Home; Esc does the same. Ctrl+F goes to the search and Ctrl+,
  opens Settings.
- **The Home page:** **Continue** (the project you opened last, with Open and Show in Explorer), **Start** (New
  project, Open project…, Import…, a drop well, Import folder… and a link to the guide for timelines from Premiere,
  Resolve or Final Cut) and **Recent** (your latest projects as cards).
- **One Import for everything:** Import…, Import folder… (read with its subfolders, up to 1,000 files) or files dropped
  anywhere on Home. A Wabcut project opens. A timeline (FCP7 XML, FCPXML, OpenTimelineIO, EDL) becomes a new project
  with its import report. A Premiere `.prproj` asks before the experimental reader runs. Files Wabcut cannot read (a
  Resolve `.drp`, a Final Cut library, AAF, a dotLottie archive, a picture dragged from a web browser) say what to do
  instead, with **How to export**. Media open the New project page with the files.
- **New project page:** format tiles grouped Landscape / Vertical & square / Custom, with 1440p60, 4K UHD at 60 fps and
  vertical 9:16 at 60 fps added; a free default name ("Untitled Project", "Untitled Project 2"…); the location; and a
  summary line ("1920 × 1080 · 29.97 fps · 48 kHz stereo · 3 video / 3 audio tracks"). **Advanced settings** has the
  frame size, the frame rate (up to 240 fps) and drop-frame timecode, pixel aspect, fields, start timecode, audio sample
  rate, audio master (stereo or 5.1), how many video and audio tracks (1 to 99) and their format, colour management
  and the preview file format, with **Reset to defaults**. These apply before the project exists, so a new project has
  no undo step and nothing unsaved. Your last format and settings are filled in next time. A location that cannot be
  made (a drive that is not there) is refused before anything is created.
- **Start with media:** media imported from Home open the New project page with the files listed (remove any with ×),
  **Match my media** chosen (the first video's size and frame rate; a variable frame rate becomes its nominal rate), the
  first file's name, and **Add the clips to the sequence** on, so Create puts them on the timeline one after another.
  A recording with several audio streams shows each stream on its own named track (Game, Mic, Discord) or left out,
  and the project gets one audio track per stream you keep. Chips mark 4K, high frame rate, long, variable frame rate
  and HDR media; proxies are made for 4K, above 60 fps, or 20 minutes and longer. This replaces 0.2's "New gaming
  video" card.
- **Projects page:** All or Starred, sorted by Last opened or Name, as a grid or a list (Wabcut remembers your choice).
  Each project's menu (its **…** button, a right-click, the Menu key or Shift+F10) has Open, Open Next to This One, Show
  in Explorer, Copy Path, Star / Unstar, **Locate…** (a project whose file moved) and Remove from List (a starred
  project is unstarred too; Undo stars it again). The arrows move
  between projects, Enter opens, and Delete removes one from the list with **Undo** (or Ctrl+Z) for a few seconds.
  **Clear Unstarred Projects…** asks first. The list shows 50 projects (it was 12; Settings can change it), and starred
  projects always stay.
- **Learn:** **Explore the sample project** (it moved here from Home's header), the tour, the Keyboard Trainer and
  What's new; the user guide on the Wabcut website (getting started, the window, coming from Premiere Pro, moving
  timelines between editors, keyboard shortcuts, gaming videos, Shorts, video essays, documentaries); release notes,
  the roadmap and reporting an issue. When Wabcut starts, the sample project waits behind Home until you open it from
  Learn.
- Home's messages (an import that failed, guidance, an open or save that failed) show in a strip under the top bar,
  with an action where there is one (Save As…).

### The editor's shell
- **A menu bar** in the title bar, in Premiere's order: File, Edit, Clip, Sequence, Markers, Graphics, View, Window,
  Help. Each command shows its shortcut and is greyed out when it cannot run now. A command acts where you were before the
  menu opened: Mark In with the Source Monitor focused marks the source, Copy in a text field copies its text. Alt+F,
  E, C, S, M, G, V, W and H open the menus on Windows and Linux (unless you gave that key to a command); Left and Right
  move between open menus. In a narrow window the menus fold into one **Menu** button.
- **File** adds **Open Recent** (the last ten projects, and Clear Recent), **Close Panel** (Ctrl+W), **Close Project**
  (Ctrl+Shift+W; with only one project open, Home shows with the sample project behind it), **Save a Copy…**
  (Ctrl+Alt+S: the project as it is now goes to another file, and you keep working in this one) and **Revert** (back to
  the saved version, after a confirmation). **Edit** ends with **Settings…** (Ctrl+,).
- **Window** has the workspaces (switch, New Workspace from a preset, Duplicate, Rename…, Delete…), **Maximize
  Frame**, **Reset Panel Layout**, History and every panel. A closed panel opens as a tab in the **group you clicked
  last**, which has an outline in the accent colour (violet unless you changed it); a panel that is open already comes forward where it is. Either way it gets the
  keyboard.
- **The left rail is gone.** The work area starts at the window's edge, the panels open from the Window menu, and
  Settings > Interface no longer has the rail's editor.
- **Maximize Frame:** **\`** maximizes the panel group under the pointer, **Shift+\`** the active group, and a
  double-click on a tab does it too; the same again brings the other panels back.
- **The tab menu:** right-click a panel's tab for Close Panel, Close Other Panels in Group, Undock Panel, Undock Panel
  Group (Dock Panel Group while it has a window) and Maximize / Restore Frame. A lone panel's tab drags like a grouped
  one.
- **Help:** the Wabcut User Guide, Getting Started, Coming from Premiere Pro and Keyboard Shortcuts pages on the
  website, then Keyboard Shortcuts…, Welcome and Setup…, Take the Tour, Keyboard Trainer…, What's New, Release Notes,
  Open the Sample Project, Report an Issue and About Wabcut.
- **One Wabcut at a time:** a `.wabcut` you double-click in File Explorer while Wabcut runs opens in that window
  (Wabcut asks about unsaved changes first; the project that is already open just comes forward). `--new-instance` starts a second Wabcut.
- **The window opens where you left it:** its size, position and maximized state; centred on the main screen when its
  monitor is gone.
- **Shortcuts that did nothing in 0.2 now work:** Shift+5 (Effect Controls), Alt+Shift+0 (Window > Reset Panel Layout,
  with Undo on its message), Shift+` (Maximize Frame), Ctrl+W (Close Panel), Ctrl+Shift+W (Close Project), Ctrl+Alt+S (Save
  a Copy), and Left / Right in the History panel. Settings (Ctrl+,) is a keymap command now, so you can give it another
  key.
- **The command palette** lists "Open panel: …", "Workspace: …", "Open Recent: …" and every setting, and the window
  and workspace commands no longer say "Not available yet".
- **Messages** at the bottom right can carry an action (Undo, Save As…, Show in Explorer) and stay while the pointer
  is on them. A project that cannot be opened or saved always says why, and a failed save offers Save As…. Leaving the
  sample project after editing it first asks "Discard your changes to the sample project?".

### The Effects panel
- **Premiere's folders:** Favorites (when you have some), Presets, Lumetri Presets, Audio Effects, Audio Transitions,
  Video Effects, Video Transitions, Audio Plug-ins, then your custom bins. Presets bins nest and folders show how many
  items they hold. At the next start the folders are closed except Favorites and the one you opened last; the folders
  inside keep their state. A search opens every folder with a match and counts the matches; clearing it puts your
  folders back as they were.
- **Views and filters:** **List**, **Compact** or **Icons** (remembered), and a filter for All, Video, Audio,
  Transitions, Presets, Favorites or Audio Plug-ins. Audio plug-ins can go in Favorites and custom bins, and the panel
  remembers whether they are grouped by vendor or by category.
- **Apply to every selected clip:** a double-click or Enter puts an effect, a preset or an audio plug-in on every
  selected clip in one undo step. A transition goes on both ends of each selected clip.
- **Presets per bin:** a presets bin's menu has **Import Presets…** (into that bin) and **Export Presets…** (that bin's
  presets, as `<bin>.wabcut-presets.json`); the file dialogs start in the folder you used last.
- **Drag and drop in the panel:** an effect dropped on a custom bin goes in it, one of your presets dropped on a
  presets bin moves there, and a `.wabcut-presets.json` file dropped from File Explorer is imported.
- **Menus:** Apply to Selected Clips, Add to / Remove from Favorites, Add to Custom Bin, Remove from the bin, **Set as
  Default Transition**, Preset Properties…, Export Preset…, New Presets Bin, Rename, Delete…, Expand All and Collapse
  All.
- **Deletes ask first** (Delete or Backspace, or the menus). F2 renames a custom bin or one of your presets bins in
  place, and a new custom bin appears ready to name. The keyboard moves through folders and items (arrows, Home, End,
  Page Up, Page Down; Left and Right close and open folders). A status line says what happened and clears itself; a
  banner says when the presets library cannot be read.

### Timeline and monitors
- **Type a time:** click the timecode of the Timeline, the Program Monitor or the Source Monitor and type a timecode,
  digits ("10203" is 00:01:02:03 in a timecode display, a frame count in Frames display), Premiere's shorthand ("2." is two seconds) or an offset ("+15", "-1:00"). Enter goes
  there, Esc cancels. A time that drop-frame timecode skips goes to the next one that exists.
- **Gaps:** click the empty space between two clips to select the gap. Delete (while the Timeline has the focus) or
  right-click > **Ripple Delete** closes it in one undo step, or says why not (a sync-locked track with clips in that
  time).
- **Track headers** of video and audio tracks have a menu (right-click, the Menu key or Shift+F10): Rename…, Add Video
  Track Above / Add Audio Track Below, **Add Tracks…** (how many of each kind, where, and the audio format), **Delete
  Track** (it asks first when the track has clips), **Delete Empty Tracks** and Sync Lock. Double-click or F2 renames a
  track in place, and its name shows beside V1 / A1 where there is room. Each is one undo step.
- **Marker names on the ruler,** cut short before the next marker. Pointing at a marker shows its name, comment, time
  and length, on the Program Monitor's time bar too.
- **Default transitions:** choose the default video and audio transitions in Settings > Timeline & editing, or with
  Set as Default Transition; Ctrl+D, Ctrl+Shift+D and Shift+D use them. Right-click a transition on the timeline for
  **Set Duration…**, Center / Start / End at Cut, Replace with the default, Set as Default Transition and Clear.
- **Reveal:** the clip menu has **Reveal in Project** (selects the clip's item in the Library and brings the Library
  forward; give it a key in Keyboard Shortcuts: `clip.revealInProject`) and **Reveal in File Explorer**.
- **Source Monitor drops:** drop a Library clip on the Source Monitor to open it there, or a file from File Explorer to
  import it and open it.
- **In and Out from the Library:** a Library item dragged to the timeline edits in only the In to Out you marked in the
  Source Monitor, and its card shows that range as a band, with its length on the badge.
- **Snap** and the time remapping view stay the way you left them at the next start.

### Library, export and settings
- **Library:** a thumbnail-size slider in the icon view, between zoom-out and zoom-in buttons; = and - (and Shift+] /
  Shift+[) size the cards. Home, End, Page Up, Page Down and Shift with the arrows move or extend the selection in the
  grid, list and Freeform views.
- **Export remembers** the preset (or gaming preset), the range and the folder you used last. A sequence with In and
  Out marks starts on In to Out (unless you last chose the whole sequence), and the button then says **Export In to
  Out**. Picking another preset keeps the range you chose. **Reset to defaults** forgets it all.
  Quick Export remembers its preset, has a Range row when the sequence has In/Out marks or a work area, and its preset
  list works with the keyboard.
- **Background work says when it ends:** Quick Export starting (Cancel), an export finished (Show in Explorer) or
  failed with the reason (Show the queue), and Render In to Out done or failed (Try Again). While Home is open these
  show in Home's message strip.
- **Search in Settings:** the field at the top of Settings finds a setting by its name or a keyword ("proxies", "dark",
  "default folder") and opens its page on it. The command palette lists every setting too.
- **Start-up:** Settings > General > **When Wabcut starts**: Show Home, or Reopen the last project (not after a crash
  or when there are autosaves to recover).
- **Project defaults** in Settings > General > Projects: the default location for new projects, how many recent
  projects to show, and project locking.

### Fixes
- Effect presets did not work from the panels: Save Preset, favourites, custom bins and Import / Export Presets
  answered "No project", because the presets part of the app never got the open project. They work now.
- The graphics tools in Callouts > Edit (Align and Distribute, Text Styles, Roll and Crawl) answered "No project" for
  the same reason, and the Title Style panel's text styles list stayed empty.
- File > Open Project…, an import or a drop of the project you are working in read it again from disk and dropped its
  undo history; it now stays as it is (File > Revert reads the saved file again).
- A panel dropped onto the Source and Program Monitors where they share a narrow place split them into two narrow
  columns. A drop there now does nothing and the overlay says why; left or right of the pair it docks beside them.
  Presses and hover there go to the monitor that is shown.
- In a sequence that counts in frames or feet+frames (Sequence Settings > Display Format), typed lengths and times
  (Speed/Duration, transition durations, caption times, marker times) are read in that format. They were read as
  timecode.
- Media with a variable frame rate that start a new project (imported or dropped over the sample project) give it
  their nominal frame rate instead of 30 fps.
- Importing media together with Lottie files over the sample project (File > Import, a drop on the timeline) put the
  animations in the sample, where they were lost when the new project started; they now go into the new project.
- Caption files dropped alone on Home made a project behind Home and said nothing; they now open the New project page,
  and the project gets them as caption tracks.
- A built-in Lumetri look that was exported and imported again was stuck in a bin you could not delete; it now lands
  in a presets bin of its own that you can rename or delete.
- The Effects panel no longer lists Essential Sound: that entry is the Essential Sound panel's record of a clip and
  did nothing to the sound when applied from the Effects panel.

What is not done or not checked yet is under [Known issues](#known-issues).

## Known issues
- The 0.3 changes are verified by automated tests only, in hidden windows. None of them has been through a release run
  with real projects yet: keep backups of your projects.
- Drag and drop (files dropped on Home, a picture dragged from a web browser, inside the Effects panel, onto the Source
  Monitor, files from File Explorer onto the Effects panel) was tested with simulated drops, not with a real mouse drag. Pointing at markers on the ruler and the time bar was
  tested with simulated hover only.
- Screen readers have not been checked by hand (NVDA, Narrator). The Effects panel's rows report keyboard focus to
  them, but whether a screen reader announces it is not confirmed, and folders say "open" or "closed" in their
  description instead of as an expanded state (that needs Qt 6.12).
- A gap can only be selected and a timeline transition's menu only opened with the pointer; a lone panel's tab menu has
  no key (its actions do: Ctrl+W, Shift+`, the pop-out button). The timecode field opens with a click, or from the
  keyboard once the timecode has the focus.
- Applying a transition from the Effects panel puts it on both ends of each selected clip: Wabcut has no edit-point
  selection yet. Dragging it onto an edit point works as before.
- The Library's thumbnail slider is in the icon view only (the Freeform view's card size is an edit with its own undo
  step). Ctrl+Delete and the next/previous field keys of Premiere's Project panel still do nothing.
- On Home, Ctrl+N is New project; in the editor it stays New Sequence, as in Premiere. The History panel still has no
  Delete or Clear.
- Only project files go to the Wabcut that is running; a launch with media files starts its own Wabcut. On macOS the
  menus have no Option+letter keys (Option+letter types accented characters there).
- Linux and macOS are preview builds: nobody has tried the new Home, the menu bar, the single instance or the window
  placement on a real Linux or macOS machine yet. The [0.2 known issues](release-notes-0.2.md#known-issues) still
  apply.
- Not code-signed; no auto-update. Real local-AI quality and live cloud-provider runs are not benchmarked.

## Privacy

Nothing leaves your computer unless you turn on a cloud feature with your own key, and every cloud call is shown with its
cost first. Redacted regions and bleeped words are never uploaded in clear. Crash reports stay local until you choose
to share them, and never contain API keys.
