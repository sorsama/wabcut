"""Sessions: `wabcut.connect()` and what a script does with one."""

from __future__ import annotations

import contextlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterator, Literal, Mapping, Sequence

from . import _errors as errors
from ._catalog import Catalog, CommandInfo, load_catalog, normalize_params
from ._commands import Commands
from ._result import Result
from ._schema import validate
from ._transport import EOF, JsonRpcClient, LocalSocketChannel, ProcessChannel
from ._version import __version__

PROTOCOL_VERSION = "2025-11-25"
ENV_SESSION = "WABCUT_SESSION"  #: the address of the editor's script socket (set for a script run from Wabcut)
ENV_TOKEN = "WABCUT_TOKEN"  #: the one-time token for it
ENV_CLI = "WABCUT_CLI"  #: where wabcut-cli is, when it is not next to this package or on the PATH


@dataclass(frozen=True)
class Access:
    """What a session may do. The server enforces it; the package refuses early with a clear message."""

    edit: bool = False  #: edit commands (EditSequence, EditProject), undo, redo and transactions
    analysis: bool = False  #: MediaAnalysis commands (transcription, loudness, ...)
    files: bool = False  #: commands that read or write files, inside the project's folder (and granted folders)
    save: bool = False  #: the session has a project file to save to


def find_cli(explicit: str | os.PathLike[str] | None = None) -> Path:
    """Where `wabcut-cli` is: `explicit`, $WABCUT_CLI, next to the installed package, or on the PATH."""
    exe = "wabcut-cli.exe" if os.name == "nt" else "wabcut-cli"
    tried: list[str] = []
    here = Path(__file__).resolve().parent
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    elif os.environ.get(ENV_CLI):
        candidates.append(Path(os.environ[ENV_CLI]))
    else:
        # The installed layout: <install>/bin/python/wabcut next to <install>/bin/wabcut-cli; in a macOS bundle
        # Contents/Resources/python/wabcut next to Contents/MacOS/wabcut-cli.
        candidates += [here.parent.parent / exe, here.parent.parent.parent / "MacOS" / exe]
    for candidate in candidates:
        tried.append(str(candidate))
        if candidate.is_file():
            return candidate
    if not explicit and not os.environ.get(ENV_CLI):
        found = shutil.which("wabcut-cli")
        tried.append("wabcut-cli on the PATH")
        if found:
            return Path(found)
    raise errors.CliNotFoundError(
        "wabcut-cli was not found (looked at: " + "; ".join(tried) + "). Install Wabcut, or set the WABCUT_CLI "
        "environment variable (or pass cli=...) to the full path of wabcut-cli."
    )


