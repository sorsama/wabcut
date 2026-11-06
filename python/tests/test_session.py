"""The session against a fake Wabcut: what is sent, what is refused before sending, how errors map."""

import unittest

import wabcut
from wabcut._errors import command_error

from support import fake_session, structured


def edit_ok(revision=1, **more):
    return lambda args: structured({"revision": revision, "committed": True, "dryRun": False, "basedOnRevision": revision - 1,
                                    **more})


class CallTests(unittest.TestCase):
    def test_a_read_returns_its_data_and_an_edit_returns_a_result(self):
        session, channel = fake_session({
            "project_summary": lambda a: structured({"revision": 0, "data": {"media": 3}}),
            "marker_add": edit_ok(4, createdIds=[9]),
        })
        self.assertEqual(session.project.summary(), {"media": 3})
        result = session.marker.add(time="00:00:01:00", name="A")
        self.assertIsInstance(result, wabcut.Result)
        self.assertTrue(result.committed)
        self.assertEqual((result.revision, result.created_ids, result.created_id), (4, [9], 9))
        self.assertEqual(session.revision, 4)
        self.assertEqual(channel.calls("marker_add")[0]["arguments"], {"time": "00:00:01:00", "name": "A"})

    def test_the_envelope_goes_with_an_edit(self):
        session, channel = fake_session({"marker_add": edit_ok()})
        session.marker.add(time=5, dryRun=True, expectedRevision=0, idempotencyKey="k")
        self.assertEqual(channel.calls("marker_add")[0]["arguments"],
                         {"time": 5, "dryRun": True, "expectedRevision": 0, "idempotencyKey": "k"})

    def test_the_dict_form_and_keywords_merge_and_a_keyword_wins(self):
        session, channel = fake_session({"marker_add": edit_ok()})
        session.marker.add({"time": 1, "name": "dict"}, name="keyword")
        self.assertEqual(channel.calls("marker_add")[0]["arguments"], {"time": 1, "name": "keyword"})

    def test_none_leaves_a_parameter_out(self):
        session, channel = fake_session({"marker_add": edit_ok()})
        session.marker.add(time=1, name=None, comment=None)
        self.assertEqual(channel.calls("marker_add")[0]["arguments"], {"time": 1})

    def test_none_is_sent_where_the_schema_allows_null(self):
        session, channel = fake_session({"marker_add": edit_ok()})
        session.marker.add(time=1, highlight=None)  # {"type": ["object", "null"]}
        self.assertEqual(channel.calls("marker_add")[0]["arguments"], {"time": 1, "highlight": None})

    def test_a_keyword_named_parameter_takes_a_trailing_underscore(self):
        session, channel = fake_session({"sequence_summary": lambda a: structured({"revision": 0, "data": {}})})
        session.sequence.summary(from_="00:00:01:00", to="00:00:02:00")
        self.assertEqual(channel.calls("sequence_summary")[0]["arguments"], {"from": "00:00:01:00", "to": "00:00:02:00"})

    def test_command_by_name_accepts_aliases_and_tool_names(self):
        session, channel = fake_session({"marker_add": edit_ok()})
        session.command("marker_add", {"time": 1})
        session.command("marker.add", {"time": 2})
        self.assertEqual([c["arguments"]["time"] for c in channel.calls("marker_add")], [1, 2])

    def test_query_refuses_an_edit(self):
        session, channel = fake_session()
        with self.assertRaises(wabcut.PermissionDeniedError):
            session.query("marker.add", {"time": 1})
        self.assertEqual(channel.calls(), [])

    def test_a_command_the_package_does_not_know_is_sent_as_it_is(self):
        session, channel = fake_session({"newer_thing": lambda a: structured({"revision": 2, "data": {"x": 1}})})
        self.assertEqual(session.command("newer.thing", {"anything": True}), {"x": 1})
        self.assertEqual(channel.calls("newer_thing")[0]["arguments"], {"anything": True})


