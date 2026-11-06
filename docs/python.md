# Scripting Wabcut with Python (0.7)

Wabcut has one typed command surface ([commands.md](commands.md)): every edit the app, `wabcut-cli`, the AI assistant,
the [MCP server](mcp.md) and the [extensions](extensions.md) make is one of its commands, checked against a schema,
previewable, and one undo step. The `wabcut` Python package puts the same surface in your own Python, for the jobs you
would rather write than click: grade every clip of a recording, build a rough cut from a shot list, add chapter markers
from a spreadsheet, rename clips, export markers, check loudness before delivery, or run the same recipe on a folder of
projects on a render machine.

You can use it two ways:

| | From a terminal, a build script or a render machine | From Wabcut's **File > Run Python Script…** |
|---|---|---|
| Opens | a project **file**, with no window | the project that is **open** |
| Edits | saved to the file when you ask (`save="exit"`) | one undo step in the open project |
| Permissions | the options you pass to `connect()` | the switches you turn on in the dialog |

Both run **your own Python** (3.11 or newer). Wabcut itself runs only typed commands, never the script's code; nothing
here opens a network port.

## Quick start

```python
import wabcut

with wabcut.connect("film.wabcut", edit=True, save="exit") as session:
    summary = session.project.summary()                         # a read returns its data
    print(summary["sequences"][0]["name"])
    clips = wabcut.records(session.clip.find(name="intro"), "clips")
    session.clip.rename(clips=[c["id"] for c in clips], name="Intro")      # an edit returns a Result
    with session.transaction("Chapters"):                       # one undo step, all or nothing
        session.marker.add(time="00:00:00:00", name="Start", type="chapter")
        session.marker.add(time="00:01:30:00", name="The turn", type="chapter")
```

`connect("film.wabcut")` starts `wabcut-cli` for you (it needs no window and works on a machine with no display). The
project is **read-only** until you pass `edit=True`, and nothing is written to the file until you save: call
`session.save()`, or open the session with `save="exit"` (saved when the block ends without an error) or `save="auto"`
(after every change). A script that raises an error saves nothing.

Install the package from the `python` folder that ships next to `wabcut.exe` / `wabcut-cli`:

```
pip install "<the Wabcut folder>/python"
```

or skip installing and point Python at the folder (`set PYTHONPATH=<the Wabcut folder>\python` on Windows,
`export PYTHONPATH=<the Wabcut folder>/python` elsewhere). If the folder is read-only (a system-wide install), copy it
somewhere you can write first. The package has no dependencies. `python -m wabcut doctor film.wabcut` shows which
`wabcut-cli` it found and opens the project once to prove it works; set `WABCUT_CLI` (or pass `cli=`) when `wabcut-cli`
is not next to the package or on the `PATH`.

## Running a script from Wabcut

**File > Run Python Script…** (also in the command palette, with your recent scripts) opens one dialog:

