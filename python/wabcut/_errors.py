"""The exceptions of the wabcut package.

Everything the package raises on purpose is a `WabcutError`. A command the server (or this package, before sending
anything) refused is a `CommandError` with the command's name, the catalog's error code and its details; the class is
the code's own (`PermissionDeniedError`, `InvalidParamsError`, `StaleRevisionError`, ...), so a script can catch what it
expects:

    try:
        session.clip.rename(clip=7, name="Intro")
    except wabcut.NotFoundError:
        ...
"""

from __future__ import annotations

from typing import Any


class WabcutError(Exception):
    """Base class of every error the package raises on purpose."""


class CliNotFoundError(WabcutError):
    """`wabcut-cli` could not be found (set the WABCUT_CLI environment variable or pass `cli=`)."""


class ConnectionLostError(WabcutError):
    """The connection to Wabcut ended while a call was waiting (the process exited or the pipe closed).

    `stderr` holds the last lines Wabcut wrote, which usually say why.
    """

    def __init__(self, message: str, stderr: str = "") -> None:
        super().__init__(message + (f"\n{stderr}" if stderr else ""))
        self.stderr = stderr


class HandshakeError(WabcutError):
    """Wabcut did not accept the connection (a wrong or missing session token, an unsupported version)."""


class ProtocolError(WabcutError):
    """The server answered with something that is not valid JSON-RPC or not what was asked for."""

    def __init__(self, message: str, code: int | None = None, data: Any = None) -> None:
        super().__init__(message)
        self.code = code
        self.data = data


class CallTimeoutError(WabcutError):
    """A call did not finish within the time given for it; it was cancelled (nothing was committed)."""


class NotSupportedError(WabcutError):
    """This Wabcut or this kind of session cannot do what was asked (for example transactions on an old version)."""


class CommandError(WabcutError):
    """A command was refused or failed. Nothing was changed.

    Attributes:
        command: the command's dotted name (`clip.addEffect`), or the tool for session calls (`edit_undo`).
        code: the catalog's error code (`InvalidParams`, `NotFound`, `PermissionDenied`, `StaleRevision`,
            `EditRejected`, `LockedTrack`, `InvariantViolation`, `Cancelled`, `TimedOut`, `Internal`, ...).
        message: what is wrong, in English.
        details: machine-readable context (for `InvalidParams`: `{"issues": [{"path", "message"}]}`).
        client_side: the package refused it before sending anything.
    """

    code = "Internal"

    def __init__(self, command: str, code: str, message: str, details: Any = None, *, client_side: bool = False) -> None:
        super().__init__(f"{command} failed ({code}): {message}")
        self.command = command
        self.code = code
        self.message = message
        self.details = details
        self.client_side = client_side


class UnknownCommandError(CommandError):
    """No command or alias has that name."""


class UnsupportedVersionError(CommandError):
    """The command exists, but not in the version asked for."""


class InvalidRequestError(CommandError):
    """The request is malformed."""


class InvalidParamsError(CommandError):
    """The parameters do not fit the command's schema (`details["issues"]` lists each problem with its path)."""


class NotFoundError(CommandError):
    """A sequence, track, clip, media item or marker the command names does not exist."""


class PermissionDeniedError(CommandError):
    """The session may not run this command (read-only session, analysis or file commands not allowed)."""


class StaleRevisionError(CommandError):
    """`expectedRevision` is not the project's current revision (`details["current"]`)."""


class IdempotencyConflictError(CommandError):
    """An idempotency key was reused for a different request."""


class EditRejectedError(CommandError):
    """The edit itself is not possible (a locked track, nothing to undo, a transaction is open, ...)."""


class InvariantViolationError(CommandError):
    """The result would break a project invariant; nothing was committed."""


class LockedTrackError(CommandError):
    """The edit would change a locked track; nothing was committed."""


class CancelledError(CommandError):
    """The call was cancelled; nothing was committed."""


class TimedOutError(CommandError):
    """The call ran past its time limit; nothing was committed."""


class InternalError(CommandError):
    """Wabcut failed in a way it did not expect."""


_BY_CODE: dict[str, type[CommandError]] = {
    "UnknownCommand": UnknownCommandError,
    "UnsupportedVersion": UnsupportedVersionError,
    "InvalidRequest": InvalidRequestError,
    "InvalidParams": InvalidParamsError,
    "NotFound": NotFoundError,
    "PermissionDenied": PermissionDeniedError,
    "StaleRevision": StaleRevisionError,
    "IdempotencyConflict": IdempotencyConflictError,
    "EditRejected": EditRejectedError,
    "InvariantViolation": InvariantViolationError,
    "LockedTrack": LockedTrackError,
    "Cancelled": CancelledError,
    "TimedOut": TimedOutError,
    "Internal": InternalError,
}


def command_error(command: str, code: str, message: str, details: Any = None, *, client_side: bool = False) -> CommandError:
    """The exception for an error of the catalog (the class of its code; `CommandError` for a code it does not know)."""
    return _BY_CODE.get(code, CommandError)(command, code, message, details, client_side=client_side)
