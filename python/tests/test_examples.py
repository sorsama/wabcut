"""The example scripts of docs/python.md run, against the real wabcut-cli, and do what their headings say."""

import ast
import csv
import importlib.util
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

import wabcut

from support import CliCase

EXAMPLES = Path(__file__).resolve().parent.parent / "examples"


def run(script, *args, check=True):
    """The example as a script, the way a user runs it (`python script.py ...`)."""
    environment = dict(os.environ)
    environment["PYTHONPATH"] = str(EXAMPLES.parent) + os.pathsep + environment.get("PYTHONPATH", "")
    environment["PYTHONIOENCODING"] = "utf-8"
    done = subprocess.run([sys.executable, "-X", "utf8", str(EXAMPLES / script), *map(str, args)], capture_output=True,
                          text=True, encoding="utf-8", env=environment, timeout=300, check=False)
    if check and done.returncode != 0:
        raise AssertionError(f"{script} exited with {done.returncode}:\n{done.stdout}\n{done.stderr}")
    return done


class ExampleSourceTests(unittest.TestCase):
    def test_every_example_is_valid_python_with_a_header(self):
        scripts = sorted(EXAMPLES.glob("*.py"))
        self.assertGreaterEqual(len(scripts), 6)
        for script in scripts:
            with self.subTest(script.name):
                text = script.read_text(encoding="utf-8")
                ast.parse(text)
                self.assertIn("# wabcut-name:", text)
                self.assertIn("# wabcut-permissions:", text)

    def test_the_headers_ask_for_known_permissions(self):
        known = {"Read", "EditSequence", "EditProject", "MediaAnalysis", "Files"}
        for script in sorted(EXAMPLES.glob("*.py")):
            line = next(l for l in script.read_text(encoding="utf-8").splitlines() if l.startswith("# wabcut-permissions:"))
            asked = {p.strip() for p in line.split(":", 1)[1].split(",")}
            with self.subTest(script.name):
                self.assertTrue(asked <= known, asked - known)
                self.assertIn("Read", asked)


class ExampleRunTests(CliCase):
    def setUp(self):
        super().setUp()
        self.path = self.project()
        # A project with media: synthetic bars and tone (no files needed) and a title clip.
        with self.connect(self.path, edit=True, save="exit") as session:
            session.media.newSynthetic(kind="barsAndTone", name="Bars")
            session.title.add(at="00:00:00:00", text="GX010023 intro", duration="00:00:05:00")

    def markers(self, sequence=None):
        with self.connect(self.path, sequence=sequence) as session:
            return session.marker.list()["markers"]

    def test_markers_from_csv(self):
        table = self.folder / "markers.csv"
        table.write_text("time,name,type\n00:00:00:00,Intro,chapter\n00:01:30:00,The turn,chapter\n00:04:12:10,Ace,comment\n",
                         encoding="utf-8")
        run("markers_from_csv.py", self.path, table)
        found = self.markers()
        self.assertEqual([(m["name"], m["type"]) for m in found], [("Intro", "chapter"), ("The turn", "chapter"), ("Ace", "comment")])

    def test_the_sample_tables_that_ship_with_the_examples_load(self):
        for name, columns in (("markers.csv", {"time", "name", "type"}), ("shotlist.csv", {"media", "in", "out", "title"}),
                              ("renames.csv", {"old", "new"})):
            with self.subTest(name), open(EXAMPLES / name, newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
                self.assertTrue(rows)
                self.assertEqual(set(rows[0]), columns)

    def test_rename_from_csv_is_one_undo_step(self):
        table = self.folder / "renames.csv"
        table.write_text("old,new\nGX010023,Opening drive\nNOPE,Never\n", encoding="utf-8")
        done = run("rename_from_csv.py", self.path, table)
        self.assertIn("No clip is named like 'NOPE'", done.stdout)
        with self.connect(self.path) as session:
            names = [c["name"] for c in wabcut.records(session.clip.find(name="Opening"), "clips")]
            self.assertEqual(names, ["Opening drive"])

    def test_apply_grade_grades_every_clip_of_the_media(self):
        done = run("apply_grade.py", self.path, "GX010023 intro")
        self.assertIn("Graded 1 clip(s)", done.stdout)
        with self.connect(self.path) as session:
            clip = wabcut.records(session.clip.find(name="GX010023"), "clips")[0]
            effects = session.clip.details(clip=clip["id"])
            self.assertIn("wabcut.color.grade", json.dumps(effects))

    def test_sequence_from_csv_builds_a_sequence_with_chapters(self):
        shots = self.folder / "shotlist.csv"
        shots.write_text("media,in,out,title\nBars,00:00:01:00,00:00:03:00,First\nBars,00:00:05:00,00:00:06:00,Second\n",
                         encoding="utf-8")
        run("sequence_from_csv.py", self.path, shots)
        with self.connect(self.path) as session:
            sequences = session.project.summary()["sequences"]
            self.assertEqual([s["name"] for s in sequences], ["Test sequence", "Rough cut"])
            cut = sequences[1]
            self.assertEqual(cut["clips"], 4)  # two shots, picture and sound each
            self.assertEqual([m["name"] for m in session.marker.list(sequence=cut["id"])["markers"]], ["First", "Second"])

    def test_export_markers_writes_chapters_and_a_table(self):
        table = self.folder / "markers.csv"
        table.write_text("time,name,type\n00:00:00:00,Intro,chapter\n00:01:00:00,Middle,chapter\n00:02:00:00,End,chapter\n",
                         encoding="utf-8")
        run("markers_from_csv.py", self.path, table)
        out = self.folder / "out"
        out.mkdir()
        before = self.path.read_bytes()
        run("export_markers.py", self.path, out)
        self.assertEqual((out / "youtube-chapters.txt").read_text(encoding="utf-8").splitlines(),
                         ["0:00 Intro", "1:00 Middle", "2:00 End"])
        self.assertIn("Intro", (out / "markers.csv").read_text(encoding="utf-8"))
        self.assertEqual(self.path.read_bytes(), before)  # read-only: the project file is untouched

    def test_a_project_with_several_sequences_asks_which(self):
        with self.connect(self.path, edit=True, save="exit") as session:
            session.sequence.create(preset="1080p60", name="Second")
        table = self.folder / "markers.csv"
        table.write_text("time,name,type\n00:00:00:00,Intro,chapter\n", encoding="utf-8")
        done = run("markers_from_csv.py", self.path, table, check=False)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("sequence", done.stderr)
        run("markers_from_csv.py", self.path, table, "Test sequence")  # naming it works
        self.assertEqual([m["name"] for m in self.markers("Test sequence")], ["Intro"])

    def test_loudness_qc_reports_a_failure_with_exit_code_1_or_a_clear_error(self):
        # The project has no audio to measure: either the measurement fails clearly or the programme is off target.
        done = run("loudness_qc.py", self.path, check=False)
        self.assertNotEqual(done.returncode, 0)
        self.assertTrue(done.stderr or done.stdout)


if __name__ == "__main__":
    unittest.main()