class RefusedBeforeSendingTests(unittest.TestCase):
    def test_a_bad_parameter_never_reaches_the_server(self):
        session, channel = fake_session({"clip_rename": edit_ok()})
        with self.assertRaises(wabcut.InvalidParamsError) as caught:
            session.clip.rename(clips="not a list", name=5)
        error = caught.exception
        self.assertTrue(error.client_side)
        self.assertEqual(error.command, "clip.rename")
        self.assertEqual(error.code, "InvalidParams")
        paths = {i["path"] for i in error.details["issues"]}
        self.assertEqual(paths, {"/clips", "/name"})
        self.assertEqual(channel.calls(), [])

    def test_a_missing_required_parameter_and_an_unknown_one_are_named(self):
        session, channel = fake_session()
        with self.assertRaises(wabcut.InvalidParamsError) as caught:
            session.clip.rename(bogus=1)
        text = {(i["path"], i["message"]) for i in caught.exception.details["issues"]}
        self.assertIn(("/bogus", "is not a known parameter"), text)
        self.assertIn(("/", 'is missing required "clips"'), text)
        self.assertEqual(channel.calls(), [])

    def test_a_read_only_session_refuses_edits_and_says_how_to_allow_them(self):
        session, channel = fake_session(edit=False)
        with self.assertRaises(wabcut.PermissionDeniedError) as caught:
            session.marker.add(time=1)
        self.assertIn("edit=True", str(caught.exception))
        self.assertTrue(caught.exception.client_side)
        for call in (session.undo, session.redo, session.save, lambda: session.batch([("marker.add", {"time": 1})])):
            with self.assertRaises(wabcut.PermissionDeniedError):
                call()
        with self.assertRaises(wabcut.PermissionDeniedError):
            with session.transaction():
                pass
        self.assertEqual(channel.calls(), [])

    def test_analysis_and_file_commands_need_their_flags(self):
        session, channel = fake_session()
        with self.assertRaises(wabcut.PermissionDeniedError) as analysis:
            session.audio.loudness()
        self.assertIn("analysis=True", str(analysis.exception))
        with self.assertRaises(wabcut.PermissionDeniedError) as files:
            session.captions.export(path="x.srt")
        self.assertIn("files=True", str(files.exception))
        self.assertEqual(channel.calls(), [])

    def test_a_script_run_inside_wabcut_gets_messages_that_do_not_mention_connect_flags(self):
        session, _ = fake_session(edit=False, origin="app")
        with self.assertRaises(wabcut.PermissionDeniedError) as caught:
            session.marker.add(time=1)
        self.assertNotIn("connect(", str(caught.exception))
        self.assertIn("not allowed to edit", str(caught.exception))


class ErrorMappingTests(unittest.TestCase):
    def test_every_catalog_code_has_its_own_exception(self):
        cases = {
            "UnknownCommand": wabcut.UnknownCommandError, "UnsupportedVersion": wabcut.UnsupportedVersionError,
            "InvalidRequest": wabcut.InvalidRequestError, "InvalidParams": wabcut.InvalidParamsError,
            "NotFound": wabcut.NotFoundError, "PermissionDenied": wabcut.PermissionDeniedError,
            "StaleRevision": wabcut.StaleRevisionError, "IdempotencyConflict": wabcut.IdempotencyConflictError,
            "EditRejected": wabcut.EditRejectedError, "InvariantViolation": wabcut.InvariantViolationError,
            "LockedTrack": wabcut.LockedTrackError, "Cancelled": wabcut.CancelledError, "TimedOut": wabcut.TimedOutError,
            "Internal": wabcut.InternalError,
        }
        for code, cls in cases.items():
            error = command_error("x.y", code, "m", {"d": 1})
            self.assertIs(type(error), cls)
            self.assertIsInstance(error, wabcut.CommandError)
            self.assertEqual((error.command, error.code, error.message, error.details), ("x.y", code, "m", {"d": 1}))
        self.assertIs(type(command_error("x", "SomethingNew", "m")), wabcut.CommandError)

    def test_an_error_from_the_server_carries_the_command_code_and_details(self):
        error = {"code": "StaleRevision", "message": "the project is at revision 5, not 3", "details": {"current": 5}}
        session, _ = fake_session({"marker_add": lambda a: structured({"revision": 5}, error=error)})
        with self.assertRaises(wabcut.StaleRevisionError) as caught:
            session.marker.add(time=1, expectedRevision=3)
        self.assertEqual(caught.exception.command, "marker.add")
        self.assertEqual(caught.exception.details, {"current": 5})
        self.assertFalse(caught.exception.client_side)
        self.assertEqual(session.revision, 5)

    def test_an_unknown_tool_becomes_an_unknown_command_with_a_suggestion(self):
        session, _ = fake_session()
        with self.assertRaises(wabcut.UnknownCommandError) as caught:
            session.command("clip.addEfect", {"clips": [1]})
        self.assertIn("clip.addEffect", str(caught.exception))

    def test_describe_an_unknown_command(self):
        session, _ = fake_session()
        with self.assertRaises(wabcut.UnknownCommandError):
            session.describe("nope.nothing")
        self.assertIn("clips", session.describe("clip.rename").help())


