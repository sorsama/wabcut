# Captions and transcripts: files, Find and Replace, spell check (0.2)

The caption workshop (C.7: caption tracks, cues, styles, presets, speakers; `captions.*`) and the transcript commands
(AI-3: `transcript.*`, in `wabcut_ai_edit`) gained Premiere Pro's file, search and spelling workflow in 0.2. Caption
Track Styles are described in [graphics.md](graphics.md) ("Caption Track Styles").

## Export transcript

`transcript.export` is Premiere's **Export transcript as a text file**: the sequence's transcript, with the project's
corrections and speakers, as paragraphs - split, like `transcript.text`, at speaker changes and pauses longer than
`maxPause` (default 1 s) - each with its start and end timecode and speaker. Unnamed diarization speakers are written
"Speaker 1", "Speaker 2"... as Premiere does; `"speakers": false` leaves the speaker line out.

- **txt** - one block a paragraph, a blank line between blocks:

  ```
  00:00:00:00 - 00:00:00:22
  Speaker 1
  So welcome to the show.
  ```

- **csv** - `Start,End,Speaker,Text` rows (RFC 4180 quoting), for spreadsheets.
- **docx** - the .txt layout as a Word document (timecodes in grey, speakers in bold, an empty paragraph between
  blocks), which Word, LibreOffice and Google Docs open (`wabcut/interchange/Docx.h`: Office Open XML written by
  Wabcut itself, stored ZIP entries, no dependency). A .docx needs `path` (there is no text to return).

Without `path` the text is returned in the result; with one, the format follows the extension (or `format`), an
existing file is replaced only with `"overwrite": true`, and a dry run writes nothing. The Captions page's
**Transcript** button saves either.

## Import captions

`captions.import` adds a caption track from a **SubRip (.srt)**, **WebVTT (.vtt)** or **timed text (.txt)** file - the
.txt layout above, so an exported transcript (Premiere's or Wabcut's) comes back as captions, one cue a paragraph, - or
the same layout in a **Word document (.docx)**: every paragraph and line break of `word/document.xml` is a line, tracked
deletions left out (the ZIP and XML are untrusted: the bounded ZIP reader, at most 16 MiB of document XML),
with the speaker when the project has a speaker of that name. File times are sequence times, plus `offset`. Styling
tags (`<i>`, `<v Speaker>`...) and WebVTT cue settings are dropped; overlapping cues are trimmed to the next cue's start;
cues without text or length are skipped and counted. The file is untrusted: at most 16 MiB, 100,000 cues and 4096 bytes
a cue (longer text is cut at a character boundary). The track takes the file's name unless `name` is given, and a
preset's style with `preset`. The Captions page's track menu has **Import captions...**.

This imports captions, not a transcript: Wabcut's transcripts come from transcription and live in the analysis cache
per source file. Premiere's `.prtranscript` files are not read.

## Shift captions (0.4)

An imported SRT that is half a second late is fixed in one step. The Captions page's track menu has **Shift
Captions…**; it opens a row under the track with:

- an **offset**: frames (`-12`) or a timecode in the sequence's rate (`-00:00:00:12`, `+00:00:01:00`, `-1:12`). A minus
  moves the captions earlier; without a sign they move later. An empty, zero or unreadable offset asks for one;
- the captions that move: **All captions**, or **From the playhead** (the caption under the playhead, else the first one
  starting after it, and every later one);
- **Shift** and **Cancel**.

Shift moves the cues of the track the page shows: one undo step, "Shift Captions". It is refused, with nothing changed,
when a caption would start before 0 or land on a caption that stays; the row says why. The row stays open after a shift
for another try, with the focus back in the offset field; Esc (wherever the focus is in the row) or Cancel closes it.
The palette's "Shift Captions" (`captions.shiftCues`) moves the track the Captions page shows too (the sequence's first
one when the page was never opened); it no longer asks for a track id.
Premiere selects every caption and nudges or drags them; a typed offset is exact.

## Find and Replace

Premiere's Find and Replace, as commands, each change one undo step:

- `captions.findReplace` - in the cues of one caption track (`captionTrack`) or of every caption track of the
  sequence. A replacement that would empty a cue is refused.
- `transcript.findReplace` - in the transcript: a match inside a word, or across consecutive words when the find text
  has spaces, is corrected exactly as `transcript.correctRange` does (saved with the project on top of the cached
  transcript; captions generated or updated afterwards read it). A match spanning two media items is left alone and
  counted.

- `captions.find` / `transcript.find` - the matches, in order (caption track, cue, byte range, the cue's time and text;
  in the transcript the words a match spans), for **Find Next** and **Find Previous**.
