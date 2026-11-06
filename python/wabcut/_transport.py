"""How the package talks to Wabcut: JSON-RPC 2.0, one message per line, over a channel that only this script holds.

Two channels exist, both local and neither a network listener:

* `ProcessChannel`: the script starts `wabcut-cli mcp` and talks to it over its standard input and output. Nothing else
  can reach that pipe. This is the headless session (`wabcut.connect(project="film.wabcut")`).
* `LocalSocketChannel`: a named pipe (Windows) or a Unix socket the editor opened for one script run. The editor hands
  the script its address and a one-time token in the environment (`WABCUT_SESSION`, `WABCUT_TOKEN`); the first line the
  script sends is that token. This is how a script started from Wabcut's Scripts menu reaches the open project.

The protocol on top is the Model Context Protocol subset documented in docs/mcp.md (`initialize`, `tools/call`).
"""

from __future__ import annotations

import json
import os
import queue
import socket
import subprocess
import threading
import time
from collections import deque
from typing import Any

from ._errors import CallTimeoutError, CliNotFoundError, ConnectionLostError, HandshakeError, ProtocolError

MAX_MESSAGE_BYTES = 4 * 1024 * 1024  #: the server refuses longer lines
_SLICE = 0.2  #: how long a read waits before the caller gets the chance to notice Ctrl+C or a deadline


class _Eof:
    def __repr__(self) -> str:
        return "<end of stream>"


EOF = _Eof()


class Channel:
    """A line-oriented, full-duplex byte channel."""

    def write_line(self, data: bytes) -> None:
        raise NotImplementedError

    def read_line(self, wait: float) -> "bytes | None | _Eof":
        """The next line (without its newline), None when nothing arrived within `wait` seconds, EOF when closed."""
        raise NotImplementedError

    def close(self) -> int | None:
        raise NotImplementedError

    def diagnostics(self) -> str:
        """What the other side wrote on its error output, if anything."""
        return ""


class ProcessChannel(Channel):
    """A child process's standard input and output."""

    def __init__(self, argv: list[str], env: dict[str, str] | None = None) -> None:
        creation = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
        try:
            self._proc = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
                creationflags=creation,
            )
        except OSError as error:
            raise CliNotFoundError(f"cannot start {argv[0]}: {error}") from error
        self._lines: "queue.Queue[bytes | _Eof]" = queue.Queue()
        self._stderr: deque[str] = deque(maxlen=40)
        threading.Thread(target=self._pump_stdout, name="wabcut-stdout", daemon=True).start()
        threading.Thread(target=self._pump_stderr, name="wabcut-stderr", daemon=True).start()

    def _pump_stdout(self) -> None:
        assert self._proc.stdout is not None
        try:
            for raw in iter(self._proc.stdout.readline, b""):
                self._lines.put(raw.rstrip(b"\r\n"))
        except (OSError, ValueError):
            pass
        self._lines.put(EOF)

    def _pump_stderr(self) -> None:
        assert self._proc.stderr is not None
        try:
            for raw in iter(self._proc.stderr.readline, b""):
                self._stderr.append(raw.decode("utf-8", "replace").rstrip())
        except (OSError, ValueError):
            pass

    def write_line(self, data: bytes) -> None:
        assert self._proc.stdin is not None
        try:
            self._proc.stdin.write(data + b"\n")
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as error:
            raise ConnectionLostError("the connection to wabcut-cli is closed", self.diagnostics()) from error

    def read_line(self, wait: float) -> "bytes | None | _Eof":
        try:
            return self._lines.get(timeout=wait)
        except queue.Empty:
            return None

    def diagnostics(self) -> str:
        code = self._proc.poll()
        tail = "\n".join(self._stderr)
        if code is not None:
            tail = f"(wabcut-cli exited with code {code})" + (f"\n{tail}" if tail else "")
        return tail

    def close(self, grace: float = 20.0) -> int | None:
        """Ends the session: closes the input (the server finishes, saving on exit if asked to) and waits `grace`."""
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
        except OSError:
            pass
        try:
            return self._proc.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            return self._proc.wait()
        finally:
            for stream in (self._proc.stdout, self._proc.stderr):
                try:
                    if stream:
                        stream.close()
                except OSError:
                    pass


