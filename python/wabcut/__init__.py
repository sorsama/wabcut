"""Wabcut from Python: open a project, run the same typed commands the app, `wabcut-cli` and the AI assistant use.

    import wabcut

    with wabcut.connect("film.wabcut", edit=True, save="exit") as session:
        summary = session.project.summary()                       # reads return their data
        for clip in wabcut.records(session.clip.find(name="intro"), "clips"):  # docs/commands.md lists every command
            session.clip.rename(clips=[clip["id"]], name="Intro")              # edits return a Result
        with session.transaction("Mark the chapters"):             # one undo step, all or nothing
            session.marker.add(time="00:00:00:00", name="Start", type="chapter")

A script started from Wabcut's Scripts menu calls `wabcut.connect()` with no arguments and gets the project that is open,
with the permissions it was given. The package needs only Python 3.11 or newer and Wabcut's `wabcut-cli`; it starts no
network listener, and what a script may do is decided by Wabcut, never by the script. See docs/python.md.
"""

from ._catalog import Catalog, CommandInfo, load_catalog
from ._errors import (
    CallTimeoutError,
    CancelledError,
    CliNotFoundError,
    CommandError,
    ConnectionLostError,
    EditRejectedError,
    HandshakeError,
    IdempotencyConflictError,
    InternalError,
    InvalidParamsError,
    InvalidRequestError,
    InvariantViolationError,
    LockedTrackError,
    NotFoundError,
    NotSupportedError,
    PermissionDeniedError,
    ProtocolError,
    StaleRevisionError,
    TimedOutError,
    UnknownCommandError,
    UnsupportedVersionError,
    WabcutError,
)
from ._result import Result, records
from ._session import Access, Session, connect, find_cli
from ._version import __version__

__all__ = [
    "Access",
    "CallTimeoutError",
    "CancelledError",
    "Catalog",
    "CliNotFoundError",
    "CommandError",
    "CommandInfo",
    "ConnectionLostError",
    "EditRejectedError",
    "HandshakeError",
    "IdempotencyConflictError",
    "InternalError",
    "InvalidParamsError",
    "InvalidRequestError",
    "InvariantViolationError",
    "LockedTrackError",
    "NotFoundError",
    "NotSupportedError",
    "PermissionDeniedError",
    "ProtocolError",
    "Result",
    "Session",
    "StaleRevisionError",
    "TimedOutError",
    "UnknownCommandError",
    "UnsupportedVersionError",
    "WabcutError",
    "__version__",
    "connect",
    "find_cli",
    "load_catalog",
    "records",
]