- **Replace** (one match): `captions.findReplace` with `at` (the match's caption track, cue and start byte) or
  `transcript.findReplace` with `words`; a match that is no longer there (the text changed) is refused. The caption
  result names the next match (`next`), wrapping round.

All take `find`, **Match Case** (`matchCase`) and **Whole Word** (`wholeWord`), the replacing ones `replace` (may be
empty). Matching is plain text. Without Match Case, letters match under Unicode simple case folding ("ÉCOLE" finds
"école", "Σ" finds "σ" and "ς"; a fold maps one letter to one, so "ß" does not find "ss"), generated from Unicode's
tables by `tools/gen_case_folding.py`. Whole Word treats letters, digits, marks and `_` of every script as word
characters, and spaces, punctuation, symbols and emoji of every script as boundaries - so in Thai, written without
spaces between words, Whole Word only finds text standing alone between spaces or punctuation
(`wabcut/model/TextFind.h`). A dry run reports the changes (`changes`: before and after, at most 200) without making
them. The Captions page's **Find and Replace** button opens a dialog with the scope (this track, all captions, the
transcript), the two options, **Find Previous** / **Find Next** (the caption is picked and the playhead goes to it; the
status line says "3 of 12"), **Replace** (this match, then the next) and **Replace All**.

## Spell check

Premiere checks the spelling of captions; Wabcut checks captions and the text of graphics (`wabcut/model/Spelling.h`):

- **The checker** is the operating system's: on Windows the Spelling Checking API, the dictionaries of Windows' own
  text boxes - every language the user installed (Settings > Time & language > Language & region; English is always
  there). A caption track is checked in its language (ISO 639-1 "en" takes the first installed English), a graphic's
  text layer in its own, else the user's. It finds misspellings itself (and repeated words: "the the"), and suggests
  corrections. Other platforms have no checker yet: nothing is underlined.
- **Your dictionary** is Wabcut's, not the system's: **Add to Dictionary** writes the word to `spelling-dictionary.txt`
  in the app's data folder (one word a line; `WABCUT_SPELLING_DICTIONARY` names another file, which wabcut-cli reads
  too), and **Ignore All** lasts until the app quits. Both match case-insensitively.
- **While typing**, misspelled words are underlined in red in a caption's text, the Essential Graphics text box and the
  Program Monitor's in-place text editor; right-click one for its suggestions, **Delete Repeated Word**, **Add to
  Dictionary** and **Ignore All**. A chosen suggestion is one edit.
