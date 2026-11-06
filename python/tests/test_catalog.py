"""The generated catalog and stubs are consistent with each other and with the package."""

import keyword
import re
import unittest
from importlib import resources

import wabcut
from wabcut._schema import validate

from support import fake_session


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.catalog = wabcut.load_catalog()

    def test_it_has_the_whole_catalog(self):
        self.assertGreater(len(self.catalog), 400)
        for name in ("clip.addEffect", "timeline.rippleDelete", "marker.add", "project.summary", "export.queue.list"):
            self.assertIn(name, self.catalog.commands)

    def test_every_example_fits_its_own_schema(self):
        for name, info in self.catalog.commands.items():
            if info.example is not None:
                with self.subTest(name):
                    self.assertEqual(validate(info.params, info.example), [])

    def test_names_are_unique_after_their_tool_spelling(self):
        tools = [c.tool_name for c in self.catalog.commands.values()]
        self.assertEqual(len(tools), len(set(tools)))
        aliases = [a for c in self.catalog.commands.values() for a in c.aliases]
        self.assertEqual(len(aliases), len(set(aliases)))
        self.assertFalse(set(aliases) & set(self.catalog.commands))

    def test_find_accepts_names_aliases_and_tool_names(self):
        self.assertIs(self.catalog.find("clip_addEffect"), self.catalog.find("clip.addEffect"))
        self.assertIsNone(self.catalog.find("clip.doesNotExist"))
        some_alias = next((a, c) for c in self.catalog.commands.values() for a in c.aliases)
        self.assertEqual(self.catalog.find(some_alias[0]).name, some_alias[1].name)

    def test_a_close_name_is_suggested(self):
        self.assertEqual(self.catalog.suggest("clip.addEfect")[0], "clip.addEffect")

    def test_edits_and_reads_follow_the_capability(self):
        for info in self.catalog.commands.values():
            self.assertEqual(info.mutates, info.capability in ("EditSequence", "EditProject"), info.name)

    def test_no_group_hides_a_session_method(self):
        session, _ = fake_session()
        own = {n for n in dir(type(session)) if not n.startswith("_")}
        clash = own & set(self.catalog.groups())
        self.assertEqual(clash, set(), "a command group named like a Session method can never be reached")

    def test_the_stub_declares_every_command(self):
        stub = resources.files("wabcut").joinpath("_commands.pyi").read_text(encoding="utf-8")
        self.assertIn("class Commands:", stub)
        # One def or __call__ per command (a command that is also a group is the group's __call__).
        defs = len(re.findall(r"^    def \w+\(", stub, re.M))
        self.assertEqual(defs, len(self.catalog))
        for group in self.catalog.groups():
            self.assertIn(f"    {group}: ", stub, group)

    def test_the_stub_is_valid_python(self):
        stub = resources.files("wabcut").joinpath("_commands.pyi").read_text(encoding="utf-8")
        compile(stub, "_commands.pyi", "exec")

    def test_keyword_parameters_take_a_trailing_underscore_in_the_stub(self):
        stub = resources.files("wabcut").joinpath("_commands.pyi").read_text(encoding="utf-8")
        self.assertRegex(stub, r"\n        from_: ")
        self.assertRegex(stub, r"\n        in_: ")
        for line in stub.splitlines():
            match = re.match(r"^        (\w+): ", line)
            if match:
                self.assertNotIn(match.group(1), set(keyword.kwlist) - {"None", "True", "False"}, line)

    def test_groups_and_commands_are_reachable_as_attributes(self):
        session, _ = fake_session()
        self.assertTrue(callable(session.clip.addEffect))
        self.assertTrue(callable(session.export.queue))  # a command that is also a group
        self.assertTrue(callable(session.export.queue.list))
        self.assertTrue(callable(session.color.grade.set))
        self.assertIn("addEffect", dir(session.clip))
        with self.assertRaises(AttributeError) as caught:
            session.clip.addEfect  # noqa: B018
        self.assertIn("clip.addEffect", str(caught.exception))
        with self.assertRaises(AttributeError):
            session.nonsense  # noqa: B018
        with self.assertRaises(TypeError):
            session.clip()


if __name__ == "__main__":
    unittest.main()
