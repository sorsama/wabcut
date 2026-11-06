# wabcut: script Wabcut from Python

The same typed commands that the Wabcut app, `wabcut-cli`, the AI assistant and the MCP server use, in your own Python
(3.11 or newer, no dependencies).

```python
import wabcut

with wabcut.connect("film.wabcut", edit=True, save="exit") as session:
    print(session.project.summary()["sequences"])
    with session.transaction("Chapters"):                 # one undo step, all or nothing
        session.marker.add(time="00:00:00:00", name="Start", type="chapter")
        session.marker.add(time="00:01:30:00", name="The turn", type="chapter")
```

* `wabcut.connect("film.wabcut")` starts Wabcut's `wabcut-cli` for you: no window needed. Set `WABCUT_CLI` when it is not
  next to this package or on the `PATH`.
* Inside Wabcut, **File > Run Python Script…** runs a script against the open project: `wabcut.connect()` with no
  arguments gives it, with the permissions you allowed, and everything the script does is one undo step.
* `python -m wabcut doctor film.wabcut` checks the installation; `python -m wabcut commands marker` and
  `python -m wabcut describe clip.addEffect` read the command catalog.

The full guide, with the permission model and six example scripts, is `docs/python.md` in the Wabcut repository (and
the `docs` folder of the installed app). The `examples` folder next to this file has the scripts.

`wabcut/catalog.json` and `wabcut/_commands.pyi` are generated from Wabcut's command catalog; do not edit them.