class BatchAndTransactionTests(unittest.TestCase):
    def test_a_batch_is_one_call_with_canonical_names(self):
        session, channel = fake_session({"edit_batch": lambda a: structured(
            {"revision": 3, "committed": True, "steps": [{"command": "marker.add"}] * len(a["commands"])})})
        result = session.batch([("marker.add", {"time": 1}), {"command": "marker_add", "params": {"time": 2}}], label="Two")
        arguments = channel.calls("edit_batch")[0]["arguments"]
        self.assertEqual(arguments["label"], "Two")
        self.assertEqual([c["command"] for c in arguments["commands"]], ["marker.add", "marker.add"])
        self.assertEqual(len(result.steps), 2)

    def test_a_bad_command_in_a_batch_is_named_by_index_before_anything_is_sent(self):
        session, channel = fake_session()
        with self.assertRaises(wabcut.InvalidParamsError) as caught:
            session.batch([("marker.add", {"time": 1}), ("marker.add", {"name": "no time"})])
        self.assertEqual(caught.exception.details["issues"][0]["path"], "/commands/1")
        self.assertEqual(channel.calls(), [])

    def test_a_transaction_commits_when_the_block_ends(self):
        session, channel = fake_session({
            "edit_transaction": lambda a: structured({"revision": 4, "committed": a["action"] == "commit"}),
            "marker_add": edit_ok(2),
        })
        with session.transaction("Mark things"):
            session.marker.add(time=1)
        actions = [c["arguments"] for c in channel.calls("edit_transaction")]
        self.assertEqual(actions, [{"action": "begin", "label": "Mark things"}, {"action": "commit"}])
        self.assertEqual(session.events("commandApplied")[-1]["label"], "Mark things")

    def test_an_exception_in_a_transaction_rolls_it_back_and_propagates(self):
        session, channel = fake_session({"edit_transaction": lambda a: structured({"revision": 1}), "marker_add": edit_ok(2)})
        with self.assertRaises(RuntimeError):
            with session.transaction():
                session.marker.add(time=1)
                raise RuntimeError("the script failed")
        actions = [c["arguments"]["action"] for c in channel.calls("edit_transaction")]
        self.assertEqual(actions, ["begin", "rollback"])

    def test_transactions_on_a_wabcut_without_them_say_so(self):
        session, _ = fake_session()  # no edit_transaction tool: "Unknown tool"
        with self.assertRaises(wabcut.NotSupportedError):
            with session.transaction():
                pass


