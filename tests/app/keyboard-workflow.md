# Keyboard workflow verification

The automated case `Keyboard only: import, insert, edit and export` drives the real hidden Qt shell with key
events and exports a temporary MP4. Run it through CTest after building `wabcut_app_tests`:

```powershell
ctest --test-dir build/win-msvc -R 'Keyboard only: import, insert, edit and export' --output-on-failure
```

The test creates its own still image and output directory. The native file chooser is outside this offscreen
harness: it hands the chosen path to the same `importUrls` handler, then uses keys for the remaining workflow.
No native-dialog or live screen-reader pass is implied by this test.

For an interactive acceptance pass when visible windows are authorized:

1. Start an empty project. Press Ctrl+I, type a media path in the native chooser, and press Enter.
2. Press Shift+1, Tab to the media card, and Enter to load it in the Source Monitor. Press comma to insert.
3. Press Shift+3, Home, then Right fifteen times. Press Ctrl+K to add an edit, D to select, and Delete to clear.
4. Tab to Timeline list and press Space. Use Home, Down and Enter to select a clip. Check the screen reader announces
   track, clip name, start, duration, effects and selection. Press Esc to return to the lanes.
5. Open Appearance with the keyboard. Choose high contrast and 125% or 150% interface size; restart to apply size.
6. Open Help, Keyboard Trainer. Choose a category, press Enter to start, answer using the active keymap, and press
   Esc to close. Focus must return to an available control.
7. Press Ctrl+M, Tab to the output path, enter a new path, Tab to Export and press Space. Verify the job completes.

The accessibility case traverses `QAccessibleInterface` roles and names; it is separate from listening with an
installed screen reader. Windows shortcuts above use the default Premiere keymap; imported maps use their own keys.