class Session(Commands):
    """A project opened for a script: read it, edit it, undo, save.

    Every command of the catalog is an attribute path: `session.clip.addEffect(...)`, `session.project.summary()`.
    A command that edits returns a `Result`; one that reads returns its data. `session.command(name, params)` runs
    one by name; `session.batch([...])` runs several as one undo step; `with session.transaction():` makes everything
    inside one undo step that is kept only if the block succeeds.

    Use it as a context manager so the connection is closed:

        with wabcut.connect("film.wabcut", edit=True, save="exit") as session:
            ...
    """

    def __init__(
        self,
        client: JsonRpcClient,
        *,
        access: Access,
        project: Path | None = None,
        cli: Path | None = None,
        server_version: str = "",
        call_timeout: float | None = None,
        owner: ProcessChannel | None = None,
        origin: Literal["cli", "app"] = "cli",
        sequence: int | None = None,
    ) -> None:
        self._client = client
        self.access = access
        self.project_path = project
        self._cli = cli
        self.server_version = server_version
        self._call_timeout = call_timeout
        self._owner = owner
        self.origin = origin
        #: The sequence commands work on when a call names none: set from `connect(sequence=...)`, by the editor (the
        #: sequence open in the timeline), or by assigning it. None: a project with several sequences needs one named.
        self.sequence_id: int | None = sequence
        self._revision = 0
        self._dirty = False
        self._in_transaction: str | None = None
        self._events: deque[dict[str, Any]] = deque(maxlen=5000)
        self._handlers: dict[str, list[Callable[[dict[str, Any]], None]]] = {}
        self._closed = False
        self._lock = threading.RLock()
        self._temp_copies = 0

    # ------------------------------------------------------------------ introspection

    @property
    def revision(self) -> int:
        """The project revision after the last call this session made (every edit, undo and redo raises it)."""
        return self._revision

    @property
    def catalog(self) -> Catalog:
        """The commands this version of the package knows (`describe`, `commands` are shortcuts)."""
        return load_catalog()

    def commands(self, text: str | None = None) -> list[CommandInfo]:
        """The catalog's commands; with `text`, those whose name, title or description contain all its words."""
        catalog = load_catalog()
        return catalog.search(text) if text else [c for _, c in sorted(catalog.commands.items())]

    def describe(self, name: str) -> CommandInfo:
        """One command of the catalog; `print(session.describe("clip.addEffect").help())` documents it."""
        info = load_catalog().find(name)
        if info is None:
            raise self._unknown(name)
        return info

    # ------------------------------------------------------------------ running commands

    def command(
        self,
        name: str,
        params: Mapping[str, Any] | None = None,
        /,
        *,
        dryRun: bool = False,
        expectedRevision: int | None = None,
        idempotencyKey: str | None = None,
        validate_params: bool = True,
    ) -> Result | Any:
        """Runs a command by name. An edit returns a `Result`; a read returns the command's data.

        `dryRun=True` previews an edit without committing it; `expectedRevision=N` fails with StaleRevisionError unless
        the project is at revision N; `idempotencyKey` makes a retry return the first result. A command this version
        of the package does not know is sent as it is (`validate_params=False` skips the package's own check of the
        parameters; the server always checks).
        """
        info = load_catalog().find(name)
        envelope: dict[str, Any] = {}
        if dryRun:
            envelope["dryRun"] = True
        if expectedRevision is not None:
            envelope["expectedRevision"] = expectedRevision
        if idempotencyKey is not None:
            envelope["idempotencyKey"] = idempotencyKey
        if info is None:
            return self._call_unknown(name, dict(params or {}), envelope)
        merged, _ = normalize_params(info, dict(params or {}), {})
        return self._run(info, merged, envelope, validate_params)

    def query(self, name: str, params: Mapping[str, Any] | None = None, /) -> Any:
        """Runs a read command and returns its data; refuses a command that edits."""
        info = load_catalog().find(name)
        if info is not None and info.mutates:
            raise errors.PermissionDeniedError(
                info.name, "PermissionDenied", f"{info.name} edits the project; use session.command() for it",
                client_side=True,
            )
        return self.command(name, params)

    def _call_command(self, info: CommandInfo, params: dict[str, Any] | None, keywords: dict[str, Any]) -> Result | Any:
        merged, envelope = normalize_params(info, params, keywords)
        return self._run(info, merged, envelope, True)

    def _with_sequence(self, info: CommandInfo, params: dict[str, Any]) -> dict[str, Any]:
        """`params` with the session's default sequence filled in, for a command that takes one and was not given one."""
        if self.sequence_id is not None and "sequence" not in params and "sequence" in info.params.get("properties", {}):
            return {**params, "sequence": self.sequence_id}
        return params

    def _run(self, info: CommandInfo, params: dict[str, Any], envelope: dict[str, Any], check: bool) -> Result | Any:
        self._check_access(info)
        params = self._with_sequence(info, params)
        if check:
            issues = validate(info.params, params)
            if issues:
                first = issues[0]
                raise errors.command_error(
                    info.name,
                    "InvalidParams",
                    f"{first.path} {first.message}" + (f" (and {len(issues) - 1} more)" if len(issues) > 1 else ""),
                    {"issues": [{"path": i.path, "message": i.message} for i in issues], "clientSide": True},
                    client_side=True,
                )
        structured = self._tool(info.name, info.tool_name, {**params, **envelope})
        if not info.mutates:
            return structured.get("data")
        result = Result.from_structured(structured)
        self._after_edit(info.title, "edit", result, info.name)
        return result

    def _call_unknown(self, name: str, params: dict[str, Any], envelope: dict[str, Any]) -> Result | Any:
        """A command the package's catalog lacks (a newer Wabcut): sent as it is; the server decides."""
        structured = self._tool(name, name.replace(".", "_"), {**params, **envelope})
        if "committed" in structured or "basedOnRevision" in structured:
            result = Result.from_structured(structured)
            self._after_edit(name, "edit", result, name)
            return result
        return structured.get("data")

    def _check_access(self, info: CommandInfo) -> None:
        reason = None
        if info.mutates and not self.access.edit:
            reason = "this session is read-only: open it with wabcut.connect(project, edit=True)" if self.origin == "cli" \
                else "this script was not allowed to edit the project (allow editing when you run it)"
        elif info.capability == "MediaAnalysis" and not self.access.analysis:
            reason = "this session does not run analysis commands: open it with wabcut.connect(project, analysis=True)" \
                if self.origin == "cli" else "this script was not allowed to run analysis commands"
        elif info.touches_files and not self.access.files:
            reason = "this session may not use commands that read or write files: open it with " \
                "wabcut.connect(project, files=True)" if self.origin == "cli" else \
                "this script was not allowed to use commands that read or write files"
        if reason:
            raise errors.PermissionDeniedError(info.name, "PermissionDenied", reason, client_side=True)

    def _unknown(self, name: str, from_server: bool = False) -> errors.CommandError:
        """The exception for a command nobody knows (`from_server`: this Wabcut answered "unknown tool")."""
        catalog = load_catalog()
        info = catalog.find(name)
        if info is not None and from_server:
            if info.capability == "MediaAnalysis" or info.touches_files:
                what = "analysis commands" if info.capability == "MediaAnalysis" else "commands that read or write files"
                flag = "analysis=True" if info.capability == "MediaAnalysis" else "files=True"
                how = f"open it with wabcut.connect(project, {flag})" if self.origin == "cli" else "allow it when you run the script"
                return errors.PermissionDeniedError(info.name, "PermissionDenied",
                                                    f"this session does not serve {what}: {how}")
            return errors.UnknownCommandError(
                info.name, "UnknownCommand",
                f"Wabcut {self.server_version or '(unknown version)'} does not have {info.name}; the package is newer "
                "than the wabcut-cli it started: install the matching Wabcut or point WABCUT_CLI at it")
        hint = catalog.suggest(name)
        text = f"there is no command '{name}'" + (f" in Wabcut {self.server_version}" if self.server_version else "")
        if hint:
            text += f". Did you mean {', '.join(hint)}?"
        return errors.UnknownCommandError(name, "UnknownCommand", text, {"suggestions": hint}, client_side=True)

    def _tool(self, label: str, tool: str, arguments: dict[str, Any]) -> dict[str, Any]:
        """One `tools/call`: the structured answer, or the exception for its error."""
        with self._lock:
            if self._closed:
                raise errors.WabcutError("this session is closed")
            try:
                result = self._client.request(
                    "tools/call", {"name": tool, "arguments": arguments}, timeout=self._wait_limit()
                )
            except errors.ProtocolError as error:
                if error.code == -32602 and "Unknown tool" in str(error):
                    raise self._unknown(label, from_server=True) from None
                raise
        structured = result.get("structuredContent")
        if structured is None:
            for block in result.get("content", ()):
                if block.get("type") == "text":
                    with contextlib.suppress(ValueError):
                        structured = json.loads(block.get("text", ""))
                        break
        if not isinstance(structured, dict):
            raise errors.ProtocolError(f"{label}: the answer has no structured content")
        if "revision" in structured:
            self._revision = int(structured["revision"])
        if result.get("isError"):
            error = structured.get("error") or {}
            message = error.get("message", "the command failed")
            if "several sequences" in message and self.sequence_id is None:
                message += (' (name one in the call, or set a default: wabcut.connect(project, sequence=...) or '
                            "session.sequence_id = ...; session.project.summary() lists them)")
            raise errors.command_error(label, error.get("code", "Internal"), message, error.get("details"))
        return structured

    #: Seconds the package waits beyond the server's own time limit before it gives up on an answer.
    _grace = 30.0

    def _wait_limit(self) -> float | None:
        return None if self._call_timeout is None else self._call_timeout + self._grace

    # ------------------------------------------------------------------ batches, undo, save, transactions

    def batch(
        self,
        commands: Sequence[Mapping[str, Any] | tuple[str, Mapping[str, Any]] | tuple[str]],
        *,
        label: str | None = None,
        dryRun: bool = False,
        expectedRevision: int | None = None,
        idempotencyKey: str | None = None,
    ) -> Result:
        """Runs several commands in order as ONE undo step: all of them or none.

        Each item is `("clip.rename", {"clip": 7, "name": "Intro"})` or `{"command": "clip.rename", "params": {...}}`.
        Later commands see the earlier ones' results; `Result.steps` has each command's own answer. The first failure
        stops the batch and nothing changes.
        """
        catalog = load_catalog()
        items: list[dict[str, Any]] = []
        for index, item in enumerate(commands):
            if isinstance(item, Mapping):
                name, params = item["command"], dict(item.get("params") or {})
            else:
                name, params = item[0], dict(item[1]) if len(item) > 1 else {}  # type: ignore[misc]
            info = catalog.find(name)
            if info is not None:
                self._check_access(info)
                params, _ = normalize_params(info, params, {})
                params = self._with_sequence(info, params)
                issues = validate(info.params, params)
                if issues:
                    first = issues[0]
                    raise errors.command_error(
                        "edit_batch",
                        "InvalidParams",
                        f"/commands/{index}{first.path if first.path != '/' else ''} {first.message}",
                        {"issues": [{"path": f"/commands/{index}{i.path if i.path != '/' else ''}", "message": i.message}
                                    for i in issues], "clientSide": True},
                        client_side=True,
                    )
                name = info.name
            items.append({"command": name, "params": params})
        arguments: dict[str, Any] = {"commands": items}
        if label:
            arguments["label"] = label
        if dryRun:
            arguments["dryRun"] = True
        if expectedRevision is not None:
            arguments["expectedRevision"] = expectedRevision
        if idempotencyKey is not None:
            arguments["idempotencyKey"] = idempotencyKey
        if not self.access.edit:
            raise errors.PermissionDeniedError("edit_batch", "PermissionDenied", "this session is read-only",
                                               client_side=True)
        structured = self._tool("edit_batch", "edit_batch", arguments)
        result = Result.from_structured(structured)
        self._after_edit(label or f"Batch ({len(items)} commands)", "edit", result, "edit_batch")
        return result

    def undo(self) -> Result:
        """Undoes the last undo step (not available inside a transaction)."""
        return self._undo_redo("edit_undo", "undo")

    def redo(self) -> Result:
        """Redoes the last undone step."""
        return self._undo_redo("edit_redo", "redo")

    def _undo_redo(self, tool: str, kind: str) -> Result:
        if not self.access.edit:
            raise errors.PermissionDeniedError(tool, "PermissionDenied", "this session is read-only", client_side=True)
        result = Result.from_structured(self._tool(tool, tool, {}))
        self._after_edit(result.undone or result.redone or kind, kind, result, tool)
        return result

    def save(self, copy_name: str | None = None) -> Path | None:
        """Writes the project file (atomically, keeping rotated .bak copies). `copy_name` ("Rough cut v2.wabcut") saves a
        copy next to it instead. Not available inside a transaction. Returns the path written."""
        if not self.access.save:
            raise errors.PermissionDeniedError(
                "project_save", "PermissionDenied",
                "this session cannot save: it is read-only"
                if self.origin == "cli" else "a script cannot save the project; use File > Save in Wabcut",
                client_side=True,
            )
        arguments = {"copyName": copy_name} if copy_name else {}
        structured = self._tool("project_save", "project_save", arguments)
        if not copy_name:
            self._dirty = False
        self._record({"kind": "saved", "revision": self._revision, "file": Path(structured.get("path", "")).name})
        path = structured.get("path")
        return Path(path) if path else None

    @contextlib.contextmanager
    def transaction(self, label: str = "Script") -> Iterator["Session"]:
        """Everything edited inside the block is ONE undo step named `label`, kept only if the block finishes.

            with session.transaction("Tidy the intro"):
                session.clip.rename(clip=1, name="Intro")
                session.marker.add(time="00:00:05:00")

        An exception inside the block (or the script ending, or Wabcut closing the connection) rolls everything back:
        the project is as it was at the start. Undo, redo and save are refused until the block ends.
        """
        if not self.access.edit:
            raise errors.PermissionDeniedError("edit_transaction", "PermissionDenied", "this session is read-only",
                                               client_side=True)
        try:
            self._tool("edit_transaction", "edit_transaction", {"action": "begin", "label": label})
        except errors.ProtocolError as error:
            if error.code == -32602:
                raise errors.NotSupportedError("this Wabcut does not offer transactions (it is older than the package)") \
                    from None
            raise
        except errors.UnknownCommandError:
            raise errors.NotSupportedError("this Wabcut does not offer transactions (it is older than the package)") from None
        self._in_transaction = label
        try:
            yield self
        except BaseException:
            self._in_transaction = None
            with contextlib.suppress(errors.WabcutError):
                self._tool("edit_transaction", "edit_transaction", {"action": "rollback"})
            raise
        else:
            self._in_transaction = None
            structured = self._tool("edit_transaction", "edit_transaction", {"action": "commit"})
            if structured.get("committed"):
                self._dirty = True
                self._record({"kind": "commandApplied", "label": label, "revision": self._revision,
                              "command": "edit_transaction", "source": "edit"})

    # ------------------------------------------------------------------ events

    def on(self, kind: str, handler: Callable[[dict[str, Any]], None]) -> Callable[[], None]:
        """Calls `handler(event)` for each event of this kind this session produces; returns a function that stops it.

        Kinds: `commandApplied` (`label`, `source` "edit" | "undo" | "redo", `revision`, `command`) after every call that
        changed the project, and `saved` after `save()`. These are the session's own activity: the package does not
        watch what other people or windows do to the project.
        """
        self._handlers.setdefault(kind, []).append(handler)

        def off() -> None:
            with contextlib.suppress(ValueError):
                self._handlers.get(kind, []).remove(handler)

        return off

    def events(self, kind: str | None = None, *, clear: bool = False) -> list[dict[str, Any]]:
        """The events this session produced since it was opened (oldest first), or since the last `clear=True`."""
        found = [e for e in self._events if kind is None or e["kind"] == kind]
        if clear:
            self._events.clear()
        return found

    def _after_edit(self, label: str, kind: str, result: Result, command: str | None = None) -> None:
        if result.committed and not result.dry_run:
            self._dirty = True
            self._record({"kind": "commandApplied", "label": label, "source": kind, "revision": result.revision,
                          "command": command})

    def _record(self, event: dict[str, Any]) -> None:
        event = {"time": time.time(), **event}
        self._events.append(event)
        for handler in list(self._handlers.get(event["kind"], ())):
            try:
                handler(event)
            except Exception as error:  # a broken listener must not break the script's edit
                print(f"wabcut: an event handler raised {error!r}", file=sys.stderr)

    # ------------------------------------------------------------------ rendering

    def render_to(
        self,
        output: str | os.PathLike[str],
        *,
        preset: str | None = None,
        sequence: str | int | None = None,
        range: str | None = None,
        encoder: str | None = None,
        audio: bool = True,
        extra_args: Sequence[str] = (),
        timeout: float | None = None,
    ) -> Path:
        """Renders the project (with the edits made so far) to `output` with `wabcut-cli render`; returns the path.

        Edits not yet saved are rendered from a temporary copy next to the project; the project file is not touched.
        `preset` is an export preset id (`session.export.presets()` lists them), `range` is `all`, `in-out` or
        `work-area`. Headless sessions only: a script run from Wabcut uses the export queue (`session.export.queue`).
        """
        if self.origin != "cli" or self.project_path is None or self._cli is None:
            raise errors.NotSupportedError(
                "render_to needs a headless session (wabcut.connect(project=...)); use the export commands "
                "(session.export.quick, session.export.queue) from a script run inside Wabcut"
            )
        source = self.project_path
        temporary: Path | None = None
        if self._dirty:
            if not self.access.save:
                raise errors.NotSupportedError("the session has unsaved edits and cannot save a copy to render")
            self._temp_copies += 1
            name = f"wabcut-render-{os.getpid()}-{self._temp_copies}.wabcut"
            saved = self.save(copy_name=name)
            temporary = saved if saved else self.project_path.parent / name
            source = temporary
        destination = Path(output).resolve()
        argv = [str(self._cli), "render", str(source), str(destination)]
        if preset:
            argv += ["--preset", preset]
        if sequence is not None:
            argv += ["--sequence", str(sequence)]
        if range:
            argv += ["--range", range]
        if encoder:
            argv += ["--encoder", encoder]
        if not audio:
            argv.append("--no-audio")
        argv += list(extra_args)
        try:
            completed = subprocess.run(argv, capture_output=True, timeout=timeout, check=False,
                                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0)
        except subprocess.TimeoutExpired as error:
            raise errors.CallTimeoutError(f"the render did not finish within {timeout:g} s") from error
        finally:
            if temporary is not None:
                with contextlib.suppress(OSError):
                    temporary.unlink()
        if completed.returncode != 0:
            tail = (completed.stderr or completed.stdout).decode("utf-8", "replace").strip()[-2000:]
            raise errors.WabcutError(f"wabcut-cli render failed with code {completed.returncode}: {tail}")
        return destination

    # ------------------------------------------------------------------ lifetime

    def close(self) -> int | None:
        """Ends the session. A headless session saves the project first when it was opened with `save="exit"`.

        Returns wabcut-cli's exit code for a headless session. An open transaction is rolled back."""
        with self._lock:
            if self._closed:
                return None
            self._closed = True
        return self._owner.close() if self._owner is not None else self._client.channel.close()

    def abort(self) -> None:
        """Ends the session without saving anything (the process is stopped)."""
        with self._lock:
            already = self._closed
            self._closed = True
        if self._owner is not None:
            with contextlib.suppress(Exception):
                self._owner._proc.kill()
            self._owner.close(grace=5.0)
        else:
            if not already:
                # Inside Wabcut: say the session ended badly, so what it did is not applied even if the script then exits
                # normally (it caught the error).
                with contextlib.suppress(Exception):
                    self._client.notify("notifications/wabcut-abort")
            self._client.channel.close()

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        if exc_type is None:
            self.close()
        else:
            self.abort()

    def __repr__(self) -> str:
        mode = "edit" if self.access.edit else "read-only"
        where = self.project_path.name if self.project_path else "Wabcut"
        return f"<wabcut.Session {where} ({mode}) revision {self._revision}>"


