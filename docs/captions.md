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

## Not yet

- Premiere's `.prtranscript` files are not read or written.
- Spell check on macOS and Linux (a Hunspell-based checker) is not there yet.
