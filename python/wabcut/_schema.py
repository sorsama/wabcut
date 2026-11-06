"""The JSON Schema subset the command catalog's parameters are written in, checked before anything is sent.

It is the same subset and the same rules as the server's validator (src/command/src/Schema.cpp): `type` (a name or a
list), `properties`, `required`, `additionalProperties` (false closes an object), `items`, `enum`, `const`, `minimum`,
`maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `minLength`, `maxLength` (counted in UTF-8 bytes, as the server does),
`minItems` and `maxItems`. A boolean is never an integer or a number, and a float with no fraction is an integer. The
server checks again; this only spares the round trip and gives the error before the script has changed anything.
"""

from __future__ import annotations

import math
from typing import Any, NamedTuple


class Issue(NamedTuple):
    path: str  #: JSON Pointer into the value ("/clips/0/track"); "/" for the value itself
    message: str


def _type_of(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, (list, tuple)):
        return "array"
    if isinstance(value, dict):
        return "object"
    return type(value).__name__


def _matches(value: Any, type_name: str) -> bool:
    kind = _type_of(value)
    if type_name == "integer":
        return kind == "integer" or (kind == "number" and math.isfinite(value) and math.floor(value) == value)
    if type_name == "number":
        return kind in ("integer", "number")
    return kind == type_name


def _json(value: Any) -> str:
    import json

    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _escape(key: str) -> str:
    return key.replace("~", "~0").replace("/", "~1")


def validate(schema: Any, value: Any) -> list[Issue]:
    """Every way `value` does not fit `schema` (empty: it fits)."""
    issues: list[Issue] = []
    _check(schema, value, "", issues)
    return issues


def _add(issues: list[Issue], path: str, message: str) -> None:
    issues.append(Issue(path or "/", message))


def _check(schema: Any, value: Any, path: str, issues: list[Issue]) -> None:
    if not isinstance(schema, dict):
        return
    if "type" in schema:
        wanted = schema["type"]
        names = [wanted] if isinstance(wanted, str) else list(wanted)
        if not any(_matches(value, n) for n in names):
            _add(issues, path, f"expected {' or '.join(names)}, got {_type_of(value)}")
            return
    if "enum" in schema and not any(_same(value, option) for option in schema["enum"]):
        _add(issues, path, f"must be one of {_json(schema['enum'])}")
    if "const" in schema and not _same(value, schema["const"]):
        _add(issues, path, f"must be {_json(schema['const'])}")
    kind = _type_of(value)
    if kind in ("integer", "number"):
        for key, bad, text in (
            ("minimum", lambda v, b: v < b, "must be at least"),
            ("maximum", lambda v, b: v > b, "must be at most"),
            ("exclusiveMinimum", lambda v, b: v <= b, "must be greater than"),
            ("exclusiveMaximum", lambda v, b: v >= b, "must be less than"),
        ):
            if key in schema and bad(value, schema[key]):
                _add(issues, path, f"{text} {_json(schema[key])}")
    if kind == "string":
        size = len(value.encode("utf-8", "surrogatepass"))
        if "minLength" in schema and size < schema["minLength"]:
            _add(issues, path, f"must be at least {schema['minLength']} characters long")
        if "maxLength" in schema and size > schema["maxLength"]:
            _add(issues, path, f"must be at most {schema['maxLength']} characters long")
    if kind == "array":
        if "minItems" in schema and len(value) < schema["minItems"]:
            _add(issues, path, f"needs at least {schema['minItems']} items")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            _add(issues, path, f"allows at most {schema['maxItems']} items")
        if "items" in schema:
            for index, item in enumerate(value):
                _check(schema["items"], item, f"{path}/{index}", issues)
    if kind == "object":
        for key in schema.get("required", ()):
            if key not in value:
                _add(issues, path, f'is missing required "{key}"')
        properties = schema.get("properties")
        closed = schema.get("additionalProperties") is False
        for key, item in value.items():
            child = f"{path}/{_escape(str(key))}"
            if properties is not None and key in properties:
                _check(properties[key], item, child, issues)
            elif closed:
                _add(issues, child, "is not a known parameter")


def _same(a: Any, b: Any) -> bool:
    """JSON equality: 1 and 1.0 are the same number, True is not 1."""
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    return a == b