class LocalSocketChannel(Channel):
    """A named pipe (Windows) or Unix socket server the editor opened for this script."""

    def __init__(self, address: str, connect_timeout: float = 10.0, *, kind: str | None = None) -> None:
        """`kind` is "pipe" (a Windows named pipe) or "unix" (a Unix socket); by default what this system uses."""
        self._buffer = b""
        self._closed = False
        self._pipe = (kind or ("pipe" if os.name == "nt" else "unix")) == "pipe"
        deadline = time.monotonic() + connect_timeout
        if self._pipe:
            self._open_pipe(address, deadline)
        else:
            self._open_unix(address, deadline)

    # -- Windows: a byte-mode named pipe. Reads are polled with PeekNamedPipe, because a thread blocked in ReadFile on the
    #    same synchronous handle would hold up WriteFile.
    def _open_pipe(self, address: str, deadline: float) -> None:
        import ctypes
        import msvcrt
        from ctypes import wintypes

        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._peek = self._kernel32.PeekNamedPipe
        self._peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, wintypes.LPDWORD, wintypes.LPDWORD,
                               wintypes.LPDWORD]
        self._peek.restype = wintypes.BOOL
        self._wintypes = wintypes
        self._ctypes = ctypes
        while True:
            try:
                self._file = open(address, "r+b", buffering=0)
                break
            except OSError as error:
                if time.monotonic() >= deadline:
                    raise HandshakeError(f"cannot connect to Wabcut's script pipe {address}: {error}") from error
                time.sleep(0.05)
        self._fd = self._file.fileno()
        self._handle = msvcrt.get_osfhandle(self._fd)

    def _open_unix(self, address: str, deadline: float) -> None:
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        while True:
            try:
                self._sock.connect(address)
                break
            except OSError as error:
                if time.monotonic() >= deadline:
                    self._sock.close()
                    raise HandshakeError(f"cannot connect to Wabcut's script socket {address}: {error}") from error
                time.sleep(0.05)

    def write_line(self, data: bytes) -> None:
        payload = data + b"\n"
        try:
            if self._pipe:
                view = memoryview(payload)
                while view:
                    written = os.write(self._fd, view[:65536])
                    view = view[written:]
            else:
                self._sock.sendall(payload)
        except OSError as error:
            raise ConnectionLostError("the connection to Wabcut is closed") from error

    def _available(self) -> int | None:
        """Bytes waiting on the pipe, or None when the other end has closed it."""
        available = self._wintypes.DWORD(0)
        ok = self._peek(self._handle, None, 0, None, self._ctypes.byref(available), None)
        if not ok:
            return None
        return int(available.value)

    def read_line(self, wait: float) -> "bytes | None | _Eof":
        deadline = time.monotonic() + wait
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                line, self._buffer = self._buffer[:newline], self._buffer[newline + 1:]
                return line.rstrip(b"\r")
            if self._closed:
                return EOF
            if self._pipe:
                available = self._available()
                if available is None:
                    self._closed = True
                    continue
                if available > 0:
                    chunk = os.read(self._fd, min(available, 1 << 20))
                    if not chunk:
                        self._closed = True
                    self._buffer += chunk
                    continue
                if time.monotonic() >= deadline:
                    return None
                time.sleep(0.002)
            else:
                self._sock.settimeout(max(0.001, deadline - time.monotonic()))
                try:
                    chunk = self._sock.recv(1 << 20)
                except socket.timeout:
                    return None
                except OSError:
                    self._closed = True
                    continue
                if not chunk:
                    self._closed = True
                self._buffer += chunk

    def close(self) -> int | None:
        try:
            if self._pipe:
                self._file.close()
            else:
                self._sock.close()
        except OSError:
            pass
        return None


class JsonRpcClient:
    """Requests and answers over a Channel, one at a time (the server handles one request at a time too)."""

    def __init__(self, channel: Channel) -> None:
        self.channel = channel
        self._next_id = 1
        self._lock = threading.RLock()

    def notify(self, method: str, params: dict[str, Any] | None = None) -> None:
        message: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        with self._lock:
            self.channel.write_line(_encode(message))

    def request(self, method: str, params: dict[str, Any] | None = None, timeout: float | None = None) -> Any:
        """The `result` of the request. Raises ProtocolError for a JSON-RPC error, CallTimeoutError after `timeout`
        seconds (the call is cancelled first), ConnectionLostError when the other side went away."""
        with self._lock:
            request_id = self._next_id
            self._next_id += 1
            payload = _encode({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}})
            if len(payload) > MAX_MESSAGE_BYTES:
                raise ProtocolError(
                    f"the request is {len(payload):,} bytes; the limit is {MAX_MESSAGE_BYTES:,}: send it in smaller parts"
                )
            self.channel.write_line(payload)
            deadline = None if timeout is None else time.monotonic() + timeout
            try:
                while True:
                    line = self.channel.read_line(_SLICE)
                    if line is None:
                        if deadline is not None and time.monotonic() >= deadline:
                            self._cancel(request_id)
                            raise CallTimeoutError(f"{method} did not finish within {timeout:g} s and was cancelled")
                        continue
                    if line is EOF:
                        raise ConnectionLostError("Wabcut ended the connection", self.channel.diagnostics())
                    assert isinstance(line, bytes)
                    if not line.strip():
                        continue
                    try:
                        message = json.loads(line)
                    except ValueError as error:
                        raise ProtocolError(f"Wabcut sent something that is not JSON: {line[:80]!r}") from error
                    if not isinstance(message, dict) or message.get("id") != request_id:
                        continue  # a notification (progress) or a late answer to a cancelled call
                    if "error" in message:
                        error = message["error"] or {}
                        raise ProtocolError(str(error.get("message", "error")), error.get("code"), error.get("data"))
                    return message.get("result", {})
            except KeyboardInterrupt:
                self._cancel(request_id)
                raise

    def _cancel(self, request_id: int) -> None:
        try:
            self.channel.write_line(_encode({"jsonrpc": "2.0", "method": "notifications/cancelled",
                                            "params": {"requestId": request_id}}))
        except Exception:  # the connection may be gone already; the caller reports the original problem
            pass


def _encode(message: dict[str, Any]) -> bytes:
    return json.dumps(message, ensure_ascii=True, separators=(",", ":")).encode("ascii")
