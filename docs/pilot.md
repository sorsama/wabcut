# Pilot: edit a real project with Wabcut

Automated tests show that Wabcut does what its authors meant. They cannot show whether a real edit, on real
recordings, feels right or survives an unusual file. The pilot answers that: a few people edit one real project each
with the latest release, write down every problem, and the problems get fixed in order of harm.

If you would like to take part, pick one of the four projects below, follow its checklist, and report what you find
with the **Pilot report** form on GitHub (Issues > New issue > Pilot report). One report per project is enough; add
single problems as you meet them, or all at the end.

## Before you start

- Install the latest Windows release from the [releases page](https://github.com/sorsama/wabcut/releases) (the MSI
  installs for your user only, no administrator rights; the portable ZIP installs nothing). The Linux and macOS
  packages are previews: reports from them are welcome, but say so in the form.
- **Work on copies of your recordings**, or keep a backup. Wabcut never changes source media, but a pilot is where
  surprises are found.
- Cloud AI features use your own API key, and the provider charges you for what you use. Every pilot project can be
  done without them (local transcription and the built-in tools are enough).
- Keep Wabcut's autosave on (it is on by default). If Wabcut crashes, the next start shows "Wabcut closed
  unexpectedly": it points at the Recovery dashboard and at the crash reports folder, and copies a short diagnostic
  summary (version, system, GPU and driver). Paste that summary into your report; attach the crash report only if
  you are happy to share it. Nothing is ever sent automatically.
- If you also have Premiere Pro, doing the same edit there is the most useful comparison you can give.

## The four pilot projects

Each project follows one workflow of the [getting started guide](getting-started.md). Do it the way you would for a
real upload; the checklist only makes sure the main paths get used.

### A. A long gaming video

From one or more real OBS, ShadowPlay or Game Bar recordings, 30 minutes or longer, to a 10-20 minute YouTube video.

- [ ] Start from the recording (drop it on Home). Check the audio tracks got the right names (Game, Mic, Discord).
- [ ] Let **Moments** find highlights and dead time; review the dead time before you remove it.
- [ ] Cut with the keyboard (J/K/L, I/O, Ctrl+K, ripple delete) and the transcript.
- [ ] Add at least one slow-motion moment, a callout or zoom, and music ducked under your voice.
- [ ] Export with the YouTube 1440p60 preset (or your usual one), then watch the whole file in another player.

### B. A Short or TikTok

From a gameplay clip with a facecam (or without one) to a 9:16 video under 60 seconds.

- [ ] Make the vertical version (Moments > Make Shorts version, or a Short from one highlight).
- [ ] Add word-by-word captions and fix at least two words by hand.
- [ ] Check the safe areas in the Program Monitor, then export with the Shorts preset.

### C. A video essay

From a voice-over recording, B-roll and music to a finished essay of 5 minutes or more.

- [ ] Transcribe the voice-over and cut it from the transcript (remove fillers and pauses).
- [ ] Place B-roll over the voice, with at least one title or quote card from the essay graphics.
- [ ] Let the music duck under the voice automatically; check the loudness reading before export.
- [ ] Export, and export the captions as an .srt file.

### D. A short documentary

From an interview filmed with two cameras (or one camera plus a separate audio recorder) to a 3-10 minute film.

- [ ] Sync the angles (by audio) and edit them as a multicam clip.
- [ ] Make the rough cut from the transcript, then refine it on the timeline.
- [ ] Do a simple colour pass (match the two cameras, one look on top).
- [ ] Export, then try sending the timeline to another editor (FCPXML or EDL) if you use one.

## What to write down

For every problem, as soon as you meet it:

1. **What you did**, step by step (the exact menu, button or shortcut).
2. **What you expected** to happen.
3. **What happened** instead. A screenshot or a short screen recording helps most.
4. **How bad it is:** it crashed or lost work, the export came out wrong, it blocked you, it slowed you down, or it is
   cosmetic.

Small irritations count. "I looked for X in the Clip menu and it was not there" is exactly the kind of note the pilot
is for.

Also note, at the end:

- how long the edit took, and how long you would expect it to take in the editor you normally use;
- the three things you liked most and the three that annoyed you most;
- your Wabcut version (Help > About Wabcut) and your hardware: Settings > Media & cache > "Performance check" shows
  the graphics card, the decoders and the export encoders Wabcut uses.

## What happens with the reports

Reports are sorted by harm and fixed in this order:

1. Crashes, hangs and lost work.
2. Wrong exports (picture, sound, timing, captions) and corrupted projects.
3. Anything that blocks a project from being finished.
4. Slowdowns and confusing behaviour.
5. Cosmetic problems.

Every fix of the first three kinds comes with an automated test that reproduces the problem, so it cannot come back
unnoticed. Fixed reports are closed with the release that contains the fix, and the [roadmap](roadmap.md) lists what
the pilot changed.

The pilot is done when all four kinds of finished edit exist, the reports are sorted, and every crash, lost-work and
wrong-export problem found is fixed and covered by a test.
