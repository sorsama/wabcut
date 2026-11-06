"""`Commands`: the base class a session inherits for `session.<group>.<command>(...)`.

At run time it is the generic implementation in `_catalog.py` (commands are looked up in catalog.json). Editors and type
checkers read `_commands.pyi` instead, which is generated from the same catalog and gives every command its typed
signature and documentation.
"""

from ._catalog import Commands

__all__ = ["Commands"]
