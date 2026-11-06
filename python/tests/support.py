"""Helpers shared by the tests: a fake Wabcut to talk to, and real projects for the end-to-end tests."""

from __future__ import annotations

import json
import os
import queue
import shutil
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any, Callable

import wabcut
from wabcut._transport import EOF, Channel, JsonRpcClient

TINY_PROJECT = {
    "bins": [],
    "format": "wabcut-project",
    "media": [],
    "nextId": 4,
    "schemaVersion": 8,
    "sequences": [
        {
            "audioRate": {"den": 1, "num": 48000},
            "audioTracks": [
                {"clips": [], "enabled": True, "id": 3, "locked": False, "muted": False, "name": "", "solo": False,
                 "syncLocked": True, "targeted": True, "transitions": []}
            ],
            "beatGrid": None,
            "bin": 0,
            "captionTracks": [],
            "dropFrame": False,
            "duration": 0,
            "effects": [],
            "frameRate": {"den": 1, "num": 25},
            "height": 180,
            "id": 1,
            "markIn": None,
            "markOut": None,
            "markers": [],
            "name": "Test sequence",
            "videoTracks": [
                {"clips": [], "enabled": True, "id": 2, "locked": False, "muted": False, "name": "", "solo": False,
                 "syncLocked": True, "targeted": True, "transitions": []}
            ],
            "width": 320,
            "workArea": None,
        }
    ],
}


def cli_path() -> Path | None:
    """The wabcut-cli the end-to-end tests use: $WABCUT_CLI, or none (the tests then skip with a clear message)."""
    found = os.environ.get("WABCUT_CLI")
    if found and Path(found).is_file():
        return Path(found)
    return None


class FakeChannel(Channel):
    """A Wabcut that answers from a table: `tools[name](arguments)` returns the tool's result (or raises ValueError to
    answer with a JSON-RPC error). Everything the client sent is kept in `sent`."""

    def __init__(self, tools: dict[str, Callable[[dict[str, Any]], dict[str, Any]]] | None = None) -> None:
        self.tools = tools or {}
        self.sent: list[dict[str, Any]] = []
        self._answers: "queue.Queue[bytes | object]" = queue.Queue()
        self.silent = False  #: never answer (for timeout tests)
        self.closed = False

    def write_line(self, data: bytes) -> None:
        message = json.loads(data)
        self.sent.append(message)
        if "id" not in message or self.silent:
            return
        method = message["method"]
        if method == "initialize":
            body: dict[str, Any] = {"result": {"protocolVersion": "2025-11-25", "serverInfo": {"name": "wabcut", "version": "0.0-test"}}}
        elif method == "tools/call":
            name = message["params"]["name"]
            handler = self.tools.get(name)
            if handler is None:
                body = {"error": {"code": -32602, "message": f"Unknown tool: {name}"}}
            else:
                try:
                    body = {"result": handler(message["params"]["arguments"])}
                except ValueError as error:
                    body = {"error": {"code": -32603, "message": str(error)}}
        else:
            body = {"error": {"code": -32601, "message": f"Method not found: {method}"}}
        self._answers.put(json.dumps({"jsonrpc": "2.0", "id": message["id"], **body}).encode())

    def read_line(self, wait: float) -> "bytes | None | object":
        try:
            return self._answers.get(timeout=wait)
        except queue.Empty:
            return None

    def end(self) -> None:
        self._answers.put(EOF)

    def close(self) -> int | None:
        self.closed = True
        return None

    def calls(self, tool: str | None = None) -> list[dict[str, Any]]:
        """The tools/call requests sent so far (of one tool, when given)."""
        out = [m["params"] for m in self.sent if m.get("method") == "tools/call"]
        return [c for c in out if tool is None or c["name"] == tool]


def structured(data: dict[str, Any], *, error: dict[str, Any] | None = None) -> dict[str, Any]:
    """A tools/call result carrying `data` as its structured content (or an error)."""
    if error is not None:
        body = {"ok": False, "revision": data.get("revision", 0), "error": error}
        return {"content": [{"type": "text", "text": json.dumps(body)}], "structuredContent": body, "isError": True}
    body = {"ok": True, **data}
    return {"content": [{"type": "text", "text": json.dumps(body)}], "structuredContent": body, "isError": False}


def fake_session(tools: dict[str, Callable[[dict[str, Any]], dict[str, Any]]] | None = None, *, edit: bool = True,
                 analysis: bool = False, files: bool = False, call_timeout: float | None = None,
                 origin: str = "cli") -> tuple[wabcut.Session, FakeChannel]:
    channel = FakeChannel(tools)
    session = wabcut.Session(
        JsonRpcClient(channel),
        access=wabcut.Access(edit=edit, analysis=analysis, files=files, save=edit),
        project=Path("film.wabcut"),
        call_timeout=call_timeout,
        origin=origin,  # type: ignore[arg-type]
    )
    return session, channel


class ProjectCase(unittest.TestCase):
    """A scratch folder per test, holding copies of the tiny project."""

    def setUp(self) -> None:
        self.folder = Path(tempfile.mkdtemp(prefix="wabcut-py-"))
        self.addCleanup(shutil.rmtree, self.folder, True)

    def project(self, name: str = "film.wabcut") -> Path:
        path = self.folder / name
        path.write_text(json.dumps(TINY_PROJECT, indent=2), encoding="utf-8")
        return path


class CliCase(ProjectCase):
    """End-to-end: needs wabcut-cli (WABCUT_CLI); skipped with a message when there is none."""

    def setUp(self) -> None:
        cli = cli_path()
        if cli is None:
            self.skipTest("set WABCUT_CLI to the full path of wabcut-cli to run the end-to-end tests")
        super().setUp()
        self.cli = cli

    def connect(self, project: Path, **options: Any) -> wabcut.Session:
        options.setdefault("cli", self.cli)
        options.setdefault("timeout", 120.0)
        return wabcut.connect(project, **options)


def wait_until(predicate: Callable[[], bool], seconds: float = 5.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()
