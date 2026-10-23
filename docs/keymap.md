# Keymap: Premiere Pro shortcuts

Owner: Keymap engineer. Code: `src/keymap/` (`wabcut_keymap`, static, no Qt). Tests: `tests/keymap/`.

Wabcut uses Premiere Pro's default keyboard shortcuts, panel by panel and OS by OS, so a Premiere editor's hands
work unchanged. The default tables are `src/keymap/keymaps/premiere-windows.json` and `premiere-macos.json`.
They are two separate tables; the macOS one is never produced by swapping Ctrl and Cmd. Linux uses the Windows
table. Both JSON files are the editable source of truth and are compiled into the library at build time.

## Sources

| Key in the JSON `sources` | Source | Date |
|---|---|---|
| `adobe-2026` | Adobe Help Center, [Default keyboard shortcuts](https://helpx.adobe.com/premiere/desktop/get-started/keyboard-shortcuts/default-keyboard-shortcuts.html) (Premiere desktop). The primary source: every row on it is transcribed. | last updated 2026-01-07, fetched 2026-10-11 |
| `adobe-2026-assign` | Adobe Help Center, [Assign multiple keyboard shortcuts to a command](https://helpx.adobe.com/premiere/desktop/get-started/keyboard-shortcuts/assign-multiple-keyboard-shortcuts-to-a-command.html). Its screenshot caption says Shift+A is assigned to the Track Select Backward Tool. | last updated 2026-01-07 |
| `adobe-2024` | Adobe Help Center, Keyboard shortcuts in Premiere Pro, [archived copy](https://web.archive.org/web/20250102043957/https://helpx.adobe.com/premiere-pro/using/keyboard-shortcuts.html). It has the transport and timeline rows (J/K/L, Space, arrows, Q/W, D, Paste Insert) that the 2026 page dropped. The old URL now redirects to the 2026 page. | last updated 2024-10-01 |
| `adobe-2017` | Adobe Help Center, Print Premiere Pro CC shortcuts, archived copies for [Windows](https://web.archive.org/web/20181022172806/https://helpx.adobe.com/premiere-pro/using/print-premiere-pro-cc-shortcuts-windows.html) and [macOS](https://web.archive.org/web/20181022154054/https://helpx.adobe.com/premiere-pro/using/print-premiere-pro-cc-shortcuts-mac.html). These are the last Adobe pages with a Tools table; they also list Zoom to Sequence. | last modified 2017-10-23 |
| `premiere-kys` | Premiere's own default key sets: [Windows `Premiere_default_keys.kys`](https://github.com/michaelmickspad/PremiereProWithAutoHotKey/blob/152b37f497d1dbe6bab7c457cf2ba623dc751412/config/Premiere_default_keys.kys) and [macOS `Default Adobe.kys`](https://github.com/EthanManners/AdobeToKDEN-Keyboard-translator/blob/ed23daa0d2d73528d97f50e8e45de0b4e0a200eb/Default%20Adobe.kys). The source for the Type Tool (T), which no Adobe table lists, and for macOS Trim Edit and Make Subsequence, where the 2026 page disagrees with the shipped key set (lead decision, see below). Also used to cross-check everything else. | Premiere version not recorded in the files |
| `wabcut` | Wabcut's own default where Premiere has none: the sequence tab cycle (Ctrl+Tab, M5i), Settings on Windows (Ctrl+,). | – |

The 2026 page has no Tools, playback or ripple-trim tables, so those rows come from the older Adobe pages. Every
binding in the JSON names its source.

## Verification notes

**Facts the lead verified.** All of them match the 2026 page except the two macOS rows the lead later decided to
take from Premiere's shipped key set (see below). `tests/keymap/test_default_tables.cpp` asserts each one per OS
and per scope:

| Fact | Windows | macOS | Test |
|---|---|---|---|
| I, O, X, M, `,`, `.`, `;`, `'`, F and Enter are the same on both OSes | same | same | `Verified: I O X M , . ; ' F and Enter …` |
| Clear In / Clear Out / Clear In and Out | Ctrl+Shift+I / O / X | Opt+I / O / X | `Verified: Clear In/Out/In-and-Out …` |
| Trim Edit | Shift+T | Shift+T (shipped key set; the page says Cmd+T) | `Trim Edit and Make Subsequence follow the shipped key sets …` |
| Make Subsequence | Shift+U | Shift+U (shipped key set; the page says Cmd+U) | same test |
| Previous in Sequence | Ctrl+Shift+; | Opt+; | same test |
| Ctrl/Cmd+L | Link (Clip menu, Application); Loop in Audio Track Mixer, Metadata and Effect Controls | same with Cmd | `Verified: Ctrl/Cmd+L …` |
| Ctrl/Cmd+R | Speed/Duration (Application); Show Rulers in Program Monitor | same with Cmd | `Verified: Ctrl/Cmd+R …` |
| Ripple Delete | Shift+Delete (Edit menu); Alt+Backspace in Timeline | Shift+Forward Delete; Opt+Delete in Timeline | `Verified: Ripple Delete …` |

**The J/K/L/C/V/Q/W/B/N rows.** None of the nine is on the 2026 page. Each was checked against the older Adobe
pages and against Premiere's default key sets. The test `Checked against Adobe: J K L C V Q W B N` asserts them.

| Key | Command | Adobe evidence | Default .kys (Win / mac) | Result |
|---|---|---|---|---|
| J | Shuttle Left | 2024 page and 2017 print pages: "Shuttle Left \| J \| J" | J / J | verified |
| K | Shuttle Stop | same pages: "Shuttle Stop \| K \| K" | K / K | verified |
| L | Shuttle Right | same pages: "Shuttle Right \| L \| L" | L / L | verified |
| C | Razor Tool | 2017 print pages, Tools: "Razor Tool \| C" | C / C | verified |
| V | Selection Tool | 2017 print pages, Tools: "Selection Tool \| V" | V / V | verified |
| Q | Ripple Trim Previous Edit to Playhead | 2024 page and 2017 print pages | Q / Q | verified |
| W | Ripple Trim Next Edit to Playhead | 2024 page and 2017 print pages | W / W | verified |
| B | Ripple Edit Tool | 2017 print pages, Tools: "Ripple Edit Tool \| B" | B / B | verified |
| N | Rolling Edit Tool | 2017 print pages, Tools: "Rolling Edit Tool \| N" | N / N | verified |

**Cross-check against Premiere's default key sets.** Every command with a `.kys` identity was looked up in the
default Windows and macOS key sets listed under Sources, and its decoded shortcut was compared with the table:

- Windows: 217 of 218 match. The remaining one, Send to Media Encoder, is missing from that older key set; a
  Premiere 26.0 Windows key set confirms `cmd.file.export.sendtoqueue` = Alt+Shift+M.
- macOS: all 218 match. Two rows disagree with the macOS column of Adobe's 2026 page, and the table follows the
  shipped key set for both:
  - **Trim Edit**: the page says Cmd+T; the shipped key set says Shift+T, as on Windows. On the page, Cmd+T is also
    Graphics > New Layer > Text.
  - **Make Subsequence**: the page says Cmd+U; the shipped key set says Shift+U, as on Windows. On the page, Cmd+U
    is also Clip > Make Subclip.

  **Lead decision (task K.1):** Wabcut must behave like the Premiere that users actually have, so it follows the
  shipped default key sets rather than the help page wherever they disagree. Both page values look like Shift
  turned into Cmd, and they would have created two conflicts on macOS. With the shipped values, both default tables
  are conflict-free (a test asserts it), and the `acknowledgedConflicts` mechanism stays available for future
  tables but is empty in both.

**Other things found on the page:**

- *Maximize Frame* shows "Shift +" with no key on the 2026 page; the backtick was lost in rendering. The 2024 page
  ("Maximize or Restore Active Frame | Shift + `") and the key sets give Shift+`.
- *Nudge Clip Selection* in the Timeline is Alt+arrows on Windows but **Cmd**+arrows on macOS, not Opt. The brief
  said "Alt/Opt+arrows"; the table follows the page and the key sets agree. On macOS, Opt+Left/Right is Trim
  Backward/Forward in Premiere (in the table since M5i).
- *Paste Insert* (Ctrl+Shift+V / Shift+Cmd+V) is missing from the 2026 Edit table. It is taken from the 2024 page.
- *Go to Next/Previous Caption Segment* are listed under the Sequence menu, so they are Application-scope here.
  Premiere stores them in `context.timeline`. The `.kys` mapping accounts for this.
- *Toggle Trim Type* (since M5i) follows the shipped key sets: Ctrl+Shift+T on Windows and Control+T on macOS, in
  `context.global`. The 2024 page's Shift+T would shadow Trim Edit in the Timeline, so it is not used (M5j).
- On macOS, *Meter Input(s) Only* uses the physical Control key (Ctrl+Shift+I in Adobe's macOS column). Qt calls
  that key MetaModifier, and the table stores it that way.

## Key names and modifiers

Chord text in the JSON is canonical `formatKeyChord(…, ChordFormat::Text)` output, and a test enforces this.
Key codes and modifier bits are Qt's, so the app passes `QKeyEvent::key()` and `modifiers()` straight in.

| In the JSON | Qt | Adobe Windows label | Adobe macOS label |
|---|---|---|---|
| `Backspace` | `Key_Backspace` | Backspace | Delete (⌫) |
| `Delete` | `Key_Delete` | Delete | Forward Delete (⌦) |
| `Enter` (Windows) / `Return` (macOS) | `Key_Return`, the main key | Enter | Return or Enter |
| `NumEnter` | `Key_Enter`, keypad Enter | — | — |
| `Keypad+5` | `Key_5` + `KeypadModifier` | Num 5 | — |
| Windows `Ctrl` / macOS `Cmd` | `ControlModifier` | Ctrl | Cmd (⌘) |
| macOS `Ctrl` | `MetaModifier` | — | Ctrl (⌃) |
| Windows `Alt` / macOS `Opt` | `AltModifier` | Alt | Opt (⌥) |
| Windows/Linux `Meta` | `MetaModifier` | Win key | — |

Qt reports ⌘ as ControlModifier and ⌃ as MetaModifier on macOS. `parseKeyChord` therefore reads "Cmd" as
Control and "Ctrl" as Meta on macOS, and rejects Cmd/Opt on Windows/Linux so the two tables cannot be mixed up.
`ChordFormat::Native` renders menus and tooltips: `Ctrl+Shift+K` on Windows, `⇧⌘K` on macOS.

## Command table

All 278 commands. *Source*: 2026, 2024 and 2017 are the Adobe pages above; † marks the 2026 "assign multiple"
page; `.kys` means Premiere's shipped default key sets. *Premiere .kys item* is the `(context, command name)` pair
the importer maps from; *(not mapped)* means no verified pair, so `.kys` import skips the command.

| Command | Wabcut id | Scope | Windows | macOS | Source | Premiere .kys item |
|---|---|---|---|---|---|---|
| New Project… | `file.newProject` | Application | `Ctrl+Alt+N` | `Opt+Cmd+N` | 2026 | `global / cmd.file.new.project` |
| New Sequence… | `file.newSequence` | Application | `Ctrl+N` | `Cmd+N` | 2026 | `global / cmd.file.new.sequence` |
| New Sequence From Clip [^21] | `file.newSequenceFromClip` | Application | | | Wabcut | (not mapped) |
| Open Project… | `file.openProject` | Application | `Ctrl+O` | `Cmd+O` | 2026 | `global / cmd.file.openproject` |
| Close [^uxs] | `file.close` | Application | `Ctrl+W` | `Cmd+W` | 2026 | `global / cmd.file.closepanel` |
| Close Project | `file.closeProject` | Application | `Ctrl+Shift+W` | `Shift+Cmd+W` | 2026 | `global / cmd.file.close` |
| Save | `file.save` | Application | `Ctrl+S` | `Cmd+S` | 2026 | `global / cmd.file.save` |
| Save As… | `file.saveAs` | Application | `Ctrl+Shift+S` | `Shift+Cmd+S` | 2026 | `global / cmd.file.saveas` |
| Save a Copy… | `file.saveCopy` | Application | `Ctrl+Alt+S` | `Opt+Cmd+S` | 2026 | `global / cmd.file.savecopy` |
| Import from Media Browser | `file.importFromMediaBrowser` | Application | `Ctrl+Alt+I` | `Opt+Cmd+I` | 2026 | `global / cmd.file.importfrombrowser` |
| Import… | `file.import` | Application | `Ctrl+I` | `Cmd+I` | 2026 | `global / cmd.file.import` |
| Export Media… | `file.exportMedia` | Application | `Ctrl+M` | `Cmd+M` | 2026 | `global / cmd.file.export.movie` |
| Export Frame… [^22] | `file.exportFrame` | Application | `Ctrl+Shift+E` | `Shift+E` | .kys | `global / cmd.export.frame` |
| Send to Media Encoder | `file.sendToMediaEncoder` | Application | `Alt+Shift+M` | `Opt+Shift+M` | 2026 | `global / cmd.file.export.sendtoqueue` |
| Get Media File Properties for Selection… | `file.getProperties` | Application | `Ctrl+Shift+H` | `Shift+Cmd+H` | 2026 | `global / cmd.file.properties.selection` |
| Exit | `file.exit` | Application | `Ctrl+Q` | `Cmd+Q` | 2026 | `global / cmd.file.exit` |
| Undo | `edit.undo` | Application | `Ctrl+Z` | `Cmd+Z` | 2026 | `global / cmd.edit.undo` |
| Redo | `edit.redo` | Application | `Ctrl+Shift+Z` | `Shift+Cmd+Z` | 2026 | `global / cmd.edit.redo` |
| Cut | `edit.cut` | Application | `Ctrl+X` | `Cmd+X` | 2026 | `global / cmd.edit.cut` |
| Copy | `edit.copy` | Application | `Ctrl+C` | `Cmd+C` | 2026 | `global / cmd.edit.copy` |
| Paste | `edit.paste` | Application | `Ctrl+V` | `Cmd+V` | 2026 | `global / cmd.edit.paste` |
| Paste Insert [^1] | `edit.pasteInsert` | Application | `Ctrl+Shift+V` | `Shift+Cmd+V` | 2024 | `global / cmd.edit.pasteinsert` |
| Paste Attributes… | `edit.pasteAttributes` | Application | `Ctrl+Alt+V` | `Opt+Cmd+V` | 2026 | `global / cmd.edit.pasteattributes` |
| Remove Attributes… [^19] | `edit.removeAttributes` | Application | | | Wabcut | (not mapped) |
| Clear | `edit.clear` | Application | `Delete` | `Delete` | 2026 | `global / cmd.edit.clear` |
| Ripple Delete | `edit.rippleDelete` | Application | `Shift+Delete` | `Shift+Delete` | 2026 | `global / cmd.edit.rippledelete` |
| Duplicate | `edit.duplicate` | Application | `Ctrl+Shift+/` | `Shift+Cmd+/` | 2026 | `global / cmd.edit.duplicate` |
| Select All | `edit.selectAll` | Application | `Ctrl+A` | `Cmd+A` | 2026 | `global / cmd.edit.selectall` |
| Deselect All | `edit.deselectAll` | Application | `Ctrl+Shift+A` | `Shift+Cmd+A` | 2026 | `global / cmd.edit.deselectall` |
| Find… | `edit.find` | Application | `Ctrl+F` | `Cmd+F` | 2026 | `global / cmd.edit.find` |
| Edit Original | `edit.editOriginal` | Application | `Ctrl+E` | `Cmd+E` | 2026 | `global / cmd.edit.editoriginal` |
| Keyboard Shortcuts… | `edit.keyboardShortcuts` | Application | `Ctrl+Alt+K` | `Opt+Cmd+K` | 2026 | `global / cmd.edit.keyboardshortcuts` |
| Settings… [^ux] | `edit.preferences` | Application | `Ctrl+,` | `Cmd+,` | Wabcut / .kys | `global / cmd.edit.preferences.general` |
| Preferences: Media… [^21] | `edit.preferencesMedia` | Application | | | .kys | `global / cmd.edit.preferences.media` |
| Preferences: Timeline… [^21] | `edit.preferencesTimeline` | Application | | | .kys | `global / cmd.edit.preferences.timeline` |
| Make Subclip… | `clip.makeSubclip` | Application | `Ctrl+U` | `Cmd+U` | 2026 | `global / cmd.clip.makesubclip` |
| Audio Channels… | `clip.audioChannels` | Application | `Shift+G` | `Shift+G` | 2026 | `global / cmd.clip.audiooptions.sourcechannelmappings` |
| Audio Gain… | `clip.audioGain` | Application | `G` | `G` | 2026 | `global / cmd.clip.audiooptions.gain` |
| Speed/Duration… | `clip.speedDuration` | Application | `Ctrl+R` | `Cmd+R` | 2026 | `global / cmd.clip.speed` |
| Insert | `clip.insert` | Application | `,` | `,` | 2026 | `global / cmd.clip.insert` |
| Overwrite | `clip.overwrite` | Application | `.` | `.` | 2026 | `global / cmd.clip.overlay` |
| Enable | `clip.enable` | Application | `Shift+E` | `Shift+Cmd+E` | 2026 | `global / cmd.clip.enable` |
| Link | `clip.link` | Application | `Ctrl+L` | `Cmd+L` | 2026 | `global / cmd.clip.linkaudioandvideo` |
| Group | `clip.group` | Application | `Ctrl+G` | `Cmd+G` | 2026 | `global / cmd.clip.group` |
| Ungroup | `clip.ungroup` | Application | `Ctrl+Shift+G` | `Shift+Cmd+G` | 2026 | `global / cmd.clip.ungroup` |
| Scale to Frame Size [^21] | `clip.scaleToFrameSize` | Application | | | .kys | `global / cmd.clip.scaletoframesize` |
| Set to Frame Size [^21] | `clip.setToFrameSize` | Application | | | .kys | `global / cmd.clip.settoframesize` |
| Set to Fill Frame [^21] | `clip.setToFillFrame` | Application | | | .kys | `global / cmd.clip.fillframe` |
| Frame Hold Options… [^21] | `clip.frameHoldOptions` | Application | | | .kys | `global / cmd.clip.videooptions.frameholdoptions` |
| Add Frame Hold [^21] | `clip.addFrameHold` | Application | | | .kys | `global / cmd.clip.videooptions.addframehold` |
| Insert Frame Hold Segment [^21] | `clip.insertFrameHoldSegment` | Application | | | .kys | `global / cmd.clip.videooptions.insertframeholdsegment` |
| Merge Clips… [^21] | `clip.mergeClips` | Application | | | .kys | `global / cmd.clip.merge` |
| Interpret Footage… [^21] | `clip.interpretFootage` | Application | | | .kys | `global / cmd.file.interpretfootage` |
| Modify Timecode… [^21] | `clip.modifyTimecode` | Application | | | .kys | `global / cmd.file.timecode` |
| Replace Footage… [^21] | `clip.replaceFootage` | Application | | | .kys | `global / cmd.clip.replacefootage` |
| Make Offline [^21] | `clip.makeOffline` | Application | | | Wabcut | (not mapped) |
| Reveal in Project [^uxt] | `clip.revealInProject` | Application | | | Wabcut | (not mapped) |
| Slow-mo at Playhead [^20] | `moments.slowMo` | Application | | | Wabcut | (not mapped) |
| Punch-in at Playhead [^20] | `moments.punchIn` | Application | | | Wabcut | (not mapped) |
| Render Effects In to Out | `sequence.renderInToOut` | Application | `Enter` | `Return` | 2026 | `global / cmd.sequence.preview` |
| Match Frame | `sequence.matchFrame` | Application | `F` | `F` | 2026 | `global / cmd.sequence.matchframe` |
| Reverse Match Frame | `sequence.reverseMatchFrame` | Application | `Shift+R` | `Shift+R` | 2026 | `global / cmd.sequence.reversematchframe` |
| Add Edit | `sequence.addEdit` | Application | `Ctrl+K` | `Cmd+K` | 2026 | `global / cmd.sequence.razorateditline` |
| Add Edit to All Tracks | `sequence.addEditToAllTracks` | Application | `Ctrl+Shift+K` | `Shift+Cmd+K` | 2026 | `global / cmd.sequence.razorateditline.all` |
| Trim Edit [^2] | `sequence.trimEdit` | Application | `Shift+T` | `Shift+T` | 2026 (Windows), .kys (macOS) | `global / cmd.placeholder` |
| Toggle Trim Type [^15] | `sequence.toggleTrimType` | Application | `Ctrl+Shift+T` | `Ctrl+T` | .kys | `global / cmd.sequence.toggletrimtype` |
| Trim Backward [^16] | `sequence.trimBackward` | Application | `Ctrl+Left` | `Opt+Left` | .kys | `global / cmd.sequence.trimbackward` |
| Trim Forward [^16] | `sequence.trimForward` | Application | `Ctrl+Right` | `Opt+Right` | .kys | `global / cmd.sequence.trimforward` |
| Trim Backward Many [^16] | `sequence.trimBackwardMany` | Application | `Ctrl+Shift+Left` | `Opt+Shift+Left` | .kys | `global / cmd.sequence.trimbackwardmany` |
| Trim Forward Many [^16] | `sequence.trimForwardMany` | Application | `Ctrl+Shift+Right` | `Opt+Shift+Right` | .kys | `global / cmd.sequence.trimforwardmany` |
| Extend Selected Edit to Playhead | `sequence.extendEditToPlayhead` | Application | `E` | `E` | 2026 | `global / cmd.sequence.extendselectededittoplayhead` |
| Apply Video Transition | `sequence.applyVideoTransition` | Application | `Ctrl+D` | `Cmd+D` | 2026 | `global / cmd.sequence.applydefaultvideotransition` |
| Apply Audio Transition | `sequence.applyAudioTransition` | Application | `Ctrl+Shift+D` | `Shift+Cmd+D` | 2026 | `global / cmd.sequence.applydefaultaudiotransition` |
| Apply Default Transitions to Selection | `sequence.applyDefaultTransitions` | Application | `Shift+D` | `Shift+D` | 2026 | `global / cmd.sequence.applydefaulttransitions` |
| Lift | `sequence.lift` | Application | `;` | `;` | 2026 | `global / cmd.sequence.lift` |
| Extract | `sequence.extract` | Application | `'` | `'` | 2026 | `global / cmd.sequence.extract` |
| Zoom In | `sequence.zoomIn` | Application | `=` | `=` | 2026 | `global / cmd.zoom.in` |
| Zoom Out | `sequence.zoomOut` | Application | `-` | `-` | 2026 | `global / cmd.zoom.out` |
| Zoom to Sequence [^3] | `sequence.zoomToSequence` | Application | `\` | `\` | 2017 | `global / cmd.tlnav.zoomto.sequence` |
| Next in Sequence | `sequence.nextInSequence` | Application | `Shift+;` | `Shift+;` | 2026 | `global / cmd.sequence.findnextsequencegap` |
| Previous in Sequence | `sequence.previousInSequence` | Application | `Ctrl+Shift+;` | `Opt+;` | 2026 | `global / cmd.sequence.findprevioussequencegap` |
| Snap in Timeline | `sequence.snap` | Application | `S` | `S` | 2026 | `global / cmd.sequence.snap` |
| Make Subsequence [^4] | `sequence.makeSubsequence` | Application | `Shift+U` | `Shift+U` | 2026 (Windows), .kys (macOS) | `global / cmd.sequence.makesubsequence` |
| Make Subsequence from In to Out [^21] | `sequence.makeSubsequenceFromInToOut` | Application | | | .kys | `global / cmd.sequence.makesubsequencefromintoout` |
| Sequence Settings… [^21] | `sequence.settings` | Application | | | .kys | `global / cmd.sequence.sequencesettingsgeneral` |
| Replace With Clip: From Source Monitor [^21] | `sequence.replaceFromSourceMonitor` | Application | | | .kys | `global / clip.replaceclip.fromsourcemonitor` |
| Replace With Clip: From Source Monitor, Match Frame [^21] | `sequence.replaceFromSourceMonitorMatchFrame` | Application | | | .kys | `global / clip.replaceclip.fromsourcemonitor.matchframe` |
| Replace With Clip: From Bin [^21] | `sequence.replaceFromBin` | Application | | | .kys | `global / clip.replaceclip.frombin` |
| Join Through Edits [^21] | `sequence.joinThroughEdits` | Application | | | .kys | `timeline / cmd.sequence.jointhroughedits` |
| Next in Track [^21] | `sequence.nextInTrack` | Application | | | .kys | `global / cmd.sequence.findnexttrackgap` |
| Previous in Track [^21] | `sequence.previousInTrack` | Application | | | .kys | `global / cmd.sequence.findprevioustrackgap` |
| Close Gap [^21] | `sequence.closeGap` | Application | | | .kys | `global / cmd.sequence.close.gaps` |
| Add New Caption Track | `sequence.addCaptionTrack` | Application | `Ctrl+Alt+A` | `Opt+Cmd+A` | 2026 | `global / cmd.caption.add.track` |
| Add Caption at Playhead | `sequence.addCaptionAtPlayhead` | Application | `Ctrl+Alt+C` | `Opt+Cmd+C` | 2026 | `global / cmd.caption.add.trackitem` |
| Go to Next Caption Segment [^5] | `sequence.nextCaptionSegment` | Application | `Ctrl+Alt+Down` | `Opt+Cmd+Down` | 2026 | `timeline / cmd.timeline.goto.next.caption.trackitem` |
| Go to Previous Caption Segment [^6] | `sequence.previousCaptionSegment` | Application | `Ctrl+Alt+Up` | `Opt+Cmd+Up` | 2026 | `timeline / cmd.timeline.goto.prev.caption.trackitem` |
| Ripple Trim Previous Edit to Playhead | `sequence.rippleTrimPreviousToPlayhead` | Application | `Q` | `Q` | 2024 | `global / cmd.sequence.rippletrimpreviousedittoplayhead` |
| Ripple Trim Next Edit to Playhead | `sequence.rippleTrimNextToPlayhead` | Application | `W` | `W` | 2024 | `global / cmd.sequence.rippletrimnextedittoplayhead` |
| Select Clip at Playhead | `sequence.selectClipAtPlayhead` | Application | `D` | `D` | 2024 | `global / cmd.tlnav.select.clip.at.playhead` |
| Selection Follows Playhead (0.2, a toggle) | `sequence.selectionFollowsPlayhead` | Application | (none) | (none) | Wabcut | (not mapped) |
| Show Video Fade Handles (0.2, a toggle) | `timeline.showVideoFadeHandles` | Application | (none) | (none) | Wabcut | (not mapped) |
| Show Audio Fade Handles (0.2, a toggle) | `timeline.showAudioFadeHandles` | Application | (none) | (none) | Wabcut | (not mapped) |
| Select Camera 1 [^17] | `multicam.selectCamera1` | Application | `1` | `1` | .kys | `global / cmd.multicam.choosenocut.camera1` |
| Select Camera 2 | `multicam.selectCamera2` | Application | `2` | `2` | .kys | `global / cmd.multicam.choosenocut.camera2` |
| Select Camera 3 | `multicam.selectCamera3` | Application | `3` | `3` | .kys | `global / cmd.multicam.choosenocut.camera3` |
| Select Camera 4 | `multicam.selectCamera4` | Application | `4` | `4` | .kys | `global / cmd.multicam.choosenocut.camera4` |
| Select Camera 5 | `multicam.selectCamera5` | Application | `5` | `5` | .kys | `global / cmd.multicam.choosenocut.camera5` |
| Select Camera 6 | `multicam.selectCamera6` | Application | `6` | `6` | .kys | `global / cmd.multicam.choosenocut.camera6` |
| Select Camera 7 | `multicam.selectCamera7` | Application | `7` | `7` | .kys | `global / cmd.multicam.choosenocut.camera7` |
| Select Camera 8 | `multicam.selectCamera8` | Application | `8` | `8` | .kys | `global / cmd.multicam.choosenocut.camera8` |
| Select Camera 9 | `multicam.selectCamera9` | Application | `9` | `9` | .kys | `global / cmd.multicam.choosenocut.camera9` |
| Cut to Camera 1 | `multicam.cutToCamera1` | Application | `Ctrl+1` | `Ctrl+1` | .kys | `global / cmd.multicam.choose.camera.1` |
| Cut to Camera 2 | `multicam.cutToCamera2` | Application | `Ctrl+2` | `Ctrl+2` | .kys | `global / cmd.multicam.choose.camera.2` |
| Cut to Camera 3 | `multicam.cutToCamera3` | Application | `Ctrl+3` | `Ctrl+3` | .kys | `global / cmd.multicam.choose.camera.3` |
| Cut to Camera 4 | `multicam.cutToCamera4` | Application | `Ctrl+4` | `Ctrl+4` | .kys | `global / cmd.multicam.choose.camera.4` |
| Cut to Camera 5 | `multicam.cutToCamera5` | Application | `Ctrl+5` | `Ctrl+5` | .kys | `global / cmd.multicam.choose.camera.5` |
| Cut to Camera 6 | `multicam.cutToCamera6` | Application | `Ctrl+6` | `Ctrl+6` | .kys | `global / cmd.multicam.choose.camera.6` |
| Cut to Camera 7 | `multicam.cutToCamera7` | Application | `Ctrl+7` | `Ctrl+7` | .kys | `global / cmd.multicam.choose.camera.7` |
| Cut to Camera 8 | `multicam.cutToCamera8` | Application | `Ctrl+8` | `Ctrl+8` | .kys | `global / cmd.multicam.choose.camera.8` |
| Cut to Camera 9 | `multicam.cutToCamera9` | Application | — | `Ctrl+9` | .kys | `global / cmd.multicam.choose.camera.9` |
| Toggle Multi-Camera View | `multicam.toggleView` | Application | `Shift+0` | `Shift+0` | .kys | `global / cmd.multicam.toggle.multicam.view` |
| Mark In | `marker.markIn` | Application | `I` | `I` | 2026 | `global / cmd.common.setin` |
| Mark Out | `marker.markOut` | Application | `O` | `O` | 2026 | `global / cmd.common.setout` |
| Mark Clip | `marker.markClip` | Application | `X` | `X` | 2026 | `global / cmd.marker.setsequenceinoutmarkeraroundtargetclip` |
| Mark Selection | `marker.markSelection` | Application | `/` | `/` | 2026 | `global / cmd.marker.setsequenceinoutmarkeraroundselection.out` |
| Go to In | `marker.goToIn` | Application | `Shift+I` | `Shift+I` | 2026 | `global / cmd.goto.in` |
| Go to Out | `marker.goToOut` | Application | `Shift+O` | `Shift+O` | 2026 | `global / cmd.goto.out` |
| Clear In | `marker.clearIn` | Application | `Ctrl+Shift+I` | `Opt+I` | 2026 | `global / cmd.clear.in` |
| Clear Out | `marker.clearOut` | Application | `Ctrl+Shift+O` | `Opt+O` | 2026 | `global / cmd.clear.out` |
| Clear In and Out | `marker.clearInAndOut` | Application | `Ctrl+Shift+X` | `Opt+X` | 2026 | `global / cmd.clear.inandout` |
| Add Marker | `marker.add` | Application | `M` | `M` | 2026 | `global / cmd.set.marker` |
| Go to Next Marker | `marker.goToNext` | Application | `Shift+M` | `Shift+M` | 2026 | `global / cmd.marker.gotomarker.next` |
| Go to Previous Marker | `marker.goToPrevious` | Application | `Ctrl+Shift+M` | `Shift+Cmd+M` | 2026 | `global / cmd.marker.gotomarker.previous` |
| Clear Selected Marker | `marker.clearSelected` | Application | `Ctrl+Alt+M` | `Opt+M` | 2026 | `global / cmd.marker.clearmarker.current` |
| Clear All Markers | `marker.clearAll` | Application | `Ctrl+Alt+Shift+M` | `Opt+Cmd+M` | 2026 | `global / cmd.marker.clearmarker.all` |
| New Layer: Text | `graphics.newTextLayer` | Application | `Ctrl+T` | `Cmd+T` | 2026 | `global / cmd.graphics.add.text` |
| New Layer: Rectangle | `graphics.newRectangleLayer` | Application | `Ctrl+Alt+R` | `Opt+Cmd+R` | 2026 | `global / cmd.graphics.add.shape.rectangle` |
| New Layer: Ellipse | `graphics.newEllipseLayer` | Application | `Ctrl+Alt+E` | `Opt+Cmd+E` | 2026 | `global / cmd.graphics.add.shape.ellipse` |
| Bring to Front | `graphics.bringToFront` | Application | `Ctrl+Shift+]` | `Shift+Cmd+]` | 2026 | `global / cmd.graphics.move.graphic.layer.to.top` |
| Bring Forward | `graphics.bringForward` | Application | `Ctrl+]` | `Cmd+]` | 2026 | `global / cmd.graphics.move.graphic.layer.up` |
| Send Backward | `graphics.sendBackward` | Application | `Ctrl+[` | `Cmd+[` | 2026 | `global / cmd.graphics.move.graphic.layer.down` |
| Send to Back | `graphics.sendToBack` | Application | `Ctrl+Shift+[` | `Shift+Cmd+[` | 2026 | `global / cmd.graphics.move.graphic.layer.to.bottom` |
| Select Next Layer | `graphics.selectNextLayer` | Application | `Ctrl+Alt+]` | `Opt+Cmd+]` | 2026 | `global / cmd.graphics.select.next.graphic.layer` |
| Select Previous Layer | `graphics.selectPreviousLayer` | Application | `Ctrl+Alt+[` | `Opt+Cmd+[` | 2026 | `global / cmd.graphics.select.previous.graphic.layer` |
| Workspace: All Panels | `window.workspace1` | Application | `Alt+Shift+1` | `Opt+Shift+1` | 2026 | `global / cmd.window.user.workspace.0` |
| Workspace: Assembly | `window.workspace2` | Application | `Alt+Shift+2` | `Opt+Shift+2` | 2026 | `global / cmd.window.user.workspace.1` |
| Workspace: Audio | `window.workspace3` | Application | `Alt+Shift+3` | `Opt+Shift+3` | 2026 | `global / cmd.window.user.workspace.2` |
| Workspace: Captions and Graphics | `window.workspace4` | Application | `Alt+Shift+4` | `Opt+Shift+4` | 2026 | `global / cmd.window.user.workspace.3` |
| Workspace: Color | `window.workspace5` | Application | `Alt+Shift+5` | `Opt+Shift+5` | 2026 | `global / cmd.window.user.workspace.4` |
| Workspace: Editing | `window.workspace6` | Application | `Alt+Shift+6` | `Opt+Shift+6` | 2026 | `global / cmd.window.user.workspace.5` |
| Workspace: Effects | `window.workspace7` | Application | `Alt+Shift+7` | `Opt+Shift+7` | 2026 | `global / cmd.window.user.workspace.6` |
| Workspace: Essential | `window.workspace8` | Application | `Alt+Shift+8` | `Opt+Shift+8` | 2026 | `global / cmd.window.user.workspace.7` |
| Workspace: Learning | `window.workspace9` | Application | `Alt+Shift+9` | `Opt+Shift+9` | 2026 | `global / cmd.window.user.workspace.8` |
| Reset to Saved Layout [^uxs] | `window.resetToSavedLayout` | Application | `Alt+Shift+0` | `Opt+Shift+0` | 2026 | `global / cmd.window.workspace.revert` |
| Maximize Frame [^7] [^uxs] | `window.maximizeFrame` | Application | `` Shift+` `` | `` Shift+` `` | 2026 | `global / cmd.toggle.maximize.focused.frame` |
| Toggle Full Screen [^22] | `monitor.toggleFullScreen` | Application | `` Ctrl+` `` | `` Ctrl+` `` | .kys | `global / cmd.toggle.fullscreen.monitor` |
| Audio Clip Mixer | `window.audioClipMixer` | Application | `Shift+9` | `Shift+9` | 2026 | `global / uif.window.Audio Clip Mixer` |
| Audio Track Mixer | `window.audioTrackMixer` | Application | `Shift+6` | `Shift+6` | 2026 | `global / uif.window.Audio Mixers` |
| Effect Controls [^uxs] | `window.effectControls` | Application | `Shift+5` | `Shift+5` | 2026 | `global / uif.window.Effect Controls` |
| Effects | `window.effects` | Application | `Shift+7` | `Shift+7` | 2026 | `global / uif.window.Effects` |
| Media Browser | `window.mediaBrowser` | Application | `Shift+8` | `Shift+8` | 2026 | `global / uif.window.Media Browser` |
| Program Monitor | `window.programMonitor` | Application | `Shift+4` | `Shift+4` | 2026 | `global / uif.window.Program Monitors` |
| Projects | `window.projects` | Application | `Shift+1` | `Shift+1` | 2026 | `global / uif.window.Projects` |
| Source Monitor | `window.sourceMonitor` | Application | `Shift+2` | `Shift+2` | 2026 | `global / uif.window.Source Monitors` |
| Timeline | `window.timeline` | Application | `Shift+3` | `Shift+3` | 2026 | `global / uif.window.Timelines` |
| History [^21] | `window.history` | Application | | | .kys | `global / uif.window.History` |
| Selection Tool | `tool.selection` | Application | `V` | `V` | 2017 | `global / cmd.tools.01pointer` |
| Track Select Forward Tool [^8] | `tool.trackSelectForward` | Application | `A` | `A` | 2017 | `global / cmd.tools.02trackselectforward` |
| Track Select Backward Tool [^9] | `tool.trackSelectBackward` | Application | `Shift+A` | `Shift+A` | 2026† | `global / cmd.tools.02_5trackselectbackward` |
| Ripple Edit Tool | `tool.rippleEdit` | Application | `B` | `B` | 2017 | `global / cmd.tools.03ripple` |
| Rolling Edit Tool | `tool.rollingEdit` | Application | `N` | `N` | 2017 | `global / cmd.tools.04roll` |
| Rate Stretch Tool | `tool.rateStretch` | Application | `R` | `R` | 2017 | `global / cmd.tools.05ratestretch` |
| Razor Tool | `tool.razor` | Application | `C` | `C` | 2017 | `global / cmd.tools.06razor` |
| Slip Tool | `tool.slip` | Application | `Y` | `Y` | 2017 | `global / cmd.tools.07slip` |
| Slide Tool | `tool.slide` | Application | `U` | `U` | 2017 | `global / cmd.tools.08slide` |
| Pen Tool | `tool.pen` | Application | `P` | `P` | 2017 | `global / cmd.tools.09pen` |
| Hand Tool | `tool.hand` | Application | `H` | `H` | 2017 | `global / cmd.tools.10hand` |
| Zoom Tool | `tool.zoom` | Application | `Z` | `Z` | 2017 | `global / cmd.tools.11zoom` |
| Type Tool [^10] | `tool.type` | Application | `T` | `T` | .kys | `global / cmd.tools.12text` |
| Play/Stop | `transport.playStop` | Application | `Space` | `Space` | 2024 | `global / cmd.transport.toggleplay` |
| Shuttle Left | `transport.shuttleBackward` | Application | `J` | `J` | 2024 | `global / cmd.transport.shuttle.left` |
| Shuttle Stop | `transport.shuttleStop` | Application | `K` | `K` | 2024 | `global / cmd.transport.shuttle.stop` |
| Shuttle Right | `transport.shuttleForward` | Application | `L` | `L` | 2024 | `global / cmd.transport.shuttle.right` |
| Shuttle Slow Left | `transport.shuttleSlowBackward` | Application | `Shift+J` | `Shift+J` | 2024 | `global / cmd.transport.shuttle.slow.left` |
| Shuttle Slow Right | `transport.shuttleSlowForward` | Application | `Shift+L` | `Shift+L` | 2024 | `global / cmd.transport.shuttle.slow.right` |
| Step Back One Frame | `transport.stepBackward` | Application | `Left` | `Left` | 2024 | `global / cmd.transport.step.back` |
| Step Forward One Frame | `transport.stepForward` | Application | `Right` | `Right` | 2024 | `global / cmd.transport.step.forward` |
| Step Back Five Frames | `transport.stepBackwardFive` | Application | `Shift+Left` | `Shift+Left` | 2024 | `global / cmd.transport.step.back.five` |
| Step Forward Five Frames | `transport.stepForwardFive` | Application | `Shift+Right` | `Shift+Right` | 2024 | `global / cmd.transport.step.forward.five` |
| Go to Previous Edit Point | `transport.goToPreviousEdit` | Application | `Up` | `Up` | 2024 | `global / cmd.tlnav.prev.edit` |
| Go to Next Edit Point | `transport.goToNextEdit` | Application | `Down` | `Down` | 2024 | `global / cmd.tlnav.next.edit` |
| Go to Previous Edit Point on Any Track | `transport.goToPreviousEditAnyTrack` | Application | `Shift+Up` | `Shift+Up` | 2024 | `global / cmd.tlnav.prev.edit.any.track` |
| Go to Next Edit Point on Any Track | `transport.goToNextEditAnyTrack` | Application | `Shift+Down` | `Shift+Down` | 2024 | `global / cmd.tlnav.next.edit.any.track` |
| Go to Sequence-Clip Start | `transport.goToStart` | Application | `Home` | `Home` | 2024 | `global / cmd.transport.sequence.start` |
| Go to Sequence-Clip End | `transport.goToEnd` | Application | `End` | `End` | 2024 | `global / cmd.transport.sequence.end` |
| Go to Selected Clip Start | `transport.goToSelectedClipStart` | Application | `Shift+Home` | `Shift+Home` | 2024 | `global / cmd.transport.selectedclip.start` |
| Go to Selected Clip End | `transport.goToSelectedClipEnd` | Application | `Shift+End` | `Shift+End` | 2024 | `global / cmd.transport.selectedclip.end` |
| Play In to Out | `transport.playInToOut` | Application | `Ctrl+Shift+Space` | `Opt+K` | 2024 | `global / cmd.transport.playintoout` |
| Play Around | `transport.playAround` | Application | `Shift+K` | `Shift+K` | 2024 | `global / cmd.transport.playedit` |
| Loop [^22] | `monitor.loop` | Application | | | .kys | `global / cmd.monitor.loop` |
| Enable Transmit [^22] | `monitor.enableTransmit` | Application | | | .kys | `global / cmd.toggletransmit` |
| Toggle Crop Direct Manipulation [^22] | `monitor.toggleCropDirectManipulation` | Application | | | .kys | `global / cmd.monitor.toggle.crop.dm` |
| Record Voice-over [^18] | `record.toggle` | Application | | | `.kys` | `timeline / cmd.timeline.voiceover.track.record` |
| Start Voice-over Recording [^18] | `record.start` | Application | | | Wabcut | (not mapped) |
| Stop Voice-over Recording [^18] | `record.stop` | Application | | | Wabcut | (not mapped) |
| Show/Hide Tracks | `audioMixer.showHideTracks` | Audio Track Mixer | `Ctrl+Alt+T` | `Opt+Cmd+T` | 2026 | `audiomixer / cmd.audiomixer.showhidetracks` |
| Loop | `audioMixer.loop` | Audio Track Mixer | `Ctrl+L` | `Cmd+L` | 2026 | `audiomixer / cmd.audiomixer.loop` |
| Meter Input(s) Only [^11] | `audioMixer.meterInputsOnly` | Audio Track Mixer | `Ctrl+Shift+I` | `Ctrl+Shift+I` | 2026 | `audiomixer / cmd.audiomixer.meterinput` |
| Remove Selected Effect | `effectControls.removeSelectedEffect` | Effect Controls | `Backspace` | `Backspace` | 2026 | `effectcontrols / cmd.edit.clear` |
| Loop During Audio-Only Playback | `effectControls.loopAudioPlayback` | Effect Controls | `Ctrl+L` | `Cmd+L` | 2026 | `effectcontrols / cmd.effectcontrols.loop.audio` |
| New Custom Bin | `effects.newCustomBin` | Effects | `Ctrl+/` | `Cmd+/` | 2026 | `effects / cmd.effects.createcustomfolder` |
| Delete Custom Item | `effects.deleteCustomItem` | Effects | `Backspace` | `Backspace` | 2026 | `effects / cmd.edit.clear` |
| Step Backward [^uxs] | `history.stepBackward` | History | `Left` | `Left` | 2026 | `history / cmd.history.step.backward` |
| Step Forward [^uxs] | `history.stepForward` | History | `Right` | `Right` | 2026 | `history / cmd.history.step.forward` |
| Delete | `history.delete` | History | `Backspace` | `Backspace` | 2026 | `history / cmd.edit.clear` |
| Open in Source Monitor | `mediaBrowser.openInSourceMonitor` | Media Browser | `Shift+O` | `Shift+O` | 2026 | `mediabrowser / cmd.mediabrowser.openinsourcemonitor` |
| Select Directory List | `mediaBrowser.selectDirectoryList` | Media Browser | `Shift+Left` | `Shift+Left` | 2026 | `mediabrowser / cmd.mediabrowser.selectdirectorylist` |
| Select Media List | `mediaBrowser.selectMediaList` | Media Browser | `Shift+Right` | `Shift+Right` | 2026 | `mediabrowser / cmd.mediabrowser.selectmedialist` |
| Loop | `metadata.loop` | Metadata | `Ctrl+L` | `Cmd+L` | 2026 | `metadata_editor / cmd.metadataeditor.loop` |
| Play | `metadata.play` | Metadata | `Space` | `Space` | 2026 | `metadata_editor / cmd.metadataeditor.play` |
| Show Rulers | `programMonitor.showRulers` | Program Monitor | `Ctrl+R` | `Cmd+R` | 2026 | `program.monitor / cmd.monitor.rulers` |
| Show Guides | `programMonitor.showGuides` | Program Monitor | `Ctrl+;` | `Cmd+;` | 2026 | `program.monitor / cmd.monitor.guides` |
| Snap in Program Monitor | `programMonitor.snap` | Program Monitor | `Ctrl+Shift+;` | `Shift+Cmd+;` | 2026 | `program.monitor / cmd.monitor.snapping` |
| Lock Guides | `programMonitor.lockGuides` | Program Monitor | `Ctrl+Alt+Shift+R` | `Opt+Shift+Cmd+R` | 2026 | `program.monitor / cmd.monitor.lockguides` |
| Add Guide… [^22] | `programMonitor.addGuide` | Program Monitor | | | .kys | `program.monitor / cmd.monitor.addguide` |
| Clear Guides [^22] | `programMonitor.clearGuides` | Program Monitor | | | .kys | `program.monitor / cmd.monitor.removeguides` |
| Save Guides as Template… [^22] | `programMonitor.saveGuides` | Program Monitor | | | .kys | `program.monitor / cmd.monitor.saveguides` |
| Manage Guides… [^22] | `programMonitor.manageGuides` | Program Monitor | | | .kys | `program.monitor / cmd.monitor.manageguides` |
| Transparency Grid [^22] | `programMonitor.transparencyGrid` | Program Monitor | | | .kys | `program.monitor / cmd.monitor.toggletransparencygrid` |
| Transparency Grid [^22] | `sourceMonitor.transparencyGrid` | Source Monitor | | | .kys | `source.monitor / cmd.monitor.toggletransparencygrid` |
| Show Dropped Frame Indicator [^22] | `programMonitor.droppedFrameIndicator` | Program Monitor | | | .kys | `program.monitor / cmd.monitor.toggledroppedframeindicator` |
| Overlays [^22] | `programMonitor.overlays` | Application | | | .kys | `global / cmd.monitor.overlays` |
| Show Audio Time Units [^22] | `programMonitor.audioTimeUnits` | Program Monitor | | | Wabcut | (not mapped) |
| Nudge Selected Object Up by Five | `programMonitor.nudgeUpFive` | Program Monitor | `Ctrl+Shift+Up` | `Shift+Cmd+Up` | 2026 | `program.monitor / cmd.monitor.nudge.up.five` |
| Nudge Selected Object Right by Five | `programMonitor.nudgeRightFive` | Program Monitor | `Ctrl+Shift+Right` | `Shift+Cmd+Right` | 2026 | `program.monitor / cmd.monitor.nudge.right.five` |
| Nudge Selected Object Left by Five | `programMonitor.nudgeLeftFive` | Program Monitor | `Ctrl+Shift+Left` | `Shift+Cmd+Left` | 2026 | `program.monitor / cmd.monitor.nudge.left.five` |
| Nudge Selected Object Down by Five | `programMonitor.nudgeDownFive` | Program Monitor | `Ctrl+Shift+Down` | `Shift+Cmd+Down` | 2026 | `program.monitor / cmd.monitor.nudge.down.five` |
| Nudge Selected Object Up by One | `programMonitor.nudgeUp` | Program Monitor | `Ctrl+Up` | `Cmd+Up` | 2026 | `program.monitor / cmd.monitor.nudge.up.one` |
| Nudge Selected Object Right by One | `programMonitor.nudgeRight` | Program Monitor | `Ctrl+Right` | `Cmd+Right` | 2026 | `program.monitor / cmd.monitor.nudge.right.one` |
| Nudge Selected Object Left by One | `programMonitor.nudgeLeft` | Program Monitor | `Ctrl+Left` | `Cmd+Left` | 2026 | `program.monitor / cmd.monitor.nudge.left.one` |
| Nudge Selected Object Down by One | `programMonitor.nudgeDown` | Program Monitor | `Ctrl+Down` | `Cmd+Down` | 2026 | `program.monitor / cmd.monitor.nudge.down.one` |
| New Bin | `project.newBin` | Project | `Ctrl+B` | `Cmd+B` | 2026 | `project / cmd.file.new.bin` |
| Delete | `project.delete` | Project | `Backspace` | `Backspace` | 2026 | `project / cmd.edit.clear` |
| List View | `project.listView` | Project | `Ctrl+PageUp` | `Cmd+PageUp` | 2026 | `project / cmd.project.wingtip.view.list` |
| Icon View | `project.iconView` | Project | `Ctrl+PageDown` | `Cmd+PageDown` | 2026 | `project / cmd.project.wingtip.view.icon` |
| Hover Scrub | `project.hoverScrub` | Project | `Shift+H` | `Shift+H` | 2026 | `project / cmd.project.enablehover` |
| Delete Selection with Options | `project.deleteWithOptions` | Project | `Ctrl+Delete` | `Cmd+Delete` | 2026 | `project / cmd.project.deletewithoptions` |
| Extend Selection Down | `project.extendSelectionDown` | Project | `Shift+Down` | `Shift+Down` | 2026 | `project / cmd.project.moveextend.down` |
| Extend Selection Left | `project.extendSelectionLeft` | Project | `Shift+Left` | `Shift+Left` | 2026 | `project / cmd.project.moveextend.left` |
| Extend Selection Right | `project.extendSelectionRight` | Project | `Shift+Right` | `Shift+Right` | 2026 | `project / cmd.project.moveextend.right` |
| Extend Selection Up | `project.extendSelectionUp` | Project | `Shift+Up` | `Shift+Up` | 2026 | `project / cmd.project.moveextend.up` |
| Move Selection Down | `project.moveSelectionDown` | Project | `Down` | `Down` | 2026 | `project / cmd.project.move.down` |
| Move Selection End | `project.moveSelectionEnd` | Project | `End` | `End` | 2026 | `project / cmd.project.move.end` |
| Move Selection Home | `project.moveSelectionHome` | Project | `Home` | `Home` | 2026 | `project / cmd.project.move.home` |
| Move Selection Left [^12] | `project.moveSelectionLeft` | Project | `Left` | `Left` | 2026 | `project / cmd.project.movel.eft` |
| Move Selection Page Down | `project.moveSelectionPageDown` | Project | `PageDown` | `PageDown` | 2026 | `project / cmd.project.move.pagedown` |
| Move Selection Page Up | `project.moveSelectionPageUp` | Project | `PageUp` | `PageUp` | 2026 | `project / cmd.project.move.pageup` |
| Move Selection Right | `project.moveSelectionRight` | Project | `Right` | `Right` | 2026 | `project / cmd.project.move.right` |
| Move Selection Up | `project.moveSelectionUp` | Project | `Up` | `Up` | 2026 | `project / cmd.project.move.up` |
| Next Column Field | `project.nextColumnField` | Project | `Tab` | `Tab` | 2026 | `project / cmd.project.nextcolumnfield` |
| Next Row Field | `project.nextRowField` | Project | `Enter` | `Return` | 2026 | `project / cmd.project.nextrowfield` |
| Open in Source Monitor | `project.openInSourceMonitor` | Project | `Shift+O` | `Shift+O` | 2026 | `project / cmd.project.openinsource` |
| Previous Column Field | `project.previousColumnField` | Project | `Shift+Tab` | `Shift+Tab` | 2026 | `project / cmd.project.previouscolumnfield` |
| Previous Row Field | `project.previousRowField` | Project | `Shift+Enter` | `Shift+Return` | 2026 | `project / cmd.project.previousrowfield` |
| Thumbnail Size Next | `project.thumbnailSizeNext` | Project | `Shift+]` | `Shift+]` | 2026 | `project / cmd.project.next.thumbnail.size.large` |
| Thumbnail Size Previous | `project.thumbnailSizePrevious` | Project | `Shift+[` | `Shift+[` | 2026 | `project / cmd.project.previous.thumbnail.size.large` |
| Toggle View | `project.toggleView` | Project | `Shift+\` | `Shift+\` | 2026 | `project / cmd.project.toggle.view` |
| Zoom In | `project.zoomIn` | Project | `=` | `=` | 2026 | `project / cmd.project.zoom.in` |
| Zoom Out | `project.zoomOut` | Project | `-` | `-` | 2026 | `project / cmd.project.zoom.out` |
| Clear Selection | `timeline.clearSelection` | Timeline | `Backspace` | `Backspace` | 2026 | `timeline / cmd.edit.clear` |
| Decrease Audio Tracks Height | `timeline.decreaseAudioTrackHeight` | Timeline | `Alt+-` | `Opt+-` | 2026 | `timeline / cmd.timeline.decrease.audio.tracks.height` |
| Decrease Video Tracks Height | `timeline.decreaseVideoTrackHeight` | Timeline | `Ctrl+-` | `Cmd+-` | 2026 | `timeline / cmd.timeline.decrease.video.tracks.height` |
| Expand All Tracks [^13] | `timeline.expandAllTracks` | Application | `Shift+=` | `Shift+=` | 2017, .kys | `global / cmd.timeline.expand.all.tracks` |
| Increase Audio Tracks Height | `timeline.increaseAudioTrackHeight` | Timeline | `Alt+=` | `Opt+=` | 2026 | `timeline / cmd.timeline.increase.audio.tracks.height` |
| Increase Video Tracks Height | `timeline.increaseVideoTrackHeight` | Timeline | `Ctrl+=` | `Cmd+=` | 2026 | `timeline / cmd.timeline.increase.video.tracks.height` |
| Minimize All Tracks [^13] | `timeline.minimizeAllTracks` | Application | `Shift+-` | `Shift+-` | 2017, .kys | `global / cmd.timeline.minimize.all.tracks` |
| Next Sequence Tab [^14] | `timeline.nextSequenceTab` | Timeline | `Ctrl+Tab` | `Ctrl+Tab` | Wabcut | (not mapped) |
| Nudge Clip Selection Left Five Frames | `timeline.nudgeClipLeftFive` | Timeline | `Alt+Shift+Left` | `Shift+Cmd+Left` | 2026 | `timeline / cmd.timeline.nudge.left.several` |
| Nudge Clip Selection Left One Frame | `timeline.nudgeClipLeft` | Timeline | `Alt+Left` | `Cmd+Left` | 2026 | `timeline / cmd.timeline.nudge.left.one` |
| Nudge Clip Selection Right Five Frames | `timeline.nudgeClipRightFive` | Timeline | `Alt+Shift+Right` | `Shift+Cmd+Right` | 2026 | `timeline / cmd.timeline.nudge.right.several` |
| Nudge Clip Selection Right One Frame | `timeline.nudgeClipRight` | Timeline | `Alt+Right` | `Cmd+Right` | 2026 | `timeline / cmd.timeline.nudge.right.one` |
| Previous Sequence Tab [^14] | `timeline.previousSequenceTab` | Timeline | `Ctrl+Shift+Tab` | `Ctrl+Shift+Tab` | Wabcut | (not mapped) |
| Select Next Clip (0.2) [^sel] | `timeline.selectNextClip` | Timeline | `Ctrl+Down` | `Cmd+Down` | Wabcut | (not mapped) |
| Select Previous Clip (0.2) [^sel] | `timeline.selectPreviousClip` | Timeline | `Ctrl+Up` | `Cmd+Up` | Wabcut | (not mapped) |
| Ripple Delete | `timeline.rippleDelete` | Timeline | `Alt+Backspace` | `Opt+Backspace` | 2026 | `timeline / cmd.timeline.ripple.delete` |
| Set Work Area Bar In Point | `timeline.setWorkAreaIn` | Timeline | `Alt+[` | `Opt+[` | 2026 | `timeline / cmd.timeline.workbar.set.in` |
| Set Work Area Bar Out Point | `timeline.setWorkAreaOut` | Timeline | `Alt+]` | `Opt+]` | 2026 | `timeline / cmd.timeline.workbar.set.out` |
| Show Next Screen | `timeline.showNextScreen` | Timeline | `PageDown` | `PageDown` | 2026 | `timeline / cmd.timeline.show.next.screen` |
| Show Previous Screen | `timeline.showPreviousScreen` | Timeline | `PageUp` | `PageUp` | 2026 | `timeline / cmd.timeline.show.previous.screen` |
| Slide Clip Selection Left Five Frames | `timeline.slideClipLeftFive` | Timeline | `Alt+Shift+,` | `Opt+Shift+,` | 2026 | `timeline / cmd.timeline.slide.left.several` |
| Slide Clip Selection Left One Frame | `timeline.slideClipLeft` | Timeline | `Alt+,` | `Opt+,` | 2026 | `timeline / cmd.timeline.slide.left.one` |
| Slide Clip Selection Right Five Frames | `timeline.slideClipRightFive` | Timeline | `Alt+Shift+.` | `Opt+Shift+.` | 2026 | `timeline / cmd.timeline.slide.right.several` |
| Slide Clip Selection Right One Frame | `timeline.slideClipRight` | Timeline | `Alt+.` | `Opt+.` | 2026 | `timeline / cmd.timeline.slide.right.one` |
| Slip Clip Selection Left Five Frames | `timeline.slipClipLeftFive` | Timeline | `Ctrl+Alt+Shift+Left` | `Opt+Shift+Cmd+Left` | 2026 | `timeline / cmd.timeline.slip.left.several` |
| Slip Clip Selection Left One Frame | `timeline.slipClipLeft` | Timeline | `Ctrl+Alt+Left` | `Opt+Cmd+Left` | 2026 | `timeline / cmd.timeline.slip.left.one` |
| Slip Clip Selection Right Five Frames | `timeline.slipClipRightFive` | Timeline | `Ctrl+Alt+Shift+Right` | `Opt+Shift+Cmd+Right` | 2026 | `timeline / cmd.timeline.slip.right.several` |
| Slip Clip Selection Right One Frame | `timeline.slipClipRight` | Timeline | `Ctrl+Alt+Right` | `Opt+Cmd+Right` | 2026 | `timeline / cmd.timeline.slip.right.one` |

[^1]: Paste Insert: Not on the 2026 page; in the Edit table of the 2024 page.
[^2]: Trim Edit: Adobe's 2026 page says Cmd+T on macOS, which collides with Graphics > New Layer > Text. Premiere's shipped macOS key set binds Trim Edit (cmd.placeholder) to Shift+T, and Wabcut follows the shipped key set.
[^3]: Zoom to Sequence: Listed on Adobe's 2017 print pages for both OSes but not on the 2024 or 2026 pages; also the binding of cmd.tlnav.zoomto.sequence in the default .kys files.
[^4]: Make Subsequence: Adobe's 2026 page says Cmd+U on macOS, which collides with Clip > Make Subclip. Premiere's shipped macOS key set binds Shift+U, and Wabcut follows the shipped key set.
[^5]: Go to Next Caption Segment: Premiere stores this in context.timeline (cmd.timeline.goto.next.caption.trackitem); the page lists it under the Sequence menu, and the table follows the page.
[^6]: Go to Previous Caption Segment: See Go to Next Caption Segment.
[^7]: Maximize Frame: The 2026 page shows "Shift +" with the key missing (a backtick lost in rendering); the 2024 page lists Maximize or Restore Active Frame = Shift + `.
[^8]: Track Select Forward Tool: The 2017 print pages call it "Track Select Tool".
[^9]: Track Select Backward Tool: Stated in Adobe's 2026 "Assign multiple keyboard shortcuts to a command" page; not in a shortcut table.
[^10]: Type Tool: Not in any Adobe table (the only "Type Tool | T" row is the Legacy Titler); cmd.tools.12text = T in the Windows and macOS default .kys files.
[^11]: Meter Input(s) Only: On macOS this is the physical Control key (Qt MetaModifier), not Command.
[^12]: Move Selection Left: Premiere's internal name really is cmd.project.movel.eft.
[^13]: Expand All Tracks / Minimize All Tracks (M5i): from Adobe's older tables (the 2017 print pages); the 2024 and 2026 pages no longer list them. Verified in M5j against both shipped key sets: `cmd.timeline.expand.all.tracks` and `cmd.timeline.minimize.all.tracks` in `context.global` (so Application scope), Shift+= and Shift+- on both OSes.
[^sel]: Select Next / Previous Clip (0.2): the keys published lists of Premiere shortcuts give (Ctrl/Cmd+Down and Up); not checked against the shipped .kys files, so marked `wabcut` in the JSON and not mapped on .kys import.
[^14]: Next / Previous Sequence Tab (M5i): Wabcut's, not Premiere's (source `wabcut` in the JSON). Premiere has no default for cycling sequence tabs. On macOS this is the physical Control key (Qt MetaModifier), because Cmd+Tab is the system app switcher. Qt reports Ctrl+Shift+Tab as Ctrl+Shift+Backtab; `KeyChord::normalized()` folds it.
[^15]: Toggle Trim Type: verified in M5j against both shipped key sets: `cmd.sequence.toggletrimtype` in `context.global`, Ctrl+Shift+T on Windows and Control+T on macOS (the physical Control key: Qt MetaModifier, written `Ctrl` in the macOS table). M5i had the 2024 page's Shift+T in the Timeline, which shadowed Trim Edit there; the id moved from `timeline.toggleTrimType` to `sequence.toggleTrimType` with the scope. With no edit point selected it still starts a Trim Edit (see "Trim mode" below).
[^16]: Trim Backward / Forward (Many): verified in M5j against both shipped key sets (`cmd.sequence.trimbackward`, `trimforward`, `trimbackwardmany`, `trimforwardmany` in `context.global`); the ids moved from `timeline.*` to `sequence.*`. Many uses the Large trim offset preference (default 5 frames). Ctrl+Alt+arrows stay Slip, as in Premiere. The Program Monitor's own Ctrl+arrow nudges win while it has focus (Windows).
[^17]: Multi-camera keys (M5l): the 2026 page lists none. Both shipped key sets put them in `context.global`, so they are Application commands (they act in any panel while the Program Monitor shows the multi-camera view; elsewhere they do nothing). Select Camera 1-9 is the bare digit on both OSes (`cmd.multicam.choosenocut.cameraN`). Cut to Camera N (`cmd.multicam.choose.camera.N`) is Ctrl+1-8 on Windows, whose key set has no Ctrl+9, and Control+1-9 on macOS: the physical Control key, written `Ctrl` in the macOS table. Toggle Multi-Camera View is Shift+0. Premiere's 0 (Toggle Multi-Camera Record) has no counterpart: Wabcut always records switches made while playing.
[^18]: Voice-over recording (A.8, [ai.md](ai.md) "Voice-over recording"): no default key, as in Premiere. Checked against both shipped key sets: the macOS set lists `cmd.timeline.voiceover.track.record` (Voice-over Record, the track header's microphone) in `context.timeline` with no key, and the Windows set does not list it. A custom `.kys` that binds it maps to `record.toggle`. `record.start` / `record.stop` are Wabcut's (start only / stop only). While a pass runs, Space (Play/Stop) stops it and keeps the take, as in Premiere.
[^19]: Remove Attributes (C.11, M5m): Premiere's Edit > Remove Attributes… has no default key, and neither shipped key set (Windows `Premiere_default_keys.kys`, macOS `Default Adobe.kys`) lists an item for it, so it has no `.kys` mapping. It opens the attributes dialog on the selection's own attributes (`clip.removeAttributes`).
[^20]: Moments (M5o, gaming): Wabcut's, with no default key; Premiere has no counterpart. They act on the clip under the playhead (the selected one if several): Slow-mo at Playhead is the 25 % / 1 s slow motion centred on the playhead (one `timeRemap.*` undo step, later clips ripple); Punch-in at Playhead zooms to 130 % of the clip's scale for 1 s (Motion keyframes). The Timeline's Moments popover (the zap button in the tracks header) and the clip menu's Moments submenu list the other presets; a key assigned in Keyboard Shortcuts shows next to both.
[^21]: P.2 (E5, Premiere editing parity): Premiere binds no key to these; Wabcut has none either. Every `.kys` pair is the command's item in Premiere's shipped macOS key set (`Default Adobe.kys`), which lists every command (the Windows set lists only bound ones), so a custom key set that binds one carries over. New Sequence From Clip and Make Offline have no verified item there. The Editing bridge runs them (`EditingBridge::handlesCommand`); Edit Original, Duplicate, Make Subsequence and Next / Previous in Sequence, listed above with their keys, now run too. Keyboard Shortcuts > Export… also saves a whole key set as a `.kys` (`exportKys`), which Premiere and Wabcut import.
[^ux]: Settings… (UX rework, 2026-10-23): Premiere's Edit > Preferences > General. The shipped macOS key set (`Default Adobe.kys`) binds it to Cmd+, in `context.global`; the Windows set binds nothing, so Ctrl+, is Wabcut's (source `wabcut` in the Windows JSON, `premiere-kys` in the macOS one). `Main.qml` opens the Settings window; the command palette lists it. `tests/keymap/test_default_tables.cpp` checks both keys and the `.kys` pair.
[^uxt]: Reveal in Project (UX rework, 2026-10-23): Premiere's timeline clip menu item, with no default key and no verified item in either shipped key set, so `.kys` import skips it. The Editing bridge runs it (`EditingBridge::revealInProject`): the primary selected clip's media item, or its nested sequence, is shown and selected in the Library, which comes forward.
[^uxs]: UX rework (stream S, 2026-10-23): these keys act now. Close (Ctrl+W) closes the panel the active panel group shows (the group clicked last: a click in it, one of its tabs, a drop there, a focus key or the Window menu; else the group with the keyboard focus). Maximize Frame (Shift+`) gives the active group the whole work area, again restores it; ` alone (a window shortcut, not a keymap command: the key is free in both key sets) does it for the group under the pointer, and a double-click on a panel's tab too. Reset to Saved Layout (Alt+Shift+0) lays the workspace out as its preset does, with Undo on the message. Effect Controls (Shift+5) shows and focuses the panel, like the other focus keys. Step Backward / Step Forward (Left / Right) move one state through the History panel while it has the focus (`Editing.runCommand`).
[^22]: The monitors (P.2, [monitors.md](monitors.md)): checked against both shipped key sets. Export Frame (`cmd.export.frame`, `context.global`) is Ctrl+Shift+E on Windows and Shift+E on macOS; Toggle Full Screen (`cmd.toggle.fullscreen.monitor`, `context.global`) is Ctrl+` on both (the physical Control key on macOS). Loop (`cmd.monitor.loop`), Enable Transmit (`cmd.toggletransmit`) and Toggle Crop Direct Manipulation (`cmd.monitor.toggle.crop.dm`) are in `context.global` without a key; Add Guide, Clear Guides (`cmd.monitor.removeguides`), Save Guides as Template, Manage Guides and Transparency Grid (`cmd.monitor.toggletransparencygrid`, in both monitors' contexts) have no key either, nor do Show Dropped Frame Indicator (`cmd.monitor.toggledroppedframeindicator`) and Overlays (`cmd.monitor.overlays`, `context.global`); Show Audio Time Units has no item in either key set, so no `.kys` mapping. Loop acts on the focused monitor (the Source Monitor's while it has the focus, else the Program Monitor's), Export Frame too. The Program Monitor's Show Rulers, Show Guides, Lock Guides, Snap and Nudge rows above now act (Motion direct manipulation, guides).

## How resolution works

`KeymapManager::resolve(Scope focused, KeyChord)` returns the command for a key press:

1. **Scope precedence.** The focused panel's bindings are tried first, then Application. A panel shortcut
   therefore shadows an Application shortcut on the same chord while that panel has focus (Ctrl+L is Loop in the
   Audio Track Mixer and Link elsewhere). This is Premiere's documented rule: "When a Panel Shortcut has the same
   assigned shortcut as an application Shortcut, the application shortcut does not function when that panel has
   focus." Shadowing is by design and is not reported as a conflict.
2. **One scope per command.** Premiere's commands are per panel, so Wabcut's are too: Edit > Ripple Delete
   (`edit.rippleDelete`, Application) and the Timeline panel's Ripple Delete (`timeline.rippleDelete`) are separate
   commands. `CommandInfo::scope` is the only scope a command can be bound in, and load() enforces it.
3. **Qt event quirks.** The chord from a `QKeyEvent` is matched to the bound chord. Key_Backtab becomes Shift+Tab.
   GroupSwitch is ignored. Keypad navigation and editing keys (Enter, Delete, Insert, arrows, Home/End,
   PgUp/PgDn) fall back to their main-keyboard bindings when they have no keypad-specific one; this also covers
   macOS, where Qt flags the arrow keys as keypad keys. Keypad Enter falls back to Return. Keypad digits and
   operators never fall back: Premiere reserves them for timecode entry, and numpad + and - type relative timecode.
   Shift plus a US-layout symbol (Shift+; arrives as `Key_Colon`) is matched to the base key (Shift+;). The exact
   chord is tried in the focused panel and then in Application before any fallback spelling is tried in either.
4. **Canonical storage.** setChords(), assign() and unassign() store chords in the same form resolve() matches,
   so a chord captured from a key event in a shortcut editor (Shift+`Key_Colon`) is stored as Shift+; and takes
   that key from whichever command held it.
5. **Conflicts.** `conflicts()` lists every chord bound to two or more commands in the same scope, with the
   commands in resolution order. A chord the user added (one that is not among that command's defaults) wins.
   After that comes the winner order of an acknowledged conflict, then default-table order. A default table must
   acknowledge each of its own conflicts in `acknowledgedConflicts`, and load() rejects an acknowledgement that
   does not match a real conflict. A conflict counts as acknowledged only while its commands and winner are
   exactly the table's. Both shipped tables are conflict-free, which a test asserts.

## User overrides

Overrides are a diff over the defaults. `setChords(command, chords)` replaces a command's chords; an empty list
unbinds it, and setting the defaults back removes the override. `assign(command, chord)` works like Premiere's
shortcut editor: it takes the chord away from any other command in the same scope and returns those commands so
the UI can say so. `saveOverrides()` writes, and `loadOverrides()` reads:

```json
{
  "schemaVersion": 1,
  "platform": "windows",
  "overrides": {
    "marker.markIn": [],
    "tool.razor": ["X", "Ctrl++"]
  }
}
```

Override files are per OS family. Windows and Linux share one, as Premiere key sets do; macOS has its own.
Unknown command ids, for example from a newer Wabcut, are skipped with a warning. Malformed files are rejected and
the current overrides stay untouched.

## Default table format

```json
{
  "schemaVersion": 1,
  "name": "Adobe Premiere Pro default (macOS)",
  "platform": "macos",
  "sources": { "adobe-2026": { "title": "…", "url": "…", "lastUpdated": "2026-01-07" } },
  "bindings": [
    { "scope": "application", "command": "marker.clearIn", "keys": ["Opt+I"], "source": "adobe-2026" }
  ],
  "acknowledgedConflicts": []
}
```

An entry in `acknowledgedConflicts` names a deliberate same-scope conflict and its winner first, for example
`{"scope": "application", "keys": "Cmd+T", "commands": ["graphics.newTextLayer", "sequence.trimEdit"], "note": "…"}`.
Both shipped tables leave the list empty.

`load()` is strict and atomic: it rejects unknown commands, a scope that is not the command's own, duplicate
commands, chords spelled for the other OS, and stale acknowledgements. `cmake/EmbedKeymaps.cmake` turns both files
into byte arrays in the build directory, and a test checks the embedded bytes equal the files.

## J/K/L transport gesture and key dispatch

Holding keys needs both press and release events, so the app does not call `resolve()` itself. The Qt-free
`KeyDispatcher` takes every press and release and returns a `KeyResult {handled, commands, transport}`. `handled`
tells the caller to consume the event even when nothing runs, for example the auto-repeat of Play/Stop. The
dispatcher applies `CommandInfo::autoRepeat`: frame steps, nudges, zoom and undo repeat while held, while Play/Stop
and other toggles do not. It routes the three shuttle commands through `TransportGesture`, which emits the typed
intents of contract revision 2 for `PlaybackController::setTransportSpeed(Rational)` and `stepFrames(int)`:

| Keys | Intent |
|---|---|
| L / J | `ShuttleStep`, speed = next rung of the ladder 1x, 2x, 4x, 8x in that direction (8x stays 8x; the other direction restarts at 1x) |
| K | `Stop` |
| hold K, tap J / L | `StepFrames(-1)` / `StepFrames(+1)` |
| hold K, hold J / L | the step, then `SetSpeed(-1/4)` / `SetSpeed(+1/4)` once the hold is recognised |
| release J/L or K during slow play | `Stop` (once, whichever is released first) |

- OS auto-repeat never climbs the ladder.
- A hold is recognised by the first auto-repeat press, or by `tick()` after 300 ms for systems with key repeat
  turned off.
- The gesture is K-first: only a J/L pressed while K is held takes part, and pressing K always stops, including
  during slow play.
- With K held, the most recently pressed J/L can become a hold; releasing it hands over to the other one if that
  is still down.
- Roles follow commands, not keys: rebinding the shuttle commands rebinds the gesture, and several keys may share
  a role (K and keypad 5 both as stop; slow play ends when the last one is released).
- `syncSpeed()` tells the gesture what the transport really runs at, for example after Play/Stop, so the next
  L climbs from there.
- `focusLost()` forgets held keys and ends slow play, but leaves normal shuttle play running.

Presses and ticks are timed with one clock (`std::chrono::steady_clock`, or one injected for tests), never with
window-system event timestamps. A fresh press of a key the dispatcher thinks is still down is treated as a lost
release followed by a new press, so a K release missed in another window does not latch the gesture.

## App integration (KeymapDispatcher)

The Qt side lives in `src/app/src/bridge/KeymapDispatcher.{h,cpp}`, inside the `Wabcut` QML module. The keymap
module itself stays Qt-free. The bridge static_asserts that every key and modifier constant equals Qt's, so
`QKeyEvent` values convert without a table.

**What it does.**
- `KeymapDispatcher` is a QML singleton. It owns the `KeymapManager`: the built-in Premiere table for the running
  OS plus the user's overrides from `<AppConfigLocation>/keymap/overrides-<os>.json`, a path given by
  `QStandardPaths`. It also owns the `KeyDispatcher` with its `TransportGesture`.
- It installs an event filter on one `QQuickWindow` (`KeymapDispatcher.window`). If nothing sets the window, it
  attaches to the first top-level QQuickWindow that becomes the focus window.
- Presses and releases are both forwarded. Window deactivation, focus-out and hiding reset held gestures, and a
  50 ms timer ticks the gesture while K is held.

**Declaring scopes (panel owners).** The panel's root item sets the attached property, and every descendant
inherits it:

```qml
import Wabcut

Item {                                // TimelinePanel.qml
    KeymapScope.scope: "timeline"     // timeline, programMonitor, sourceMonitor, project, effectControls,
                                      // effects, audioMixer, history, mediaBrowser, metadata, application
}
```

The dispatcher resolves against the focused item's nearest `KeymapScope`, then Application, so Alt+Backspace is
Ripple Delete only while focus is inside the timeline, and Shift+Delete works everywhere. An unknown scope id logs
a warning and is skipped. `KeymapDispatcher.scopeAt(item)` shows what applies to an item.

**Routing rules.** These keep keys away from the editor's shortcuts:

| Situation | Behaviour |
|---|---|
| Focus in a text editor (TextField, TextArea, NumberField while editing) | Plain, Shift and Alt/Option keys stay in the editor: typing, arrows (NumberField's Up/Down), Enter, Alt+Backspace. Ctrl/Cmd shortcuts the field does not claim (Save, Export…) still work. |
| The focused item, or an ancestor, accepts `QEvent::ShortcutOverride` | The item keeps the key. TextInput claims its editing sequences (Ctrl+Z, Ctrl+C, …); a custom control uses `Keys.onShortcutOverride: event => event.accepted = …`. Lists and grids that navigate with arrow keys are not text editors and must claim the arrows this way, or Up/Down reach edit-point navigation. The dispatcher sends the probe the way Qt Quick does: focus item first, then parents. |
| `KeymapScope.suppressShortcuts: true` on the focus item or an ancestor | No editor shortcut fires (for controls that handle every key themselves). |
| IME composition (`preeditText` not empty) | Nothing fires. |
| Focus inside a popup, menu or dialog (Qt Quick Controls overlay) | Nothing fires. Popups that should block shortcuts must take focus; Dialog and Menu do. UX rework: a popup whose content declares a scope of its own (the History panel's `KeymapScope.scope: "history"`) gets the keys bound in that scope (Left / Right step through the history, rebindings included); every other key still stays with the popup. |
| AltGr (Windows: Ctrl+Alt plus a printed character such as € or @) | Treated as typing, never as a Ctrl+Alt shortcut. macOS has no AltGr, so Option shortcuts that also type a character still fire there. |
| Keypad Enter / keypad Delete | Act as Enter (Render In to Out) and Delete (Clear). |

**Outputs (feature owners).**
- `commandTriggered(string commandId)` fires for every command except the J/K/L gesture.
- `transportIntent(kind, speedNum, speedDen, frames)` fires for the gesture. `kind` is
  `KeymapDispatcher.ShuttleStep | SetSpeed | StepFrames | Stop`. Speed is set for ShuttleStep and SetSpeed, frames
  for StepFrames.
- `trigger(commandId)` runs a command from a menu or button. Shuttle commands go through the gesture, so a
  transport-bar button climbs the same ladder.
- **The menu bar (UX rework, `panels/menubar/`).** Its rows carry keymap ids (`BarMenuItem.command`), so each shows the
  command's current key and greys out while it cannot run where it will act (`AppMenuBar.commandState(id)`: with the
  Source Monitor focused, its clip and marks for the commands it runs itself - Mark In / Out / Clip, Add Marker, Go to
  and Clear In / Out, Reverse Match Frame; otherwise `CommandPalette.commandState(id)`). Choosing a row closes the
  menus, gives the focus back to the item that had it before the first menu opened, and then calls `trigger(id)`:
  focus-routed commands (Add Marker on the timeline's clip, the Source Monitor's marks) act where you were. `trigger`
  skips the text-editor rule of real key presses, so Edit's Undo, Redo, Cut, Copy, Paste, Clear and Select All are
  not triggered when a text field had the focus: the bar runs the field's own undo / cut / copy / paste / select-all
  instead, and their state is the field's (`AppMenuBar.isTextCommand`). The menus' own keys are not keymap commands:
  Alt+F/E/C/S/M/G/V/W/H are window `Shortcut`s on Windows and Linux (Premiere's Windows keys bind none of them). A
  `Shortcut` matches before the dispatcher sees the key, so each one is disabled while the keymap binds its chord
  (`Shortcuts.commandsForKey`): a key the user assigns wins. macOS has none (`KeymapDispatcher.platform` "macos"):
  Option+letter types characters there, and the macOS key set binds Opt+M (Clear Selected Marker). Left / Right
  inside an open menu move to the neighbouring menu. Settings… is the keymap's `edit.preferences` (Ctrl+, / Cmd+,);
  there is no window Shortcut for it any more, so rebinding it works.
- `syncTransportSpeed(num, den)` reports the real transport speed, for example after Play/Stop or a seek.
- `shortcutText(commandId)` and `shortcutTexts(commandId)` give display text for tooltips and menus (`Ctrl+K` on
  Windows/Linux, `⌘K` on macOS). Read `KeymapDispatcher.revision` in the same binding so it refreshes after edits.
- `assignShortcut(commandId, chordText)`, `resetShortcut(id)`, `resetAllShortcuts()` and
  `importPremiereKeySet(fileUrl)` edit and persist the overrides; `assignShortcut` returns `{ok, lost, error}`.

```qml
Connections {
    target: KeymapDispatcher
    function onCommandTriggered(commandId) { /* route by id, see below */ }
    function onTransportIntent(kind, speedNum, speedDen, frames) {
        switch (kind) {
        case KeymapDispatcher.ShuttleStep:
        case KeymapDispatcher.SetSpeed:   playback.setTransportSpeed(speedNum, speedDen); break
        case KeymapDispatcher.StepFrames: playback.stepFrames(frames); break
        case KeymapDispatcher.Stop:       playback.pause(); break
        }
    }
}
```

From C++ (for example `ProjectController` or the playback bridge), get the instance with
`engine.singletonInstance<wabcut::app::KeymapDispatcher*>("Wabcut", "KeymapDispatcher")` and connect to the same
signals. Use `keymap().registry().find(id)->displayName` as the undo label.

**Wiring for the UI shell (M1).** Two steps:
- Main.qml should set `KeymapDispatcher.window = root` in `Component.onCompleted`. This creates the singleton
  early and attaches it explicitly.
- Each panel root should set its `KeymapScope.scope`.

The tooltip calls `MockKeymap.chord(id)` become `KeymapDispatcher.shortcutText(id)`. Several mock ids differ from
the real ones:

| MockKeymap id | Command id |
|---|---|
| `file.export.media` | `file.exportMedia` |
| `transport.shuttleLeft` / `transport.shuttleRight` | `transport.shuttleBackward` / `transport.shuttleForward` |
| `timeline.snap` | `sequence.snap` |
| `timeline.zoomIn` / `timeline.zoomOut` | `sequence.zoomIn` / `sequence.zoomOut` |

The other mock ids (`file.save`, `file.import`, `edit.find`, `transport.playStop`, `transport.shuttleStop`,
`transport.goToStart`, `transport.goToEnd`, `tool.selection`, `tool.razor`) already match.

**Wiring the edit commands (M5, timeline).** Handle `commandTriggered` in `ProjectController` with one table from
command id to an `EditSession::apply(label, fn)` call over `EditOps`. These are the ids the timeline owns, with
their Premiere meaning:

| Command ids | Edit |
|---|---|
| `clip.insert`, `clip.overwrite` | insert / overwrite the source In–Out at the playhead on the targeted tracks |
| `sequence.lift`, `sequence.extract` | lift / extract the sequence In–Out |
| `sequence.addEdit`, `sequence.addEditToAllTracks` | razor at the playhead on targeted / all tracks |
| `edit.rippleDelete` (Application), `timeline.rippleDelete` (Timeline panel) | ripple delete the selection (or the gap under the cursor) |
| `edit.clear`, `timeline.clearSelection` | delete (lift) the selection |
| `sequence.rippleTrimPreviousToPlayhead`, `sequence.rippleTrimNextToPlayhead` | Q / W |
| `sequence.extendEditToPlayhead` | E: extend the selected edit point to the playhead |
| `sequence.selectClipAtPlayhead` | D |
| `sequence.selectionFollowsPlayhead` | toggle Selection Follows Playhead (0.2) |
| `sequence.nextInSequence`, `sequence.previousInSequence` | Go to Gap: the playhead to the next / previous gap in the sequence (0.2) |
| `timeline.selectNextClip`, `timeline.selectPreviousClip` | select the next / previous clip on the targeted tracks, after / before the selected one (0.2) |
| `timeline.showVideoFadeHandles`, `timeline.showAudioFadeHandles` | show / hide the fade handles of video / audio clips (0.2) |
| `sequence.matchFrame`, `sequence.reverseMatchFrame` | F / Shift+R |
| `sequence.snap` | toggle snapping |
| `clip.link`, `clip.group`, `clip.ungroup`, `clip.enable` | link/unlink, group / ungroup (0.2: the `clip.group` / `clip.ungroup` commands), enable |
| `clip.speedDuration` | open Speed/Duration |
| `sequence.applyVideoTransition`, `sequence.applyAudioTransition`, `sequence.applyDefaultTransitions` | default transitions |
| `timeline.nudgeClip*`, `timeline.slideClip*`, `timeline.slipClip*` | nudge / slide / slip by 1 or 5 frames |
| `tool.*` | switch the active timeline tool |
| `marker.markIn/markOut/markClip/markSelection`, `marker.clearIn/clearOut/clearInAndOut`, `marker.goToIn/goToOut` | sequence In/Out marks |
| `edit.undo`, `edit.redo`, `edit.cut`, `edit.copy`, `edit.paste`, `edit.pasteInsert`, `edit.selectAll`, `edit.deselectAll` | session-wide edit commands |
| `transport.stepBackward/Forward(Five)`, `transport.goToPreviousEdit/NextEdit(AnyTrack)`, `transport.goToStart/End`, `transport.playStop`, `transport.playInToOut`, `transport.playAround` | playhead and playback |

Tests: `tests/app/test_keymap_dispatcher.cpp` drives a hidden window built from `tests/app/KeymapScene.qml` with
synthetic key events and covers each rule above.

### Multi-camera (M5l)

The camera keys run while the Program Monitor shows the multi-camera view (`Multicam.viewOn`), whichever panel has
focus. Main.qml routes them to the `Multicam` bridge (MulticamEditBridge):

| Key (Windows / macOS) | Command id | While playing | While paused |
|---|---|---|---|
| 1-9 | `multicam.selectCamera1..9` | records a switch to that angle at the playhead | switches the cut under the playhead to that angle (no new cut) |
| Ctrl+1-8 / Control+1-9 | `multicam.cutToCamera1..9` | records a switch, as above | cuts at the playhead and plays that angle from there |
| Shift+0 | `multicam.toggleView` | turns the multi-camera view on or off | same |

The switches recorded while playing are sent as one `multicam.switchAngle {clip, target, switches}` when playback
stops, so they are one undo step. "Target" is the view's Picture and sound / Picture / Sound pill. A click on an angle
in the grid behaves like its digit, and a Ctrl/Cmd+click like Cut to Camera. Angle 10 and up have no key (Premiere's
default sets bind only 1-9); click them.

### The Project scope: the Library (M5k)

The Library panel declares `KeymapScope.scope: "project"`, so Premiere's Project panel keys act on it while it has
focus. `Main.qml` sends every `project.*` command to the `Library` bridge (`LibraryBridge::runCommand`):

| Command ids | What the Library does |
|---|---|
| `project.newBin` (Ctrl/Cmd+B) | a new bin in the bin shown, named Bin 01, Bin 02, ..., then an inline rename |
| `project.delete` (Backspace) | deletes the selected items (bins with their contents), one undo step |
| `project.listView`, `project.iconView`, `project.toggleView` | list view, icon (grid) view, toggle |
| `project.hoverScrub` (Shift+H) | hover scrub on grid cards on or off |
| `project.openInSourceMonitor` (Shift+O) | the selected media in the Source Monitor |
| `project.thumbnailSizeNext` / `Previous` (Shift+] / Shift+[), `project.zoomIn` / `zoomOut` (= / -) | the cards one size step bigger or smaller (`Library.thumbnailScale`, 0.5 .. 2 in quarters; UX rework). While the Library has focus these keys never fall through to the sequence's zoom |
| `project.moveSelection*` (arrows, Home, End, PageUp, PageDown), `project.extendSelection*` (Shift+arrows) | `LibraryBridge::selectionMoveRequested(where, extend)`: the panel moves the selection in the view it shows (UX rework). The grid and the list still take the plain and Shift arrows themselves while they have focus. Shift+arrows grow and shrink the range from the item the keys went to last (the anchor stays), like Premiere. While the Text tab or the Smart search results hide the items, these keys and the card size keys leave the hidden selection alone and go to the focused control (a Text tab slider) |

Two Application commands are the Library's too: `edit.find` (Ctrl/Cmd+F) shows the Library and focuses its Find bar,
and `clip.makeSubclip` (Ctrl/Cmd+U) asks for the subclip's name and makes it from the Source Monitor's marks. In the
grid and the list, the arrows move the selection (Shift extends), Return opens and F2 renames; those keys belong to
the view while it has focus. `tests/app/test_library.cpp` covers the bridge side.

The renamed trim commands of M5j (`timeline.toggleTrimType`, `timeline.trim*` -> `sequence.*`) keep keys saved
under their old ids: `KeymapManager::loadOverrides` maps them (review P2-45).

### The Source Monitor scope (M5h)

The Source Monitor panel declares `KeymapScope.scope: "sourceMonitor"`. Like Premiere's Source Monitor, it has no
panel shortcuts of its own: Adobe's default table lists none, and the shipped key sets bind nothing in that context.
Its keys resolve to the Application commands, and the app applies them to the source while the panel has focus.
The Program Monitor has panel shortcuts (Show Rulers, Ctrl/Cmd+R, and so on), so the same chord can resolve
differently there: in the Source Monitor, Ctrl/Cmd+R is Speed/Duration. `tests/app/test_source_monitor.cpp`
checks both the resolution and the routing.

`Main.qml` asks `KeymapDispatcher.scopeAt(window.activeFocusItem)`. When the focus is in `sourceMonitor`, the
command goes to `SourceMonitor.runCommand()` first:

| Key (Windows / macOS) | Command id | In the Source Monitor | Elsewhere |
|---|---|---|---|
| I / O | `marker.markIn` / `marker.markOut` | source In / Out (stored on the media item, `source.setMarks`) | sequence In / Out |
| X | `marker.markClip` | the whole source | the clip under the playhead |
| Shift+I / Shift+O | `marker.goToIn` / `marker.goToOut` | go to the source In / Out | the sequence's |
| Ctrl+Shift+I / Opt+I | `marker.clearIn` | clear the source In | the sequence's |
| Ctrl+Shift+O / Opt+O | `marker.clearOut` | clear the source Out | the sequence's |
| Ctrl+Shift+X / Opt+X | `marker.clearInAndOut` | clear both source marks | the sequence's |
| Ctrl+Shift+Space / Opt+K | `transport.playInToOut` | play the source In to Out | (not handled yet) |
| Space, J / K / L, ← / → (Shift: 5) | `transport.playStop`, the J/K/L gesture, `transport.step*` | the source's own transport | the program's |
| Home / End | `transport.goToStart` / `transport.goToEnd` | the source's start / end | the sequence's |
| Shift+R | `sequence.reverseMatchFrame` | the program playhead goes to where the source frame is used | the same |
| F | `sequence.matchFrame` | nothing (Premiere) | load the clip under the playhead in the Source Monitor, its range marked |
| / , ↑ / ↓ | `marker.markSelection`, `transport.goTo*Edit*` | nothing: sequence-only | the sequence's |
| , / . | `clip.insert` / `clip.overwrite` | Insert / Overwrite from the source (three-point rules; Fit Clip for four points) | the same |
| ; / ' | `sequence.lift` / `sequence.extract` | (Application) | lift / extract the sequence In–Out |

### The timeline's M5i commands

| Key (Windows / macOS) | Command id | Handled by |
|---|---|---|
| Shift+T | `sequence.trimEdit` | `Trim.trimEdit()`: the edit points nearest the playhead, and the Trim view |
| Ctrl+Shift+T / Control+T | `sequence.toggleTrimType` | `Trim.toggleType()`; with no edit point selected, Trim Edit |
| Ctrl+Left/Right / Opt+Left/Right | `sequence.trimBackward` / `trimForward` | `Trim`: one frame on every selected edit point (or on a clip edge chosen with [ or ] when there are none) |
| Ctrl+Shift+Left/Right / Opt+Shift+Left/Right | `sequence.trimBackwardMany` / `trimForwardMany` | `Trim`: the Large trim offset (Preferences, default 5) |
| J / K / L with edit points selected | the transport gesture | `Trim.transportIntent()`: dynamic trimming (Main routes it while `Trim.active`) |
| Ctrl+= / Ctrl+- (Cmd on macOS) | `timeline.increase/decreaseVideoTrackHeight` | `TimelineView.changeTrackHeights("video", ±1)` |
| Alt+= / Alt+- (Opt on macOS) | `timeline.increase/decreaseAudioTrackHeight` | `TimelineView.changeTrackHeights("audio", ±1)` |
| Shift+= / Shift+- | `timeline.expandAllTracks` / `minimizeAllTracks` | `TimelineView.changeTrackHeights("all", 1)` / `minimizeAllTracks()` |
| Ctrl+Tab / Ctrl+Shift+Tab (physical Control on macOS) | `timeline.nextSequenceTab` / `previousSequenceTab` | `SequenceTabs.cycle(±1)` |
| M | `marker.add` | `Markers.add()`: with the Timeline focused and a selected clip under the playhead, a clip marker; otherwise a sequence marker. In the Source Monitor, a clip marker on the source (`SourceMonitor.addMarker()`) |
| Shift+M / Ctrl+Shift+M (Shift+Cmd+M) | `marker.goToNext` / `goToPrevious` | `Markers.goToNext()` / `goToPrevious()` |
| Ctrl+Alt+M (Opt+M) | `marker.clearSelected` | `Markers.clearAtPlayhead()`: the marker at the playhead |
| Ctrl+Alt+Shift+M (Opt+Cmd+M) | `marker.clearAll` | `Markers.clearAll()`: the sequence's markers |

The brief named T, Shift+T and Ctrl+T for trimming. In Premiere's current tables, T is the Type tool, Ctrl+T is New
Text Layer and Ctrl+Alt+arrows slip. Wabcut follows those tables, as the coordinator approved; Toggle Trim Type is the
shipped key sets' Ctrl+Shift+T (Control+T on macOS; M5j). In Premiere, Clear
Selected Marker is Ctrl+Alt+M and Clear All Markers is Ctrl+Alt+Shift+M.

## Premiere `.kys` import

`importKys(path)` reads a Premiere key set and returns user overrides, which `applyKysImport(keymap, import)`
applies. The format below was checked against real files, not inferred: Premiere 25.0 and 26.0 Windows key sets
saved on a development machine, and the Windows and macOS default key sets listed under Sources (50 public key
sets were surveyed in total).

```xml
<?xml version="1.0" encoding="UTF-8"?>
<PremiereData Version="3">
  <shortcuts Version="5">                       <!-- 4 in older files -->
    <platform>windows</platform>                <!-- "macintosh" on macOS; first or last child -->
    <context.global Version="1">                <!-- one per context: global, timeline, project, … -->
      <itemcount>924</itemcount>
      <item.806 Version="1">
        <virtualkey>1</virtualkey>              <!-- absent: the command has no shortcut -->
        <modifier.ctrl>true</modifier.ctrl>     <!-- Windows: ctrl, alt, shift -->
        <modifier.alt>false</modifier.alt>      <!-- macOS: command, opt, shift, ctrl (= ⌃) -->
        <modifier.shift>true</modifier.shift>
        <commandname>cmd.transport.playintoout</commandname>
      </item.806>
```

- A key set lists every command of the Premiere version that saved it. A command with several shortcuts has several
  items with the same name in the same context. The order of an item's children varies between versions.
- **Modifiers.** Windows writes `modifier.ctrl/alt/shift`. macOS writes `modifier.command` (Qt Control),
  `modifier.opt` (Alt), `modifier.shift` and `modifier.ctrl`, the physical ⌃ key (Qt Meta). Evidence: Undo is
  `modifier.command` + Z, Clear In is `modifier.opt` + I, and Play from Playhead to Out Point is `modifier.ctrl`
  + Space, matching Adobe's documented macOS defaults Cmd+Z, Opt+I and Ctrl+Space.
- **Virtual keys.** `0x80000000 | character` is a printable key; the character is the unshifted US character,
  for example 2147483723 = `K`. `0xC0000000 | character` is a keypad key. Small numbers are named keys; each code
  below was verified by matching a command's documented default key to its `<virtualkey>`:

  | Code | Key | Evidence |
  |---|---|---|
  | 1 | Space | Play-Stop Toggle |
  | 2 | Backspace (macOS Delete) | Timeline Clear Selection, Ripple Delete |
  | 3 | Tab | Project Next Column Field |
  | 4 | Return/Enter | Render In to Out, Project Next Row Field |
  | 7–18 | F1–F12 | Help = 7, Capture = 11 (F5), Batch Capture = 12 (F6), plus F7/F11/F12 in a documented user key set |
  | 35 | Delete (macOS Forward Delete) | Clear, Ripple Delete (Shift+) |
  | 36 / 37 | Home / End | Go to Sequence-Clip Start/End |
  | 38 / 39 | Page Up / Page Down | Show Previous/Next Screen |
  | 42 / 43 / 44 / 45 | Left / Right / Up / Down | Step Back/Forward, Go to Previous/Next Edit Point |

  The decoder rejects other codes: 0 means "no key"; 5 (probably keypad Enter) and 19–30 (probably F13–F24) have
  only single, indirect sources; characters outside US-ASCII come from key sets saved with other keyboard layouts.
  A rejected code produces a warning, and that command keeps its current bindings.
- **Mapping.** Items map to Wabcut commands by `(context, command name)` (`CommandInfo::kysContext/kysName`),
  because one name can mean different commands in different panels: `cmd.edit.clear` is Clear in
  `context.global`, Clear Selection in `context.timeline` and Delete in `context.project`. 245 of the 250
  commands have a mapping (Next and Previous Sequence Tab, Start and Stop Voice-over Recording and Remove Attributes
  have no item in Premiere's key sets). Each mapping was verified by the default key set binding that item to the
  command's documented default shortcut. Trim Edit maps to `cmd.placeholder`, an odd name but
  the evidence is consistent: in both default key sets and in a Premiere 26 Windows key set it is the only item on
  Shift+T, Adobe's documented Trim Edit key.
- **Semantics.** A command mentioned in the key set gets exactly the key set's shortcuts; no shortcuts means
  unbound. Commands the key set does not mention keep their bindings. Overrides equal to the defaults are dropped.
  Premiere commands with shortcuts but no Wabcut equivalent are listed in `KysImport::unmappedCommands`, and a
  macOS key set cannot be applied to a Windows/Linux keymap or the reverse.

**Export (P.2).** `exportKys(keymap)` writes the effective shortcuts as a Premiere key set (`<PremiereData Version="3">`,
`<shortcuts Version="5">`, the keymap's platform): one `<context.*>` per context, an item per shortcut of every command
with a `.kys` identity (`encodeKysVirtualKey` is `decodeKysVirtualKey`'s inverse over the verified codes; keys without
one are listed in `KysExport::skipped`), and an item without `<virtualkey>` for a command with no shortcut, so importing
it unbinds the command. `saveKys` writes the file; Keyboard Shortcuts > Export… saves one when the name ends in `.kys`.
`tests/keymap/test_kys_export.cpp` reads the result back with `parseKys` for both platforms.

**TODO (`.kys`).** One source suggests that Premiere 27 nests contexts under `mode.Edit` / `mode.Color`
(`/PremiereData/shortcuts/mode.Edit/context.global/…`). No such file has been seen, so the importer rejects that
layout as Unsupported instead of guessing. Verify it with a real Premiere 27 key set before adding it. The same
applies to virtual keys 5 and 19–30, which export leaves out (`KysExport::skipped`).

## Keyboard Shortcuts editor (UI)

Edit > Keyboard Shortcuts: `edit.keyboardShortcuts`, Ctrl+Alt+K on Windows/Linux and ⌥⌘K (Opt+Cmd+K) on macOS, as in
Premiere. It is also in the Edit and Help menus and the command palette. M5f; owner: Timeline/UI.

- **Code:** `ShortcutsBridge` (QML singleton `Shortcuts`, `src/app/src/bridge/ShortcutsBridge.*`) and
  `src/app/qml/shortcuts/`.
  - Every edit is made on a copy of the dispatcher's `KeymapManager` with the engine API (`assign`, `unassign`,
    `resetToDefault`, `applyKysImport`, `loadOverrides`).
  - It is then committed at once with `KeymapDispatcher::setOverrides(overrides)`. That method is C++ only: it
    replaces every user override atomically on a copy, swaps it in, saves `overridesPath` and emits `keymapChanged`.
  - The keys that are dispatched therefore change immediately, and the overrides file stays the only persisted
    state.
- **Keyboard:** a US ANSI keyboard with the navigation block and keypad (`Shortcuts.keyboard`, positions in key
  units).
  - Each key is filled by what it does with the modifiers shown (`keyAssignments(modifiers)`):
    - application commands: `Theme.keyApplication`, violet;
    - panel commands: `Theme.keyPanel`, teal;
    - both: split.

    These are Premiere's purple/green, and they do not follow the accent.
  - Shifted US symbols are drawn on their base key ("+" is Shift+=). Keypad keys are separate from the main row.
  - Clicking a modifier key, or a modifier toggle, shows another combination.
  - Clicking a key lists what it does in every panel (`commandsForKey`), with remove buttons and "Give to <the
    command chosen in the list>".
- **Commands:** `Shortcuts.commands`, in registry order, in sections by group.
  - Search matches the Wabcut name, Premiere's name, the id, the group, or a shortcut ("Ctrl+K" finds Add Edit). A
    filter limits it to application commands, all panels, or one panel.
  - Each row shows the shortcuts as keycaps (× removes one), + to type a new one, and a reset arrow when the command
    differs from the defaults.
  - Commands Wabcut does not run yet are dimmed. Their keys are kept.
  - Rows have a grip to drag the command onto a key.
- **Typing a shortcut:** the capture field takes every key (it accepts `ShortcutOverride`, so neither window shortcuts
  nor keymap commands fire) and Esc cancels. `chordText(key, modifiers)` turns the event into storage text.
- **Conflicts, before anything changes:**
  - `checkAssign(command, chord)` lists the commands in the same scope that would lose the chord. The editor asks
    "Reassign / Cancel", and `assign` then takes the chord from them, as Premiere does.
  - Premiere's third warning is shown too: a panel command and an application command on the same chord. For
    example, "While the Timeline has focus, “Clear” runs instead".
- **Key sets:**
  - **Premiere Pro (default):** the built-in table, with no overrides.
  - **Wabcut:** three changes. Ctrl+Y also redoes, on Windows. Backspace ripple-deletes in the Timeline, as in
    DaVinci Resolve and Final Cut Pro; Alt/Opt+Backspace still does too. Timeline "Clear" therefore loses Backspace;
    Delete still clears, application-wide.
  - **User sets:** "Save as a new key set…" writes the current overrides to
    `<overrides folder>/sets/<name>.<windows|macos>.json`.
  - The chosen set is remembered (`shortcuts/preset` in QSettings). The editor says "modified" when the keys differ
    from it, and Reset returns to Premiere Pro's defaults.
- **Import and export:**
  - **Import…** reads a Premiere `.kys` or a Wabcut overrides `.json`.
    - A `.kys` is merged into the current keys, as Premiere's import does.
    - The report names the Premiere commands that have no Wabcut equivalent (`KysImport::unmappedCommands`) and
      counts the commands that changed.
  - **Export…** writes the overrides as `.json`: the overrides schema above, readable by Import.

## Extension commands (C.12)

Extensions add commands with stable ids `ext:<extension id>/<command id>`. The app registers them at run time
(`KeymapDispatcher::setExtraCommands`): Application scope, group "Extensions", no default key. They are assignable in
Keyboard Shortcuts and saved with the user's overrides. A saved key for an `ext:` id whose extension is off or not
installed is kept: the command stays listed as "<id> (unavailable)", and its key says why nothing runs. .kys import
never maps to them.

## Known gaps and next steps

- **App wiring:** the bridge exists; the UI shell still has to attach it and declare panel scopes, and the
  M5 timeline and playback owners have to consume `commandTriggered` / `transportIntent` (see App integration).
- **Popups without focus:** a modal popup that does not take focus does not block shortcuts; give such popups
  `focus: true`.
- **Lost key-ups:** macOS can drop a key-up while ⌘ is held. The dispatcher heals this on the next press of that
  key and on focusLost(); until then K counts as held, so J/L step frames instead of shuttling. A bridge that sees
  modifier changes without a K event could also call focusLost().
- **Layouts:** shifted-symbol matching assumes a US layout. On other layouts, Premiere-style punctuation shortcuts
  need the bridge to pass layout-independent keys (for example from `nativeVirtualKey()`).
- **Coverage:** commands for features Wabcut does not have yet (captions, graphics, workspaces) are registered so
  the tables stay a complete transcription; the app can show them disabled. Premiere commands beyond the 2026 page
  (Select Next Clip, multicam, and so on) can be added when their features exist, together with their `.kys`
  identities. M5i's Trim Backward/Forward (Many), Toggle Trim Type and Expand/Minimize All Tracks were verified in
  M5j against both default key sets; their identities and corrected scopes are recorded in footnotes 13, 15 and 16.
- **Editor UI:** done in M5f (see "Keyboard Shortcuts editor (UI)"), including Premiere's third warning type,
  "panel shortcut overrides an application shortcut". Still missing: export to `.kys`, and keyboards other than US
  ANSI.
