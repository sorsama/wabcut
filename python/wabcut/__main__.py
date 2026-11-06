"""`python -m wabcut`: find out what is installed and read the command catalog from a terminal.

    python -m wabcut doctor [film.wabcut]   which wabcut-cli the package found, its version, and (with a project) a test call
    python -m wabcut commands [words]       the commands, optionally those matching all the words
    python -m wabcut describe clip.addEffect   one command's parameters and description
"""

from __future__ import annotations

import sys

from . import __version__, connect, find_cli, load_catalog
from ._errors import WabcutError


def _doctor(args: list[str]) -> int:
    print(f"wabcut package {__version__}, Python {sys.version.split()[0]}")
    catalog = load_catalog()
    print(f"command catalog: {len(catalog)} commands in {len(catalog.groups())} groups")
    try:
        cli = find_cli()
    except WabcutError as error:
        print(f"wabcut-cli: NOT FOUND\n  {error}")
        return 1
    print(f"wabcut-cli: {cli}")
    if not args:
        print("pass a project file to open it and run a test call: python -m wabcut doctor film.wabcut")
        return 0
    try:
        with connect(args[0]) as session:
            summary = session.project.summary()
            print(f"opened {args[0]}: Wabcut {session.server_version or '?'}, {len(summary['sequences'])} sequences, "
                  f"{summary['media']} media items")
    except WabcutError as error:
        print(f"could not open {args[0]}: {error}")
        return 1
    return 0


def _commands(args: list[str]) -> int:
    catalog = load_catalog()
    found = catalog.search(" ".join(args)) if args else [c for _, c in sorted(catalog.commands.items())]
    for info in found:
        print(f"{info.name:<36} {info.capability:<14} {info.title}")
    print(f"{len(found)} commands", file=sys.stderr)
    return 0 if found else 1


def _describe(args: list[str]) -> int:
    if len(args) != 1:
        print("usage: python -m wabcut describe <command>", file=sys.stderr)
        return 2
    info = load_catalog().find(args[0])
    if info is None:
        hint = load_catalog().suggest(args[0])
        print(f"no command '{args[0]}'" + (f"; did you mean {', '.join(hint)}?" if hint else ""), file=sys.stderr)
        return 1
    print(info.help())
    return 0


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0 if args else 2
    if args[0] == "--version":
        print(__version__)
        return 0
    handlers = {"doctor": _doctor, "commands": _commands, "describe": _describe}
    handler = handlers.get(args[0])
    if handler is None:
        print(f"unknown command '{args[0]}'\n\n{__doc__.strip()}", file=sys.stderr)
        return 2
    return handler(args[1:])


if __name__ == "__main__":
    raise SystemExit(main())
