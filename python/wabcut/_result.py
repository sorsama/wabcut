"""What an edit returns."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


def records(data: dict[str, Any], key: str) -> list[dict[str, Any]]:
    """The rows of a read that answers in the compact "fields + rows" form, as dicts.

    Many reads (`clip.find`, `media.list`, `sequence.beats`, ...) return `{"clipFields": ["id", "track", ...],
    "clips": [[6, "V1", ...], ...]}` to keep long answers small. `records(data, "clips")` turns the rows into
    `[{"id": 6, "track": "V1", ...}, ...]`. A row shorter than the field list leaves the missing fields out.
    """
    rows = data.get(key)
    if rows is None:
        raise KeyError(f"the answer has no '{key}'")
    for fields_key in (key[:-1] + "Fields" if key.endswith("s") else "", key + "Fields"):
        if fields_key and fields_key in data:
            fields = data[fields_key]
            return [dict(zip(fields, row)) for row in rows]
    raise KeyError(f"the answer has no field list for '{key}' (expected '{key[:-1]}Fields' or '{key}Fields')")


@dataclass
class Result:
    """The outcome of an edit (a command that changes the project, a batch, undo or redo).

    Reads (`session.sequence.summary()`) return the command's data directly; edits return this.
    """

    ok: bool = True
    revision: int = 0  #: the project revision after the call; pass it as `expectedRevision` next time
    based_on_revision: int | None = None  #: the revision the edit was applied to
    committed: bool = False  #: the project changed (one undo step, or part of the open transaction)
    dry_run: bool = False  #: a preview: nothing was changed
    created_ids: list[int] = field(default_factory=list)  #: ids the edit created (clips, tracks, markers, ...)
    changes: dict[str, Any] = field(default_factory=dict)  #: what changed, per sequence: tracks and time ranges
    layout: list[Any] = field(default_factory=list)  #: the resulting clips of the changed tracks
    warnings: list[str] = field(default_factory=list)
    data: Any = None  #: the command's own answer, when it has one
    steps: list[dict[str, Any]] = field(default_factory=list)  #: batches: one entry per command
    undone: str | None = None  #: undo: the step that was undone
    redone: str | None = None  #: redo: the step that was redone
    raw: dict[str, Any] = field(default_factory=dict, repr=False)  #: the server's whole answer

    @classmethod
    def from_structured(cls, structured: dict[str, Any]) -> "Result":
        return cls(
            ok=bool(structured.get("ok", True)),
            revision=int(structured.get("revision", 0)),
            based_on_revision=structured.get("basedOnRevision"),
            committed=bool(structured.get("committed", False)),
            dry_run=bool(structured.get("dryRun", False)),
            created_ids=list(structured.get("createdIds", [])),
            changes=dict(structured.get("changes", {}) or {}),
            layout=list(structured.get("layout", []) or []),
            warnings=list(structured.get("warnings", []) or []),
            data=structured.get("data"),
            steps=list(structured.get("steps", []) or []),
            undone=structured.get("undone"),
            redone=structured.get("redone"),
            raw=structured,
        )

    @property
    def created_id(self) -> int | None:
        """The first id the edit created, or None."""
        return self.created_ids[0] if self.created_ids else None