- **Check Spelling** (the Captions page's spelling button) steps through the shown caption track's misspellings: the
  caption with the word marked, the suggestions, **Change** (this one), **Change All** (every whole-word, same-case
  occurrence in the track, one undo step), **Ignore**, **Ignore All** and **Add to Dictionary**; the playhead follows.
- **Commands** (`SpellingServices`: the app's assistant and wabcut-cli give the system checker with the user's
  dictionary): `captions.checkSpelling` (a caption track's misspellings: cue, byte range, word, the checker's advice -
  suggest, replace or delete - and up to 8 suggestions; `available` says whether the language can be checked),
  `graphic.checkSpelling` (a graphic clip's text layers) and `spelling.check` (any text). They only read; corrections
  are `captions.updateCue`, `captions.findReplace` and `graphic.updateLayer` edits.

## Speakers on the Transcript tab (0.6)

A paragraph of the Transcript tab is labelled and coloured by its project speaker - the name given in the Captions tab
or here, an assignment or a merge - and falls back to "Speaker 1", "Speaker 2"... (the transcriber's diarization label)
while nobody has named it. A new paragraph starts wherever the speaker changes. The label is a button; its menu has:

- **Rename or recolour...** - a name field and six colour swatches in the paragraph (Enter applies, Esc closes). A label
  nobody named yet becomes a project speaker for that label of the clip's media, so every paragraph the label speaks
  follows (`transcript.renameSpeaker` by media and label).
- **This paragraph is** - a project speaker, or **New speaker...** (a name field): the paragraph's words belong to them
  (`transcript.assignSpeaker` over the words' timeline range, on their audio track when they share one).
- **Merge into** - the speaker becomes the chosen one wherever it speaks, and goes (`transcript.mergeSpeakers`; a label
  with no speaker yet is named and merged in the same step).

With words selected, the actions bar has a **Speaker** button with the same choice for the selection. Each change is
one undo step; captions made from the transcript show the new names after "Update from transcript".

## Correct from your script (0.6)

When you read a script, you already have the exact words; the transcriber still misspells names and jargon ("web cut"
for Wabcut, "cooper nettie's" for Kubernetes). **Sequence > Captions > Correct Transcript from Script...** (also the
spell-check button at the top of the Transcript tab, and the command palette) opens a sheet: paste the script or open a
.txt, .md or .fountain file, then **Find corrections**.

- Every line of the script is looked for in the sequence's transcript (fuzzy, on this computer; Thai and other languages
  written without spaces too). Lines said more than once (retakes) are corrected in every take about as good as the best
  one. A line with a long aside in the middle, or half of it skipped, is found in parts near where it was said.
- Where the speech follows a line, the words the transcriber got wrong take the script's spelling, capitals and
  punctuation. That covers names and jargon, numbers and abbreviations written another way ("25" for "twenty-five",
  "Doctor" for "Dr."), and names or terms in the wrong capitals ("kubernetes" becomes "Kubernetes", "github" becomes
  "GitHub"; a capital that only starts a sentence is not copied).
- Only substitutions that sound like the script are made. A word you skipped is not added, a word you added (a filler,
  an aside) is not removed, and a word you changed ("great" where the script says "good") is left as you said it. The
  sheet lists those under **Left as said**.
- The sheet lists each correction as "time  before → after" (Capitals marks one that only changes capitals), the
  differences left as said, and the lines not found closely enough, which are left alone. Click a row to move the
  playhead there. Finding runs in the background with its progress, and **Stop** ends it. An hour of speech and a long
  script take a second or two.
- **Apply** makes all the corrections as one undo step ("Correct from Script"). If the project changed since you found
  them, they are found again so you can check them first. Corrections are saved with the project on top of the
  transcript, like a correction you type yourself, so transcribing again keeps them. A correction that overlaps one you
  made earlier is merged with it, so your earlier fix is kept.
- Captions made before this still read the old words. After Apply the sheet offers **Update captions**, and the Captions
  tab offers it too. It rewrites the cues' text from the corrected transcript and keeps their timing.

There is no accept or reject per change: undo, adjust the script and find the corrections again. The command is
`transcript.correctFromScript` (`text`, `split`, `minMatch` from 0.3 to 0.99, default 0.7, and `fixCase`); a dry run lists
up to 200 changes, 50 lines not found and 50 differences left as said.

## The caption keys and Sequence > Captions (0.6)

Premiere's four caption keys act on the caption track the Captions tab shows (the one the Program Monitor draws):

| Keys (Windows / macOS) | Row | What it does |
|---|---|---|
| Ctrl+Alt+A / Opt+Cmd+A | Add New Caption Track (Sequence > Captions, the Captions track menu) | A new empty track named "Captions", then "Captions 2", ...; it becomes the shown track, and the Transcript panel opens on its Captions tab. One undo step. |
| Ctrl+Alt+C / Opt+Cmd+C | Add Caption at Playhead (Sequence > Captions, the Captions page's At playhead) | A caption at the playhead (two seconds long, or up to the next caption), its text selected in the Captions tab so typing replaces "New caption". One undo step. |
| Ctrl+Alt+Down / Opt+Cmd+Down | Go to Next Caption Segment (Sequence > Captions) | The playhead to the next caption of the shown track, and that caption is selected (the timeline lane outlines it). It does not change the project. |
| Ctrl+Alt+Up / Opt+Cmd+Up | Go to Previous Caption Segment (Sequence > Captions) | The same, to the previous caption. |

The rows are in the Sequence menu's Captions submenu, with their keys, and in the command palette. A row greys out and
says why it cannot run: "Open a sequence first", "Add a caption track first", "A caption is already at the playhead",
"No caption after the playhead" or "No caption before the playhead". A key that cannot run shows the same sentence as a
toast. The palette used to mark these keymap rows "Not available yet"; it now gives the real reason.

## Captions in the Program Monitor (0.6)

With the captions shown in the picture, the Program Monitor treats the caption under the playhead as an object, as it
does titles and clips:

- **Click** a caption to select its cue: it is outlined in the picture, the Captions tab's list scrolls to it and the
  timeline's caption lane outlines it. A cue and clips are selected one or the other, as on the caption lanes.
- **Double-click** it to open its text in the Captions panel (the panel comes up if it was hidden).
- **Drag** the outlined caption to move the caption track's text block: its placement becomes Custom at the new place
  (Caption Style's Centre X / Y), kept inside the style's safe area. The whole drag is one undo step, "Move Caption". A
  keyframed position is changed in Caption Style instead (the monitor says so).

Typing the text in the monitor itself is for later. See docs/monitors.md.

## Caption files for every track, and with timeline hand-offs (0.6)

A multi-language delivery no longer means one save per language:

- **Captions page > All tracks…** picks a caption format (SRT, WebVTT, SCC, MCC, EBU STL, TTML, SMPTE-TT, DFXP, iTT),
  then a folder, and writes every caption track of the sequence that has captions as
  `<sequence> - <track name>[.<language>].<ext>` (`Episode 3 - English.en.srt`, `Episode 3 - Thai.th.srt`; characters a
  file name cannot hold become `_`, a second track of the same name gets ` (2)`). The page says "Saved 3 caption
  file(s)". Empty tracks are skipped.
- **File > Export Timeline… > Captions > Sidecar files** writes the same files next to the FCP7 XML, FCPXML, OTIO, EDL,
  AAF or OMF file (named after it), and the export report lists each track as carried with its file. Without it the
  report lists each caption track as dropped ("caption track 'English' (en, 214 cues) is not carried by timeline
  files"): none of these formats carries captions. The choice and the format are remembered.
- Every caption file the app writes goes through `captions.export`, so words the soundtrack bleeps (Hide profanity with
  a bleep, redactions) are replaced in the file as in exports; the Captions page's SRT and VTT buttons did not do this
  before 0.6.

Commands: `interchange.export` `"captionSidecars": "<format>"` (result `"captionFiles"`), `captions.export` per track;
the bridge call is `Captions.exportAllTracks(folder, format)`. See [interchange.md](interchange.md).

## Not yet

- Premiere's `.prtranscript` files are not read or written.
- Spell check on macOS and Linux (a Hunspell-based checker) is not there yet.