class EventsAndLifetimeTests(unittest.TestCase):
    def test_events_are_recorded_and_handlers_called(self):
        session, _ = fake_session({"marker_add": edit_ok(2),
                                   "edit_undo": lambda a: structured({"revision": 3, "committed": True, "undone": "Add Marker"})})
        seen = []
        off = session.on("commandApplied", seen.append)
        session.marker.add(time=1)
        session.undo()
        self.assertEqual([e["source"] for e in seen], ["edit", "undo"])
        self.assertEqual(seen[0]["command"], "marker.add")
        self.assertEqual(seen[1]["label"], "Add Marker")
        off()
        session.marker.add(time=2)
        self.assertEqual(len(seen), 2)
        self.assertEqual(len(session.events("commandApplied")), 3)
        self.assertEqual(session.events("commandApplied", clear=True)[0]["revision"], 2)
        self.assertEqual(session.events(), [])

    def test_a_dry_run_is_not_an_event(self):
        session, _ = fake_session({"marker_add": lambda a: structured({"revision": 1, "committed": False, "dryRun": True})})
        session.marker.add(time=1, dryRun=True)
        self.assertEqual(session.events(), [])

    def test_a_handler_that_raises_does_not_break_the_edit(self):
        session, _ = fake_session({"marker_add": edit_ok(2)})
        session.on("commandApplied", lambda e: 1 / 0)
        self.assertTrue(session.marker.add(time=1).committed)

    def test_a_call_that_takes_too_long_is_cancelled(self):
        session, channel = fake_session({"marker_add": edit_ok()}, call_timeout=0.3)
        session._grace = 0.0
        channel.silent = True
        with self.assertRaises(wabcut.CallTimeoutError):
            session.marker.add(time=1)
        cancelled = [m for m in channel.sent if m.get("method") == "notifications/cancelled"]
        self.assertEqual(len(cancelled), 1)

    def test_a_connection_that_ends_is_reported(self):
        session, channel = fake_session()
        channel.silent = True
        channel.end()
        with self.assertRaises(wabcut.ConnectionLostError):
            session.project.summary()

    def test_a_closed_session_refuses_calls(self):
        session, channel = fake_session()
        session.close()
        self.assertTrue(channel.closed)
        with self.assertRaises(wabcut.WabcutError):
            session.project.summary()

    def test_a_session_inside_wabcut_that_ends_with_an_error_tells_the_editor_to_discard(self):
        session, channel = fake_session(origin="app")
        with self.assertRaises(RuntimeError):
            with session:
                raise RuntimeError("the script failed")
        self.assertIn("notifications/wabcut-abort", [m.get("method") for m in channel.sent])
        self.assertTrue(channel.closed)

    def test_a_session_that_ends_well_does_not(self):
        session, channel = fake_session(origin="app")
        with session:
            pass
        self.assertNotIn("notifications/wabcut-abort", [m.get("method") for m in channel.sent])
        self.assertTrue(channel.closed)

    def test_records_turn_rows_into_dicts(self):
        data = {"clipFields": ["id", "name"], "clips": [[1, "a"], [2, "b"]], "mediaFields": ["id"], "media": [[7]]}
        self.assertEqual(wabcut.records(data, "clips"), [{"id": 1, "name": "a"}, {"id": 2, "name": "b"}])
        self.assertEqual(wabcut.records(data, "media"), [{"id": 7}])
        with self.assertRaises(KeyError):
            wabcut.records(data, "nothing")

    def test_connect_without_a_project_outside_wabcut_explains_itself(self):
        import os
        saved = {k: os.environ.pop(k, None) for k in ("WABCUT_SESSION", "WABCUT_TOKEN")}
        try:
            with self.assertRaises(wabcut.WabcutError) as caught:
                wabcut.connect()
            self.assertIn("Scripts menu", str(caught.exception))
        finally:
            for k, v in saved.items():
                if v is not None:
                    os.environ[k] = v

    def test_a_missing_project_file_is_named(self):
        with self.assertRaises(wabcut.WabcutError) as caught:
            wabcut.connect("definitely-not-here.wabcut")
        self.assertIn("definitely-not-here.wabcut", str(caught.exception))

    def test_a_missing_cli_says_where_it_looked_and_how_to_fix_it(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as folder:
            project = Path(folder) / "p.wabcut"
            project.write_text("{}", encoding="utf-8")
            with self.assertRaises(wabcut.CliNotFoundError) as caught:
                wabcut.connect(project, cli=Path(folder) / "no-such-wabcut-cli")
            self.assertIn("WABCUT_CLI", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
