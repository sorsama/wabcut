"""The package against the real `wabcut-cli mcp` (set WABCUT_CLI): headless sessions end to end."""

import json
import threading
import unittest
from pathlib import Path

import wabcut

from support import CliCase


def marker_names(session):
    return [m["name"] for m in session.marker.list()["markers"]]


def saved_marker_names(path: Path):
    document = json.loads(path.read_text(encoding="utf-8"))
    return [m.get("name") for m in document["sequences"][0]["markers"]]


class HeadlessSessionTests(CliCase):
    def test_a_session_reads_a_project(self):
        with self.connect(self.project()) as session:
            summary = session.project.summary()
            self.assertEqual(summary["sequences"][0]["name"], "Test sequence")
            self.assertEqual(session.revision, 0)
            self.assertFalse(session.access.edit)
            self.assertTrue(session.server_version)
            self.assertEqual(session.describe("project.summary").capability, "Read")

    def test_a_read_only_session_refuses_edits_and_changes_nothing(self):
        path = self.project()
        before = path.read_bytes()
        with self.connect(path) as session:
            with self.assertRaises(wabcut.PermissionDeniedError) as caught:
                session.marker.add(time="00:00:01:00", name="Nope")
            self.assertTrue(caught.exception.client_side)
            # The server refuses it as well when the package is bypassed.
            with self.assertRaises(wabcut.PermissionDeniedError) as server_side:
                session._tool("marker.add", "marker_add", {"time": "00:00:01:00"})
            self.assertFalse(server_side.exception.client_side)
            self.assertEqual(session.revision, 0)
        self.assertEqual(path.read_bytes(), before)

    def test_edits_are_saved_when_the_session_closes_with_save_exit(self):
        path = self.project()
        with self.connect(path, edit=True, save="exit") as session:
            result = session.marker.add(time="00:00:01:00", name="Intro", type="chapter")
            self.assertTrue(result.committed)
            self.assertEqual(result.revision, 1)
            self.assertEqual(marker_names(session), ["Intro"])
        self.assertEqual(saved_marker_names(path), ["Intro"])

    def test_nothing_is_saved_without_asking(self):
        path = self.project()
        with self.connect(path, edit=True) as session:
            session.marker.add(time="00:00:01:00", name="Unsaved")
        self.assertEqual(saved_marker_names(path), [])

    def test_a_script_that_fails_does_not_save_its_edits(self):
        path = self.project()
        with self.assertRaises(RuntimeError):
            with self.connect(path, edit=True, save="exit") as session:
                session.marker.add(time="00:00:01:00", name="Half done")
                raise RuntimeError("the script crashed")
        self.assertEqual(saved_marker_names(path), [])

    def test_save_writes_the_file_and_a_copy_leaves_it_alone(self):
        path = self.project()
        with self.connect(path, edit=True) as session:
            session.marker.add(time="00:00:01:00", name="Saved")
            copy = session.save("Copy of film.wabcut")
            self.assertEqual(copy.name, "Copy of film.wabcut")
            self.assertEqual(saved_marker_names(path), [])
            self.assertEqual(saved_marker_names(copy), ["Saved"])
            session.save()
        self.assertEqual(saved_marker_names(path), ["Saved"])

    def test_autosave_writes_after_every_change(self):
        path = self.project()
        with self.connect(path, edit=True, save="auto") as session:
            session.marker.add(time="00:00:01:00", name="Auto")
            self.assertEqual(saved_marker_names(path), ["Auto"])


