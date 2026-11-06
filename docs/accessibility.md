# Accessibility

Wabcut can be used with the keyboard alone and with a screen reader such as NVDA. This page says what is there, how it
is checked, and what still needs a person with a screen reader to try.

## What is there

- **Keyboard.** Tab and Shift+Tab move through every control; Space and Enter press the focused one. Premiere's panel
  keys focus the Library (Shift+1), the Timeline (Shift+3) and Effects (Shift+7); Ctrl+F6 / Ctrl+Shift+F6 move between
  panel groups, and Ctrl+F searches the panel you are in. Esc closes the top popover or dialog and gives the focus back.
- **Screen readers.** Every button, field, slider, tab strip, toggle and list has a name, a role and its state
  (checked, selected, unavailable). Each panel is a named pane; each dialog is announced with its title. A greyed
  control says why in its description ("Undo: Nothing to undo").
- **The Timeline list** (the list button in the Timeline's toolbar) shows the timeline as tracks and clips in time
  order, each clip with its name, track, start, length, effects and whether it is selected.
- **Context menus from the keyboard** (0.7). The Menu key and Shift+F10 open the right-click menu of what is focused: in the
  Timeline the menu of the selected clip, transition or gap (the first targeted track's header menu with nothing
  selected; in the Timeline list, of the current row), on a track header's name its track menu, on a panel's tab its tab
  menu. **Select Label Group**, the 16 named label colours (Edit > Label, a clip's menu, a track header's menu) and a
  gap's selection (Sequence > Go to Gap > Next / Previous in Track, then Delete) need no pointer either.
- **High contrast** (Settings > Appearance): black or white surfaces, text at 7:1 or more, strong borders.
- **Interface size** (Settings > Appearance): 90, 100, 125 or 150 %. Sizes that would leave less than 1100 x 700 for the
  window on your screen are greyed. The new size applies when Wabcut restarts; "Restart now" does that (it asks about
  unsaved changes first). The window never opens larger than the screen's work area, at any size.
- **Reduce motion** (Settings > Appearance): no slides, glows or loops.

## How it is checked

- `tests/app/A11yAudit.h` checks names, roles and Tab order inside a panel or dialog; the suites run it over each panel.
- "3E Screen reader: Home, the editor panels, the menu bar, Export and Settings name every control"
  (`tests/app/test_screen_reader_p1.cpp`) walks Home, every built-in workspace (Edit, Audio, Export, Color, Captions),
  each menu of the menu bar and every Settings page, and fails on a control without a name or with the wrong role, an
  unnamed scroll bar or an unnamed dialog. It also reads the QML sources: every tab strip, toggle, icon button, slider
  and field there has to give a name of its own, so dialogs the walk does not open are covered too.
- The 0.6 pass also read the real UI Automation tree (what NVDA reads) of the running app on Home and in the editor.
  It found unnamed scroll bars, dialogs announced without their title, the Timeline pane announced as "Tracks", the
  meter's "Peak dB" with no value, and about fifty tab strips, sliders and fields in dialogs with no name. All are fixed.

## Manual NVDA checklist

Automated checks cannot hear what NVDA says or judge whether the order makes sense. Before a release, someone with NVDA
(free from nvaccess.org) should run this script with the keyboard only, the monitor turned away if possible. It takes
about 45 minutes. Note anything NVDA cannot read, reads wrongly, reads twice, or where you get stuck.

1. **Start.** Start Wabcut. NVDA should say the window's name and land on Home. Tab through Home: the side bar (Home,
   Projects, Learn, Settings), search, Open, Import, New project, the recent projects.
2. **New project.** Choose New project. Type a name, Tab to the folder and the settings, create it. NVDA should say
   when the editor is open.
3. **Import.** Ctrl+I, choose a video file in the Windows dialog. Shift+1 to the Library: NVDA reads the clip's name and
   length. Arrow keys move between items.
4. **Source and insert.** Enter opens the clip in the Source Monitor. Set In (I) and Out (O): is the timecode read? Press
   comma (Insert). Shift+3 to the Timeline, open the Timeline list: is the new clip read with its track and times?
5. **Cut.** Move the playhead (arrow keys, Shift+arrows), Ctrl+K to cut, select a part in the Timeline list and Delete.
   Is the result announced? Does Ctrl+Z say what it undid?
6. **Caption.** Open the Captions workspace (Window menu or the workspace tabs). Add a caption, type its text, set its
   length. Is each field named? Is the cue read in the list?
7. **Effects.** Shift+7 to Effects, search for "Gaussian Blur", apply it to the selected clip. In Effect Controls: are
   the effect's rows, values and keyframe buttons read?
8. **Export.** Ctrl+M. Tab through the Export workspace: preset, file name, folder, the estimate, the Export button.
   Export. Are the progress and the "Export finished" notice read?
9. **Menus and dialogs.** Alt opens the menu bar; arrows move through menus and submenus; a greyed row says why. Open
   Settings (Ctrl+,): each page, the search field, the Interface size row. Open Media & cache: the proxy and cache
   folder rows and their free space.
10. **Close.** Close the project with unsaved changes: is the "Save changes?" question read with its buttons?

Things to listen for in particular: whether a dialog's title is said once (not twice) when it opens, whether the
Timeline lanes (not the list) say anything useful, the order Tab takes through the Program Monitor's transport, and
anything that only works with the mouse (dragging in the timeline and monitors has menu or key equivalents; say if one
is missing).