def connect(
    project: str | os.PathLike[str] | None = None,
    *,
    edit: bool = False,
    analysis: bool = False,
    files: bool | Sequence[str | os.PathLike[str]] = False,
    save: Literal["manual", "exit", "auto"] = "manual",
    timeout: float | None = 600.0,
    cli: str | os.PathLike[str] | None = None,
    audit_log: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
    sequence: int | str | None = None,
) -> Session:
    """Opens a project for a script.

    * `connect("film.wabcut")`: a headless session on a project file. Wabcut's command line tool runs it for you; no
      window is needed, and it works on a machine with no display. The session is read-only unless `edit=True`.
    * `connect()` inside a script that Wabcut started from its Scripts menu: the project that is open in the editor,
      with the permissions you gave the script. Nothing is saved unless you save in Wabcut.

    Args:
        project: the .wabcut file; leave it out inside Wabcut.
        edit: allow edit commands, undo, redo and transactions (a headless session is read-only otherwise).
        analysis: also allow the analysis commands (transcription, loudness): they decode media and can run long.
        files: allow the commands that read or write files, inside the project's folder; a list of folders adds more.
        save: `"manual"` (call `session.save()`), `"exit"` (save when the session closes without an error) or
            `"auto"` (after every change). Needs `edit=True`.
        timeout: longest one call may run, in seconds (None: the server's own limit).
        cli: the path of wabcut-cli (default: $WABCUT_CLI, next to the installed package, or the PATH).
        audit_log: a file that gets one JSON line per call (time, tool, outcome); never parameters.
        env: environment for wabcut-cli (default: this process's).
        sequence: the sequence (its id, or its exact name) that commands work on when a call names none. Without it, a
            project with several sequences needs one named in each call. Inside Wabcut it is the sequence open in the
            timeline.
    """
    if project is None:
        session = _connect_editor()
        if sequence is not None:
            try:
                session.sequence_id = _resolve_sequence(session, sequence)
            except BaseException:
                session.abort()
                raise
        return session
    if save != "manual" and not edit:
        raise ValueError(f'save="{save}" needs edit=True')
    project_path = Path(project)
    if not project_path.is_file():
        raise errors.WabcutError(f"the project file {project_path} does not exist")
    project_path = project_path.resolve()
    cli_path = find_cli(cli)
    argv = [str(cli_path), "mcp", "--project", str(project_path)]
    if edit:
        argv += ["--allow-edit", "--transactions"]
    if analysis:
        argv.append("--allow-analysis")
    if files:
        argv.append("--allow-files")
        if not isinstance(files, bool):
            for folder in files:
                argv += ["--files-folder", str(Path(folder).resolve())]
    if save == "exit":
        argv.append("--save-on-exit")
    elif save == "auto":
        argv.append("--autosave")
    if timeout is not None:
        argv += ["--timeout-ms", str(max(1, min(3_600_000, int(timeout * 1000))))]
    if audit_log:
        argv += ["--audit-log", str(Path(audit_log).resolve())]
    channel = ProcessChannel(argv, dict(env) if env is not None else None)
    client = JsonRpcClient(channel)
    try:
        init = client.request(
            "initialize",
            {"protocolVersion": PROTOCOL_VERSION, "capabilities": {},
             "clientInfo": {"name": "wabcut-python", "version": __version__}},
            timeout=60.0,
        )
        client.notify("notifications/initialized")
    except BaseException:
        channel.close(grace=2.0)
        raise
    info = init.get("serverInfo", {}) if isinstance(init, dict) else {}
    session = Session(
        client,
        access=Access(edit=edit, analysis=analysis, files=bool(files), save=edit),
        project=project_path,
        cli=cli_path,
        server_version=str(info.get("version", "")),
        call_timeout=timeout,
        owner=channel,
        origin="cli",
    )
    if sequence is not None:
        try:
            session.sequence_id = _resolve_sequence(session, sequence)
        except BaseException:
            session.abort()
            raise
    return session