class UndoGroupingTests(CliCase):
    def test_a_transaction_is_one_undo_step(self):
        with self.connect(self.project(), edit=True) as session:
            with session.transaction("Mark the chapters"):
                for i in range(3):
                    session.marker.add(time=f"00:00:0{i}:00", name=f"M{i}")
                self.assertEqual(len(marker_names(session)), 3)
            undone = session.undo()
            self.assertEqual(undone.undone, "Mark the chapters")
            self.assertEqual(marker_names(session), [])
            self.assertFalse(undone.raw["canUndo"])  # the three edits were the only step
            session.redo()
            self.assertEqual(len(marker_names(session)), 3)

    def test_a_batch_is_one_undo_step(self):
        with self.connect(self.project(), edit=True) as session:
            result = session.batch([("marker.add", {"time": f"00:00:0{i}:00", "name": f"B{i}"}) for i in range(3)], label="Three")
            self.assertEqual(len(result.steps), 3)
            self.assertEqual(session.undo().undone, "Three")
            self.assertEqual(marker_names(session), [])
            self.assertFalse(session.redo().raw["canRedo"])

    def test_a_script_that_fails_halfway_leaves_the_project_as_it_was(self):
        with self.connect(self.project(), edit=True) as session:
            session.marker.add(time="00:00:09:00", name="Before")
            revision = session.revision
            with self.assertRaises(ZeroDivisionError):
                with session.transaction("Half"):
                    session.marker.add(time="00:00:01:00", name="One")
                    session.marker.add(time="00:00:02:00", name="Two")
                    1 / 0
            self.assertEqual(marker_names(session), ["Before"])
            self.assertEqual(session.undo().undone, "Add Marker")  # "Before" is the only step left
            self.assertGreater(session.revision, revision)  # a rollback is a change of revision, not a rewind

    def test_a_batch_with_a_failing_command_changes_nothing(self):
        with self.connect(self.project(), edit=True) as session:
            before = session.revision
            with self.assertRaises(wabcut.NotFoundError):
                session.batch([("marker.add", {"time": "00:00:01:00", "name": "First"}),
                               ("clip.rename", {"clips": [12345], "name": "x"})])
            self.assertEqual(marker_names(session), [])
            self.assertEqual(session.revision, before)

    def test_undo_and_save_wait_for_the_transaction_to_end(self):
        with self.connect(self.project(), edit=True) as session:
            with session.transaction():
                session.marker.add(time="00:00:01:00", name="X")
                with self.assertRaises(wabcut.EditRejectedError):
                    session.undo()
                with self.assertRaises(wabcut.EditRejectedError):
                    session.save()
                with self.assertRaises(wabcut.EditRejectedError):
                    with session.transaction("nested"):
                        pass
            self.assertEqual(marker_names(session), ["X"])

    def test_a_connection_that_drops_in_a_transaction_rolls_it_back(self):
        path = self.project()
        with self.connect(path, edit=True, save="exit") as session:
            session.marker.add(time="00:00:09:00", name="Kept")
            session._tool("edit_transaction", "edit_transaction", {"action": "begin", "label": "Interrupted"})
            session.marker.add(time="00:00:01:00", name="Lost")
            # The script ends here without committing: closing the connection must discard the transaction, then save.
        self.assertEqual(saved_marker_names(path), ["Kept"])


class RefusalTests(CliCase):
    def test_an_invalid_parameter_is_refused_and_nothing_changes(self):
        with self.connect(self.project(), edit=True) as session:
            with self.assertRaises(wabcut.InvalidParamsError) as caught:
                session.marker.add(time="00:00:01:00", type="not-a-type")
            self.assertTrue(caught.exception.client_side)
            self.assertEqual(session.revision, 0)
            # Past the package's own check the server refuses the same call with the same code.
            with self.assertRaises(wabcut.InvalidParamsError) as server_side:
                session.command("marker.add", {"time": "00:00:01:00", "type": "not-a-type"}, validate_params=False)
            self.assertFalse(server_side.exception.client_side)
            self.assertEqual(marker_names(session), [])

    def test_a_missing_clip_is_not_found(self):
        with self.connect(self.project(), edit=True) as session:
            with self.assertRaises(wabcut.NotFoundError) as caught:
                session.clip.rename(clips=[4242], name="x")
            self.assertEqual(caught.exception.code, "NotFound")

    def test_a_stale_revision_is_refused(self):
        with self.connect(self.project(), edit=True) as session:
            session.marker.add(time="00:00:01:00", name="One")
            with self.assertRaises(wabcut.StaleRevisionError) as caught:
                session.marker.add(time="00:00:02:00", name="Two", expectedRevision=0)
            self.assertEqual(caught.exception.details["current"], 1)
            self.assertEqual(marker_names(session), ["One"])

    def test_a_dry_run_previews_without_changing(self):
        with self.connect(self.project(), edit=True) as session:
            preview = session.marker.add(time="00:00:01:00", name="Maybe", dryRun=True)
            self.assertTrue(preview.dry_run)
            self.assertFalse(preview.committed)
            self.assertEqual(marker_names(session), [])
            self.assertEqual(session.events(), [])

    def test_an_idempotency_key_makes_a_retry_safe(self):
        with self.connect(self.project(), edit=True) as session:
            first = session.marker.add(time="00:00:01:00", name="Once", idempotencyKey="k1")
            second = session.marker.add(time="00:00:01:00", name="Once", idempotencyKey="k1")
            self.assertTrue(second.raw.get("replayed"))
            self.assertEqual(second.revision, first.revision)
            self.assertEqual(marker_names(session), ["Once"])

    def test_analysis_and_file_commands_are_not_served_unless_asked_for(self):
        with self.connect(self.project(), edit=True) as session:
            with self.assertRaises(wabcut.PermissionDeniedError):
                session.audio.loudness()
            with self.assertRaises(wabcut.PermissionDeniedError) as server_side:
                session._tool("audio.loudness", "audio_loudness", {})
            self.assertFalse(server_side.exception.client_side)


