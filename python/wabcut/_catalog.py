"""The command catalog as Python: `session.clip.addEffect(...)`.

`catalog.json` (generated from the same registry the app, `wabcut-cli` and the MCP server use; docs/commands.md) lists
every command with its parameter schema. `Commands` is what a session inherits: `session.<group>.<command>(**params)`
looks the command up there, checks the parameters against its schema, sends it and returns the answer. The typed
signatures an editor shows come from `_commands.pyi`, generated from the same catalog.

Spelling: a command and its parameters are spelled as in the catalog (`timeline.rippleDelete`, `captionTrack`). A
parameter that is a Python keyword takes a trailing underscore (`from_`, `in_`); a name that cannot be a keyword at all
goes in the dict form `session.media.newSynthetic({"params": {...}})`.
"""

from __future__ import annotations

import difflib
import json
import keyword
from dataclasses import dataclass, field
from functools import lru_cache
from importlib import resources
from typing import TYPE_CHECKING, Any, Iterable

if TYPE_CHECKING:
    from ._result import Result

ENVELOPE = ("dryRun", "expectedRevision", "idempotencyKey")


@dataclass(frozen=True)
class CommandInfo:
    """One command of the catalog."""

    name: str  #: dotted name, "clip.addEffect"
    version: int
    title: str
    capability: str  #: Read, EditSequence, EditProject or MediaAnalysis
    mutates: bool  #: changes the project (EditSequence / EditProject)
    description: str
    params: dict[str, Any]  #: JSON Schema of the parameters object
    aliases: tuple[str, ...] = ()
    runs_model: bool = False  #: can run an AI model (needs MediaAnalysis as well)
    touches_files: bool = False  #: reads or writes files its parameters name
    file_params: tuple[str, ...] = ()
    example: Any = None
    _extra: dict[str, Any] = field(default_factory=dict, repr=False, compare=False)

    @property
    def tool_name(self) -> str:
        """The name on the wire (MCP tool): the dotted name with `_` for each dot."""
        return self.name.replace(".", "_")

    @property
    def required(self) -> tuple[str, ...]:
        return tuple(self.params.get("required", ()))

    def help(self) -> str:
        """The text `help(session.clip.addEffect)` and `python -m wabcut describe` show."""
        lines = [f"{self.name}  -  {self.title}", "", self.description.strip(), ""]
        needs = f"needs {self.capability}"
        if self.runs_model:
            needs += "; runs an AI model, so MediaAnalysis is needed too"
        if self.touches_files:
            needs += "; reads or writes files its parameters name"
        lines.append(f"Command {self.name} (v{self.version}); {needs}.")
        if self.aliases:
            lines.append("Also known as: " + ", ".join(self.aliases))
        properties: dict[str, Any] = self.params.get("properties", {})
        if properties:
            lines += ["", "Parameters:"]
            required = set(self.required)
            for key, prop in properties.items():
                kind = _type_text(prop)
                lines.append(f"  {_py_name(key)}  ({kind}){'  [required]' if key in required else ''}")
                text = prop.get("description", "") if isinstance(prop, dict) else ""
                if text:
                    lines.append("      " + text.replace("\n", "\n      "))
        if self.mutates:
            lines += ["", "Also: dryRun=True previews it, expectedRevision=N fails unless the project is at revision N,",
                      "idempotencyKey='k' makes a retry return the first result."]
        return "\n".join(lines)


def _type_text(prop: Any) -> str:
    if not isinstance(prop, dict):
        return "any"
    if "enum" in prop:
        return " | ".join(json.dumps(v) for v in prop["enum"])
    kind = prop.get("type", "any")
    return " | ".join(kind) if isinstance(kind, list) else str(kind)


def _py_name(key: str) -> str:
    return key + "_" if keyword.iskeyword(key) or key == "self" else key


class Catalog:
    """The commands of `catalog.json`."""

    def __init__(self, commands: Iterable[CommandInfo]) -> None:
        self.commands: dict[str, CommandInfo] = {c.name: c for c in commands}
        self._by_alias: dict[str, str] = {}
        for c in self.commands.values():
            for alias in c.aliases:
                self._by_alias[alias] = c.name
        self._by_tool = {c.tool_name: c.name for c in self.commands.values()}

    def __len__(self) -> int:
        return len(self.commands)

    def find(self, name: str) -> CommandInfo | None:
        """A command by dotted name, alias or tool name (`clip_addEffect`)."""
        if name in self.commands:
            return self.commands[name]
        canonical = self._by_alias.get(name) or self._by_tool.get(name)
        return self.commands.get(canonical) if canonical else None

    def suggest(self, name: str, count: int = 3) -> list[str]:
        pool = list(self.commands) + list(self._by_alias)
        return difflib.get_close_matches(name, pool, n=count, cutoff=0.6)

    def groups(self) -> list[str]:
        """The first segments of the command names (`clip`, `sequence`, ...)."""
        return sorted({n.split(".", 1)[0] for n in self.commands})

    def under(self, prefix: str) -> list[CommandInfo]:
        """Commands whose name is `prefix` or starts with `prefix.`."""
        dotted = prefix + "."
        return [c for n, c in sorted(self.commands.items()) if n == prefix or n.startswith(dotted)]

    def search(self, text: str) -> list[CommandInfo]:
        """Commands whose name, title or description contains every word of `text` (case-insensitive)."""
        words = text.lower().split()
        found = []
        for name, info in sorted(self.commands.items()):
            haystack = f"{name} {info.title} {info.description}".lower()
            if all(w in haystack for w in words):
                found.append(info)
        return found


