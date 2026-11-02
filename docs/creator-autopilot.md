# Creator autopilot

Design note for roadmap card #62 (Wabcut 0.6). The autopilot combines parts Wabcut already has (transcripts, smart
search over the library, B-roll generation with your own key, highlights, shot detection, chapters, Export Frame) into
three jobs a video essayist or a gaming creator does on every upload:

1. **Auto B-roll**: read what is said and put matching B-roll from your own library on a free track.
2. **Publishing kit**: a title, a description, chapters, tags and thumbnail candidates, written as files.
3. **Pacing match**: measure how often a reference video cuts and suggest cuts so your edit has a similar pace.

Rules for all three:

- **Preview first.** Nothing changes until you press Apply. B-roll and pacing previews draw on the timeline as a ghost
  (dashed new clips, cut lines) and list every suggestion; you can switch any suggestion off before you apply.
- **One undo step** per Apply ("Auto B-roll", "Pacing Match"). The publishing kit only writes files and never changes
  the project.
- **Off the GUI thread**, with progress and Stop, like the other creator tools.
- **Run again when the timeline changed.** If you edit the timeline while a B-roll, publishing kit or pacing job runs,
  or after its preview, the sheet says "The timeline changed since this preview" and shows **Run again**. Apply is
  refused for the old preview, so it never undoes your edit, and Write files is refused for an old kit, so its chapters
  and thumbnails always match the timeline. Run again repeats the job with the same options on the timeline as it is
  now (a kit written with your AI asks for your consent again when it needs it).
- **Works with no cloud at all.** Everything below has an offline path on this computer. Lower quality is fine there.
- **The cloud is opt-in and priced.** Two things can use your own AI key: generating B-roll for a line nothing in your
  library matches (the Generate panel, prefilled), and writing the kit's title, description and tags with your chat
  model. Both show the cost estimate and ask for consent before anything is sent. Only text (the transcript) is sent for
  the kit; the video never leaves the computer. A no-cloud project or Offline mode turns both off.
- **No uploading, no posting.** The kit is files in a folder you choose.

Where: **Sequence > Creator Autopilot...** in the menu bar (a keymap command, `sequence.creatorAutopilot`, so it is in
the command palette and can get a shortcut) and the sequence pill's menu next to Rough Cut from Script. One sheet with
three tabs: B-roll, Publishing kit, Pacing. Hidden while AI is turned off in Settings.

## Auto B-roll (`autopilot.broll`)

- The sequence's transcript is cut into lines (sentences, split at pauses). Each line gets keywords: its words weighted
  by how specific they are to this video (TF-IDF over the lines, stop words dropped).
- Each line is looked up in the library's video items that the sequence does not already use as A-roll:
  - **by picture**, when the local visual search model is installed (SigLIP 2, on this computer): the line's text
    against the library's indexed keyframes;
  - **by words**, always: the keywords against each item's name, bin, Description and Comment, its clip markers and its
    transcript. A word match on a clip marker or a transcript phrase uses that moment; otherwise the item's In/Out or its
    middle.
  The better of the two wins; weak matches are dropped.
- Placement: a clip per matched line, as long as the line (2 to 6 seconds), video only, at most 4 per minute, at least
  4 seconds apart, never over the first 3 seconds (the hook stays on you), the same shot never twice. It goes on the
  lowest video track above the A-roll that is free for every placement; if none is, a new top track named "B-roll".
- Lines with no match are listed with a prompt; **Generate...** opens the Generate panel prefilled with it and the
  line's time (cost and consent there, as always).

## Publishing kit (`autopilot.publishKit`)

- **Title**: three ideas from the most telling sentences (the hook counts extra), up to 70 characters. The sequence's
  own name is an idea too when it is not a default one.
- **Description**: a short lead (the most telling sentences, in order), the chapter list, and hashtags.
- **Chapters**: the sequence's chapter markers when it has them, else topic changes in the transcript (the Podcast
  workflow's topic finder). The first chapter is at 0:00; warnings say when YouTube would not show them (fewer than 3,
  or one shorter than 10 seconds).
- **Tags**: the most specific words and repeated two-word phrases, at most 15 and 500 characters in all.
- **Thumbnail candidates**: frames at highlight markers, chapter starts, the middle of shots and the most telling
  sentences, scored on this computer for sharpness, contrast, colour and exposure, at least 5 seconds apart. Written at
  1280 x 720 (cropped to fill) as JPEG under 2 MB, YouTube's thumbnail rules.
- You can edit the title, description and tags in the preview. **Write files** puts them in
  `<folder>/<sequence> - publishing kit/`: `title.txt`, `description.txt`, `chapters.txt`, `tags.txt`,
  `thumbnail-1.jpg`..., `publishing-kit.md` (everything on one page) and `publishing-kit.json`.
- **Write with my AI** (optional): your chat model rewrites the title ideas, description and tags from the transcript
  text and the chapter titles; the estimate is shown first. If its answer cannot be read, the offline kit stays.

## Pacing match (`autopilot.pacing`)

- Pick a reference video from the library (a video you like the pace of). Its cuts come from shot detection (cached
  per file, like Scene Edit Detection). The report shows both rhythms: cuts per minute, the median and average shot
  length, and the shot lengths over time as bars.
- The reference's shot length over its running time (8 parts) is the target for the same part of your edit. A shot of
  your edit longer than 1.35 times the target gets cuts, spaced like the reference, each moved to the best nearby place:
  a sentence end, a pause between words, a beat of the sequence's beat grid; never inside a word.
- Shots already shorter than the reference are reported, not changed: pacing match never removes footage.
- Apply adds the suggestions either as **markers** ("Cut (pace)", to cut by hand or drop B-roll on) or as **cuts**
  (Add Edit on all tracks), one undo step.

## Tests

The three commands are tested through the engine with fixtures (transcripts, a colour-word embedder, shots handed over
in memory, a fake frame source and a fake still writer): B-roll lands on the matching item and undoes in one step,
the kit's files are complete, and pacing match gives the cuts a known reference rhythm implies. The sheet has a UI test
with screenshots in the dark and light themes. No test needs the network, a model or a GPU.

## Left for later

- Punch-in zooms on pacing cuts (a visible jump cut), and B-roll from stock libraries.
- Thumbnail text overlays and face-aware crops.