class ConcurrencyAndAuditTests(CliCase):
    def test_two_sessions_on_one_project_are_independent(self):
        path = self.project()
        with self.connect(path, edit=True) as first, self.connect(path, edit=True) as second:
            first.marker.add(time="00:00:01:00", name="From first")
            second.marker.add(time="00:00:02:00", name="From second")
            self.assertEqual(marker_names(first), ["From first"])
            self.assertEqual(marker_names(second), ["From second"])
            self.assertEqual((first.revision, second.revision), (1, 1))

    def test_threads_may_share_a_session(self):
        with self.connect(self.project(), edit=True) as session:
            errors = []

            def worker(n):
                try:
                    for i in range(5):
                        session.marker.add(time=f"00:00:{n:02d}:{i:02d}", name=f"T{n}-{i}")
                except Exception as error:  # noqa: BLE001
                    errors.append(error)

            threads = [threading.Thread(target=worker, args=(n,)) for n in range(4)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(errors, [])
            self.assertEqual(len(marker_names(session)), 20)

    def test_the_audit_log_names_each_call_and_never_its_parameters(self):
        log = self.folder / "audit.jsonl"
        with self.connect(self.project(), edit=True, audit_log=log) as session:
            session.project.summary()
            session.marker.add(time="00:00:01:00", name="SecretMarkerName")
            with self.assertRaises(wabcut.NotFoundError):
                session.clip.rename(clips=[4242], name="x")
        lines = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual([entry["tool"] for entry in lines], ["project_summary", "marker_add", "clip_rename"])
        self.assertEqual([entry["ok"] for entry in lines], [True, True, False])
        self.assertEqual(lines[2]["code"], "NotFound")
        self.assertTrue(lines[1]["committed"])
        self.assertNotIn("SecretMarkerName", log.read_text(encoding="utf-8"))
        self.assertTrue(all("time" in entry for entry in lines))


class SequenceFromDataTests(CliCase):
    """What the examples in docs/python.md do."""

    def test_a_marker_for_each_row_of_a_table_in_one_undo_step(self):
        rows = [("00:00:00:00", "Intro"), ("00:00:10:00", "Setup"), ("00:00:20:00", "Payoff")]
        with self.connect(self.project(), edit=True) as session:
            with session.transaction("Chapters from a table"):
                for time, name in rows:
                    session.marker.add(time=time, name=name, type="chapter")
            self.assertEqual(marker_names(session), [name for _, name in rows])
            chapters = session.sequence.exportMarkers(format="youtube")
            self.assertIn("Setup", chapters["text"])

    def test_titles_can_be_found_and_renamed(self):
        with self.connect(self.project(), edit=True) as session:
            session.title.add(at="00:00:00:00", text="Hello")
            clips = wabcut.records(session.clip.find(name="Hel"), "clips")
            self.assertEqual([c["name"] for c in clips], ["Hello"])
            session.clip.rename(clips=[c["id"] for c in clips], name="Welcome")
            self.assertEqual([c["name"] for c in wabcut.records(session.clip.find(name="Welcome"), "clips")], ["Welcome"])


class RenderTests(CliCase):
    def test_render_to_renders_the_edits_made_so_far(self):
        path = self.project()
        output = self.folder / "out.mp4"
        with self.connect(path, edit=True) as session:
            session.title.add(at="00:00:00:00", text="Rendered", duration="00:00:01:00")
            try:
                result = session.render_to(output, timeout=300)
            except wabcut.WabcutError as error:
                self.skipTest(f"this wabcut-cli cannot render here: {error}")
            self.assertEqual(result, output.resolve())
            self.assertGreater(output.stat().st_size, 1000)
            self.assertEqual(saved_marker_names(path), [])  # the project file was not touched
            self.assertEqual([p.name for p in self.folder.glob("wabcut-render-*")], [])  # the temporary copy is gone


if __name__ == "__main__":
    unittest.main()