def _resolve_sequence(session: Session, sequence: int | str) -> int:
    """The id of a sequence given by id or exact name."""
    sequences = session.project.summary()["sequences"]
    for candidate in sequences:
        if str(candidate["id"]) == str(sequence) or candidate["name"] == sequence:  # "3" from a command line is an id
            return int(candidate["id"])
    names = ", ".join(f"{c['id']}: {c['name']}" for c in sequences)
    raise errors.NotFoundError("connect", "NotFound", f"the project has no sequence '{sequence}' (it has {names})")


def _connect_editor() -> Session:
    address = os.environ.get(ENV_SESSION)
    token = os.environ.get(ENV_TOKEN)
    if not address or not token:
        raise errors.WabcutError(
            "connect() needs a project file when the script is not run from Wabcut: "
            'wabcut.connect("film.wabcut"). Inside Wabcut, run the script from the Scripts menu.'
        )
    channel = LocalSocketChannel(address)
    hello = json.dumps({"wabcut": "hello", "token": token, "client": f"wabcut-python {__version__}"}).encode("ascii")
    channel.write_line(hello)
    deadline = time.monotonic() + 15.0
    answer: Any = None
    while time.monotonic() < deadline:
        line = channel.read_line(0.2)
        if line is None:
            continue
        if line is EOF:
            break
        answer = json.loads(line)
        break
    if not isinstance(answer, dict) or not answer.get("ok"):
        channel.close()
        reason = answer.get("error") if isinstance(answer, dict) else (
            "the connection closed without an answer (a wrong token, or the script run has already ended)")
        raise errors.HandshakeError(f"Wabcut did not accept this script: {reason}")
    client = JsonRpcClient(channel)
    try:
        client.request(
            "initialize",
            {"protocolVersion": PROTOCOL_VERSION, "capabilities": {},
             "clientInfo": {"name": "wabcut-python", "version": __version__}},
            timeout=30.0,
        )
        client.notify("notifications/initialized")
    except BaseException:
        channel.close()
        raise
    return Session(
        client,
        access=Access(edit=bool(answer.get("edit")), analysis=bool(answer.get("analysis")),
                      files=bool(answer.get("files")), save=False),
        project=None,
        server_version=str(answer.get("version", "")),
        call_timeout=None,
        origin="app",
        sequence=answer.get("sequence") if isinstance(answer.get("sequence"), int) else None,
    )