1. **Choose** a `.py` file.
2. **Review** it. The dialog shows the script's name and what it asks for, as switches. *Read your project* is always on.
   *Edit your sequences*, *Change the project's structure*, *Analyse media and use AI features* and *Use commands that
   read or write files* appear only when the script asks for them, and you can leave any of them off: the script gets
   what it asks for **and** you allow, exactly like an [extension](extensions.md#permissions-and-trust). A choice can be
   remembered for this exact version of the script (its SHA-256): if the file changes, you are asked again. The dialog
   also shows the Python it will use; none is bundled, so Wabcut looks when the dialog opens (your own choice, then
   `WABCUT_PYTHON`, the `py` launcher, `python3` / `python` on the PATH) and says what to do if it finds none.
3. **Run.** The script's output and errors stream into the dialog. **Stop** ends it.

The dialog is modal while a script runs, so the project is not edited under it. The script works on a **copy** of the
project, on its own thread, so Wabcut stays responsive however long a command takes. When the script ends cleanly, what
it did becomes **one undo step**, "Script: <name>", on the open project. A script that fails, is stopped, or loses its
connection leaves the project exactly as it was; if you changed the project some other way meanwhile, the script's edits
are not applied and Wabcut says so. Saving stays yours (File > Save).

A script says who it is and what it needs in comment lines near its top:

```python
# wabcut-name: Markers from a table
# wabcut-description: One marker per row of markers.csv.
# wabcut-permissions: Read, EditSequence
```

The permission names are `Read`, `EditSequence`, `EditProject`, `MediaAnalysis` and `Files` (see
[Permissions](#permissions)). A script without the line asks only to read. Inside Wabcut the script calls
`wabcut.connect()` with no arguments and gets the open project; the sequence open in the timeline is the one commands
work on unless a call names another.

```python
import wabcut

with wabcut.connect() as session:                # the open project, with the permissions you gave
    for clip in wabcut.records(session.clip.find(name="intro"), "clips"):
        print(clip["id"], clip["name"], clip["start"])
```

Print what you want to see: it appears in the dialog. An uncaught error shows its traceback there.

## The `wabcut` package

### Connecting

```python
wabcut.connect(project=None, *, edit=False, analysis=False, files=False, save="manual", timeout=600.0,
               cli=None, audit_log=None, env=None, sequence=None) -> Session
```

| Option | Meaning |
|---|---|
| `project` | the `.wabcut` file; leave it out inside Wabcut |
| `edit` | allow edit commands, undo, redo and transactions |
| `analysis` | also allow the analysis commands (transcription, loudness, …): they decode media and can run long |
| `files` | allow the commands that read or write files (imports, exports, merges), inside the project's folder only; pass a list of folders to add more |
| `save` | `"manual"`, `"exit"` or `"auto"` (needs `edit=True`) |
| `timeout` | longest one call may run, in seconds |
| `sequence` | the sequence (id or exact name) that commands work on when a call names none; without it a project with several sequences needs one named in each call |
| `audit_log` | a file that gets one JSON line per call: time, tool, outcome, revision; never parameters or answers |

Two sessions on the same project file are independent copies; whichever saves last wins.

### Commands

Every command of [commands.md](commands.md) is `session.<group>.<command>(**parameters)`: `timeline.rippleDelete` is
`session.timeline.rippleDelete(...)`, `export.queue.list` is `session.export.queue.list()`. Names and parameters are
spelled exactly as in the catalog. A parameter that is a Python keyword takes a trailing underscore
(`session.sequence.summary(from_="00:00:10:00")`); the dict form `session.media.newSynthetic({"kind": "colorMatte"})`
takes any name.

* A **read** returns the command's data: `session.project.summary()`, `session.marker.list()["markers"]`.
  Many reads answer in a compact "fields + rows" form; `wabcut.records(data, "clips")` turns the rows into dicts.
* An **edit** returns a `Result`: `committed`, `revision`, `created_ids` (the ids of what it created),
  `changes`, `warnings`, `data` (the command's own answer) and, for a batch, `steps`.
* Every edit takes `dryRun=True` (preview, nothing changes), `expectedRevision=n` (fail with `StaleRevisionError` unless
  the project is at revision n) and `idempotencyKey="k"` (a repeated call returns the first result).
* Times are timecodes in the sequence's frame rate (`"00:01:02:03"`) or ticks; tracks are `"V1"`, `"A1"`; a clip is its id
  or `{"track": "V1", "time": "00:00:05:00"}`.
* Parameters are checked **before** anything is sent, against the same schema the server uses, and checked again by the
  server. A bad call raises `InvalidParamsError` naming the path of each problem and changes nothing.

Help is built in: `python -m wabcut commands marker` lists matching commands, `python -m wabcut describe clip.addEffect`
prints one's parameters and description, and `session.describe("clip.addEffect").help()` does the same in code. The
package ships type stubs generated from the catalog (`_commands.pyi`), so an editor completes command and parameter
names, shows each command's documentation, and flags a wrong type before you run anything. `session.command(name,
params)` runs a command by name, including one a newer Wabcut has that this package does not know yet.

### One undo step: batches and transactions

* `session.batch([("marker.add", {...}), ("clip.rename", {...})], label="…")` sends several commands as **one** request:
  all or none, one undo step; `Result.steps` holds each command's answer. The first failure stops it and changes nothing.
* `with session.transaction("name"):` makes everything inside the block one undo step named `name`, and keeps it **only
  if the block finishes**. An exception (or the script ending, or the connection dropping) rolls everything back, to the
  state at the start of the block. Undo, redo and `save()` are refused inside the block, and an autosave waits for it.

```python
try:
    with session.transaction("Import the shot list"):
        for row in rows:
            session.clip.insert(media=row["media"], at=row["at"])   # a bad row raises ...
except wabcut.CommandError as error:
    print("nothing was changed:", error)                             # ... and none of the rows stays
```

### Errors

Everything the package raises on purpose is a `wabcut.WabcutError`. A command that was refused is a
`wabcut.CommandError` with `.command`, `.code`, `.message`, `.details` and `.client_side` (the package refused it before
sending); the class is the code's own:

| Code (class) | When |
|---|---|
| `InvalidParams` (`InvalidParamsError`) | a parameter fails the schema; `details["issues"]` lists each with its path |
| `NotFound` (`NotFoundError`) | a sequence, track, clip, media item or marker does not exist |
| `PermissionDenied` (`PermissionDeniedError`) | the session may not run it: read-only, analysis or file commands not allowed |
| `StaleRevision` (`StaleRevisionError`) | `expectedRevision` is not the project's revision (`details["current"]`) |
| `EditRejected` (`EditRejectedError`) | the edit is not possible: a locked track, nothing to undo, a transaction is open |
| `LockedTrack`, `InvariantViolation`, `Cancelled`, `TimedOut`, `IdempotencyConflict`, `UnknownCommand`, … | likewise, one class each |

Other errors: `CliNotFoundError` (no `wabcut-cli`), `ConnectionLostError` (Wabcut went away; `.stderr` says why),
`CallTimeoutError` (a call was cancelled after `timeout`), `HandshakeError`, `NotSupportedError`, `ProtocolError`.
Ctrl+C in a waiting script cancels the running call first, so nothing half-done is committed.

### Events

`session.on("commandApplied", handler)` calls `handler(event)` after every call this session made that changed the project
(`label`, `source` = `edit` / `undo` / `redo`, `revision`, `command`), and `"saved"` after `save()`. `session.events()`
returns what was recorded. These are the session's own activity: the package does not watch what other windows or people
do to the project.

### Rendering

`session.render_to("out.mp4", preset="youtube-1080p")` renders the project with the edits made so far, using
`wabcut-cli render` (`preset`, `sequence`, `range`, `encoder`, `audio=False` and `extra_args`). Edits that are not saved
are rendered from a temporary copy beside the project; the project file is not touched. It needs a headless session; a
script run from Wabcut uses the export commands (`session.export.queue(...)`) instead.

## Examples

The `python/examples` folder has six scripts that run both ways (`python script.py film.wabcut …` or from the Scripts
menu). The first four are for **video essayists and documentary makers**, the next two suit **gamers**, and all of them
are one undo step.

| Script | What it does |
|---|---|
| `markers_from_csv.py` | a marker for each row of `markers.csv` (`time,name,type`): chapters for an essay, or the clutches and aces from a game's event log |
| `export_markers.py` | writes the YouTube chapter list and a spreadsheet of the markers; read-only |
| `sequence_from_csv.py` | a rough cut from `shotlist.csv` (`media,in,out,title`): the shots in order, a chapter marker on each |
| `rename_from_csv.py` | renames clips from `renames.csv` (`old,new`), e.g. camera file names to scene names |
| `apply_grade.py` | gives every clip of one media the same colour grade, in every sequence |
| `loudness_qc.py` | measures the mix against a target (default −14 LUFS, true peak under −1 dBTP) and exits 1 when it is off, for a render farm or a delivery script |

A batch job over a folder of projects, for a render machine:

```python
import pathlib, wabcut

for project in sorted(pathlib.Path("D:/Episodes").glob("*/*.wabcut")):
    with wabcut.connect(project, edit=True, save="exit", audit_log="episodes-audit.jsonl") as session:
        session.color.grade.set(clips=[...], params={"basic.exposure": 0.2})
        session.render_to(project.with_suffix(".mp4"), preset="youtube-1080p")
```

## Permissions

What a script may do is decided by Wabcut, never by the script:

| Permission | Lets the script |
|---|---|
| `Read` (always) | see the project's sequences, clips, markers, media names, transcripts and captions |
| `EditSequence` | change clips, markers, captions, effects and audio inside sequences |
| `EditProject` | add, change and remove tracks, bins, media, subclips and sequences. The export queue, the export presets and watch folders and your effects library are not reachable from a script, and what a script changes in the project is undone with it |
| `MediaAnalysis` | run analysis and AI commands (transcription, search, translation, dubbing). Cloud AI still asks for your consent and stays within your spend cap and no-cloud settings; a script cannot give consent |
| `Files` | use the commands that read or write files (imports, exports, merges) **inside the project's folder**: Wabcut's commands refuse other places. This is a rule for the commands, not a sandbox for the script (see below); the script also gets the path of `wabcut-cli` (`WABCUT_CLI`) only with this permission |
| network | never: no command reaches the network on a script's behalf |

* **A headless session** gets what `connect()` was given: read-only by default. A call the session may not make is refused
  twice, by the package and by the server (`PermissionDeniedError`); the server does not even list the commands it will
  not run.
* **In Wabcut** a script gets what it asks for in its `wabcut-permissions` line **and** you switch on. Commands that
  would load native code (`plugins.scan`) are never available to a script.
* **The audit trail:** a headless session writes one JSON line per call to `audit_log` (never parameters or answers).
  Inside Wabcut the dialog shows the number of commands and edits, and Wabcut appends the same kind of line (time, script
  name, command, outcome) to `script-audit.jsonl` in its data folder, with a line when a run starts and when it ends.

What this does **not** do: your script is your own Python and runs with your account. Wabcut limits what a script can do
**to the project**; it cannot stop the Python code from reading your files or using the internet by itself (the dialog
says so). Run only scripts you trust. On Windows, macOS and Linux alike the script is an ordinary child process of Wabcut;
it is not sandboxed. (Extensions, which are JavaScript in a sandboxed helper, are the choice for code you do not trust.)

### How the connection is protected

* A headless session talks to the `wabcut-cli mcp` process it started, over that process's standard input and output.
  Nothing else can reach that pipe, and there is no port.
* A script run from Wabcut reaches the editor through a **named pipe** (Windows) or a **Unix socket** that exists for that
  one run: its name is random, only your user can open it, and the first line a client sends must be a one-time 128-bit
  token that only the script's environment holds (`WABCUT_SESSION`, `WABCUT_TOKEN`). One connection is accepted; it closes
  when the script ends. Wabcut starts no network listener for scripts, and an external program cannot attach to a running
  Wabcut (there is no "allow external scripts" switch).
* Wabcut's own API keys, set as `WABCUT_…` environment variables, are removed from a script's environment.

## Headless and render machines

`connect(project)` runs `wabcut-cli mcp --project <file> [--allow-edit] [--allow-analysis] [--allow-files]
[--save-on-exit | --autosave] [--timeout-ms n] --transactions [--audit-log file]` (see [mcp.md](mcp.md)); you can use those
flags from any other language. A render machine needs Wabcut's files, Python 3.11 or newer and no desktop session.
`wabcut-cli` exits with 2 when it cannot load the project, which `connect()` reports as `ConnectionLostError` with
`wabcut-cli`'s message. For a CI job, run your script like any other and use its exit code (`loudness_qc.py` ends with 1
when the mix is off target).

## How the package is built

`python/wabcut` is plain Python with no dependencies. Two files are **generated** from the command catalog by
`wabcut_command_docgen` (the same tool that writes commands.md and mcp.md) and must not be edited by hand:
`catalog.json` (every command's parameter schema, description and capability) and `_commands.pyi` (the typed stubs). The
tests "the Python package's generated files are up to date" and "the Python package's own tests pass against wabcut-cli"
keep them honest; regenerate after changing a command:

```
build\win-msvc\bin\wabcut_command_docgen.exe docs/commands.md docs/mcp.md docs/extensions.md
```

The package version is the Wabcut release it ships with (`python/wabcut/_version.py`). It is installed with the app
(`python` beside the executables, `Contents/Resources/python` in the macOS app) and ships in every package.

## Limits of this version

* A script run from Wabcut works on a copy of the project and is applied when it ends: it cannot see edits you make while
  it runs (the dialog is modal), and it applies all or nothing. Saving the project is yours.
* Events are the session's own; there is no live feed of what the user does in the editor.
* A running Wabcut cannot be attached to from a terminal. Run scripts from the Scripts menu, or open the project file
  headless.
* AI model commands run in the script's background worker and follow the editor's consent, spend cap and no-cloud rules: a
  provider you have not agreed to use in Settings is refused, and a script cannot consent for you.