@lru_cache(maxsize=1)
def load_catalog() -> Catalog:
    """The catalog shipped with this package."""
    text = resources.files(__package__).joinpath("catalog.json").read_text(encoding="utf-8")
    document = json.loads(text)
    if document.get("format") != 1:
        raise RuntimeError("wabcut/catalog.json has a format this version of the package does not read")
    commands = []
    for entry in document["commands"]:
        commands.append(
            CommandInfo(
                name=entry["name"],
                version=int(entry.get("version", 1)),
                title=entry.get("title", ""),
                capability=entry.get("capability", "Read"),
                mutates=bool(entry.get("mutates", False)),
                description=entry.get("description", ""),
                params=entry.get("params", {}),
                aliases=tuple(entry.get("aliases", ())),
                runs_model=bool(entry.get("runsModel", False)),
                touches_files=bool(entry.get("touchesFiles", False)),
                file_params=tuple(entry.get("fileParams", ())),
                example=entry.get("example"),
            )
        )
    return Catalog(commands)


class _Node:
    """A group of commands (`session.clip`) or one command (`session.clip.addEffect`): a command that is also the
    start of longer names (`export.queue` and `export.queue.list`) is both."""

    __slots__ = ("_session", "_path", "_info", "_catalog")

    def __init__(self, session: "Commands", path: str) -> None:
        self._session = session
        self._path = path
        self._catalog = load_catalog()
        self._info = self._catalog.commands.get(path)

    def __repr__(self) -> str:
        if self._info and not self._children():
            return f"<wabcut command {self._path}>"
        return f"<wabcut commands {self._path}.*: {', '.join(self._children()) or 'none'}>"

    def _children(self) -> list[str]:
        prefix = self._path + "."
        names = {n[len(prefix):].split(".", 1)[0] for n in self._catalog.commands if n.startswith(prefix)}
        return sorted(names)

    def __dir__(self) -> list[str]:
        return self._children() + ["__call__"]

    def __getattr__(self, name: str) -> Any:
        if name.startswith("_"):
            raise AttributeError(name)
        wire = name[:-1] if name.endswith("_") and keyword.iskeyword(name[:-1]) else name
        path = f"{self._path}.{wire}"
        if path in self._catalog.commands or any(n.startswith(path + ".") for n in self._catalog.commands):
            return _Node(self._session, path)
        hint = self._catalog.suggest(path)
        more = f" Did you mean {', '.join(hint)}?" if hint else ""
        raise AttributeError(f"there is no command or group '{path}' in this version of Wabcut.{more}")

    def __call__(self, _params: dict[str, Any] | None = None, /, **kwargs: Any) -> Any:
        if self._info is None:
            raise TypeError(f"'{self._path}' is a group of commands, not a command: {', '.join(self._children())}")
        return self._session._call_command(self._info, _params, kwargs)


class Commands:
    """What a session inherits: every command group as an attribute."""

    def __getattr__(self, name: str) -> Any:
        if name.startswith("_"):
            raise AttributeError(name)
        catalog = load_catalog()
        wire = name[:-1] if name.endswith("_") and keyword.iskeyword(name[:-1]) else name
        if wire in catalog.groups() or wire in catalog.commands:
            return _Node(self, wire)
        hint = catalog.suggest(wire)
        more = f" Did you mean {', '.join(hint)}?" if hint else ""
        raise AttributeError(f"'{type(self).__name__}' has no command group '{name}'.{more}")

    def __dir__(self) -> list[str]:
        return sorted(set(super().__dir__()) | set(load_catalog().groups()))

    def _call_command(self, info: CommandInfo, params: dict[str, Any] | None, keywords: dict[str, Any]) -> "Result | Any":
        raise NotImplementedError


def normalize_params(info: CommandInfo, params: dict[str, Any] | None, keywords: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    """Splits what a caller passed into the command's parameters and the envelope (dryRun, ...).

    The dict form and the keywords are merged (a keyword wins); a keyword `from_` means the parameter `from`; keywords
    whose value is None are left out unless the schema allows null there.
    """
    properties: dict[str, Any] = info.params.get("properties", {})
    merged: dict[str, Any] = dict(params or {})
    envelope: dict[str, Any] = {}
    for key, value in keywords.items():
        if key in properties:
            wire = key
        elif key.endswith("_") and key[:-1] in properties:
            wire = key[:-1]
        elif key in ENVELOPE and info.mutates:
            if value is not None:
                envelope[key] = value
            continue
        else:
            wire = key  # unknown: the validator reports it with its path
        if value is None and not _allows_null(properties.get(wire)):
            merged.pop(wire, None)
            continue
        merged[wire] = value
    return merged, envelope


def _allows_null(prop: Any) -> bool:
    if not isinstance(prop, dict):
        return False
    kind = prop.get("type")
    return kind == "null" or (isinstance(kind, list) and "null" in kind)
