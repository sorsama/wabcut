"""A script run from Wabcut reaches the editor over a named pipe / Unix socket with a one-time token: the package's side of
that connection, against a stand-in editor (the real one is exercised by the app tests)."""

import os
import unittest
from unittest import mock

import wabcut

from editor_server import TOKEN, EditorServer
from support import structured


def summary_tool(arguments):
    return structured({"revision": 0, "data": {"sequences": [{"id": 7, "name": "Main"}], "media": 0}})


def marker_tool(arguments):
    return structured({"revision": 1, "committed": True, "dryRun": False, "createdIds": []})


class EditorChannelTests(unittest.TestCase):
    def connect(self, server, token=TOKEN, **options):
        with mock.patch.dict(os.environ, server.environment(token)):
            return wabcut.connect(**options)

    def test_a_script_gets_the_open_project_with_what_it_was_allowed(self):
        with EditorServer({"project_summary": summary_tool, "marker_add": marker_tool}) as server:
            with self.connect(server) as session:
                self.assertEqual(session.origin, "app")
                self.assertTrue(session.access.edit)
                self.assertFalse(session.access.analysis)
                self.assertFalse(session.access.files)
                self.assertFalse(session.access.save)
                self.assertEqual(session.server_version, "0.7-test")
                self.assertEqual(session.project.summary()["sequences"][0]["name"], "Main")
                self.assertTrue(session.marker.add(time="00:00:01:00").committed)
        # The sequence open in the timeline is what commands work on when a call names none.
        self.assertEqual(session.sequence_id, 7)
        self.assertEqual(server.calls("marker_add")[0]["arguments"], {"time": "00:00:01:00", "sequence": 7})
        self.assertEqual(server.calls("project_summary")[0]["arguments"], {})  # a command without a sequence parameter

    def test_a_call_that_names_a_sequence_keeps_it(self):
        with EditorServer({"marker_add": marker_tool}) as server:
            with self.connect(server) as session:
                session.marker.add(time=1, sequence=9)
        self.assertEqual(server.calls("marker_add")[0]["arguments"]["sequence"], 9)

    def test_a_read_only_grant_refuses_edits_before_sending_anything(self):
        hello = {"ok": True, "edit": False, "analysis": False, "files": False, "version": "x"}
        with EditorServer({"marker_add": marker_tool}, hello=hello) as server:
            with self.connect(server) as session:
                with self.assertRaises(wabcut.PermissionDeniedError) as caught:
                    session.marker.add(time=1)
                self.assertIn("not allowed to edit", str(caught.exception))
        self.assertEqual(server.calls("marker_add"), [])

    def test_a_wrong_token_is_refused(self):
        with EditorServer() as server:
            with self.assertRaises(wabcut.HandshakeError) as caught:
                self.connect(server, token="not-the-token")
            self.assertIn("token", str(caught.exception))
        self.assertTrue(server.rejected)

    def test_the_editor_going_away_is_reported(self):
        with EditorServer({"project_summary": summary_tool}) as server:
            server.drop_after_requests = 1
            with self.connect(server) as session:
                session.project.summary()
                with self.assertRaises(wabcut.ConnectionLostError):
                    session.project.summary()

    def test_a_session_that_ends_with_an_error_tells_the_editor(self):
        with EditorServer({"project_summary": summary_tool}) as server:
            with self.assertRaises(RuntimeError):
                with self.connect(server) as session:
                    session.project.summary()
                    raise RuntimeError("the script failed")
        self.assertIn("notifications/wabcut-abort", [m.get("method") for m in server.lines])

    def test_big_answers_and_requests_pass_whole(self):
        blob = "x" * 600_000
        with EditorServer({"project_summary": lambda a: structured({"revision": 0, "data": {"blob": blob}}),
                           "marker_add": marker_tool}) as server:
            with self.connect(server) as session:
                self.assertEqual(session.project.summary()["blob"], blob)
                session.marker.add(time=1, name="n" * 100, comment=blob)
        self.assertEqual(len(server.calls("marker_add")[0]["arguments"]["comment"]), 600_000)

    def test_a_sequence_can_be_chosen_by_name_or_id(self):
        with EditorServer({"project_summary": summary_tool}) as server:
            with self.connect(server, sequence="Main") as session:
                self.assertEqual(session.sequence_id, 7)
        with EditorServer({"project_summary": summary_tool}) as server:
            with self.connect(server, sequence="7") as session:  # an id typed on a command line
                self.assertEqual(session.sequence_id, 7)

    def test_a_sequence_that_does_not_exist_is_named_in_the_error(self):
        with EditorServer({"project_summary": summary_tool}) as server:
            with self.assertRaises(wabcut.NotFoundError) as caught:
                self.connect(server, sequence="Nope")
            self.assertIn("7: Main", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
