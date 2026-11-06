"""A stand-in for the editor's private script connection: a named pipe (Windows) or Unix socket server that speaks what
Wabcut's script server speaks (the token line, then JSON-RPC), for testing the package's local channel without the app."""

from __future__ import annotations

import json
import os
import socket
import tempfile
import threading
import uuid
from typing import Any, Callable

TOKEN = "t0k3n-" + "0" * 26


class EditorServer:
    """One connection. `tools[name](arguments)` answers a tools/call with structured content; every line the client sent is
    kept in `lines`. `hello` is the answer to a good token."""

    def __init__(self, tools: dict[str, Callable[[dict[str, Any]], dict[str, Any]]] | None = None,
                 hello: dict[str, Any] | None = None, token: str = TOKEN) -> None:
        self.tools = tools or {}
        self.token = token
        self.hello = hello if hello is not None else {"ok": True, "edit": True, "analysis": False, "files": False,
                                                       "version": "0.7-test", "sequence": 7}
        self.lines: list[dict[str, Any]] = []
        self.rejected = False
        self.drop_after_requests: int | None = None  # close the connection after this many tools/call requests
        self.address = ""
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()
        self._dir: tempfile.TemporaryDirectory[str] | None = None

    # -- the protocol, independent of the channel
    def _serve(self, read_line: Callable[[], bytes | None], write_line: Callable[[bytes], None]) -> None:
        first = read_line()
        if first is None:
            return
        try:
            message = json.loads(first)
        except ValueError:
            message = {}
        if message.get("wabcut") != "hello" or message.get("token") != self.token:
            self.rejected = True
            write_line(json.dumps({"ok": False, "error": "the token is wrong"}).encode())
            return
        write_line(json.dumps(self.hello).encode())
        calls = 0
        while True:
            line = read_line()
            if line is None:
                return
            if not line.strip():
                continue
            request = json.loads(line)
            self.lines.append(request)
            if "id" not in request:
                continue
            method = request["method"]
            if method == "initialize":
                result: dict[str, Any] = {"protocolVersion": "2025-11-25", "serverInfo": {"name": "wabcut", "version": "0.7-test"}}
            elif method == "tools/call":
                calls += 1
                if self.drop_after_requests is not None and calls > self.drop_after_requests:
                    return
                handler = self.tools.get(request["params"]["name"])
                if handler is None:
                    write_line(json.dumps({"jsonrpc": "2.0", "id": request["id"],
                                           "error": {"code": -32602, "message": "Unknown tool: " + request["params"]["name"]}}).encode())
                    continue
                result = handler(request["params"]["arguments"])
            else:
                result = {}
            write_line(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}).encode())

    # -- Windows: a byte-mode named pipe
    def _start_pipe(self) -> None:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateNamedPipeW.restype = wintypes.HANDLE
        kernel32.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                              wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
        kernel32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
        kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, wintypes.LPDWORD, wintypes.LPVOID]
        kernel32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD, wintypes.LPDWORD, wintypes.LPVOID]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
        kernel32.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
        name = r"\\.\pipe\wabcut-test-" + uuid.uuid4().hex[:12]
        # PIPE_ACCESS_DUPLEX = 3, byte mode, wait, one instance
        handle = kernel32.CreateNamedPipeW(name, 3, 0, 1, 1 << 16, 1 << 16, 0, None)
        if handle in (None, wintypes.HANDLE(-1).value):
            raise OSError(ctypes.get_last_error(), "CreateNamedPipe failed")
        self.address = name

        def run() -> None:
            self._ready.set()
            kernel32.ConnectNamedPipe(handle, None)
            buffer = b""

            def read_line() -> bytes | None:
                nonlocal buffer
                while b"\n" not in buffer:
                    chunk = ctypes.create_string_buffer(1 << 16)
                    read = wintypes.DWORD(0)
                    if not kernel32.ReadFile(handle, chunk, 1 << 16, ctypes.byref(read), None) or read.value == 0:
                        return None
                    buffer += chunk.raw[: read.value]
                line, buffer = buffer.split(b"\n", 1)
                return line

            def write_line(data: bytes) -> None:
                payload = data + b"\n"
                written = wintypes.DWORD(0)
                kernel32.WriteFile(handle, payload, len(payload), ctypes.byref(written), None)

            try:
                self._serve(read_line, write_line)
            finally:
                kernel32.FlushFileBuffers(handle)  # DisconnectNamedPipe would drop what the client has not read yet
                kernel32.DisconnectNamedPipe(handle)
                kernel32.CloseHandle(handle)

        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()

    # -- POSIX: a Unix socket
    def _start_unix(self) -> None:
        self._dir = tempfile.TemporaryDirectory(prefix="wabcut-sock-")
        self.address = os.path.join(self._dir.name, "s")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)  # type: ignore[attr-defined]
        server.bind(self.address)
        server.listen(1)

        def run() -> None:
            self._ready.set()
            connection, _ = server.accept()
            buffer = b""

            def read_line() -> bytes | None:
                nonlocal buffer
                while b"\n" not in buffer:
                    chunk = connection.recv(1 << 16)
                    if not chunk:
                        return None
                    buffer += chunk
                line, buffer = buffer.split(b"\n", 1)
                return line

            def write_line(data: bytes) -> None:
                connection.sendall(data + b"\n")

            try:
                self._serve(read_line, write_line)
            finally:
                connection.close()
                server.close()

        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()

    def __enter__(self) -> "EditorServer":
        if os.name == "nt":
            self._start_pipe()
        else:
            self._start_unix()
        self._ready.wait(5)
        return self

    def __exit__(self, *exc: Any) -> None:
        if self._thread is not None:
            self._thread.join(timeout=5)
        if self._dir is not None:
            self._dir.cleanup()

    def environment(self, token: str = TOKEN) -> dict[str, str]:
        return {"WABCUT_SESSION": self.address, "WABCUT_TOKEN": token}

    def calls(self, tool: str) -> list[dict[str, Any]]:
        return [m["params"] for m in self.lines if m.get("method") == "tools/call" and m["params"]["name"] == tool]
