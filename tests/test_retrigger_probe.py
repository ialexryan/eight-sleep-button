"""Offline repeat-activation experiment contracts; all credentials are synthetic."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import requests

import test_eightctl as fixtures


SPEC = importlib.util.spec_from_file_location(
    "retrigger_probe", Path(__file__).resolve().parents[1] / "scripts/retrigger_probe.py")
probe = importlib.util.module_from_spec(SPEC)
# Reuse the same exception/client classes as the synthetic transport helpers.
with patch.dict(sys.modules, {"eightctl": fixtures.eight}):
    SPEC.loader.exec_module(probe)


def active(started="2026-09-17T10:00:00Z", until="2026-09-17T10:15:00Z"):
    aggregate = fixtures.temperature("hotFlash")
    aggregate["devices"][0]["currentState"].update(
        started=started, until=until, instance={"timestamp": started})
    return aggregate


class RetriggerProbeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.local = Path(temporary.name) / ".local"
        self.output = io.StringIO()
        self.output_context = contextlib.redirect_stdout(self.output)
        self.output_context.__enter__()
        self.addCleanup(self.output_context.__exit__, None, None, None)

    def client(self, *responses, target=fixtures.TARGET):
        transport = fixtures.Transport(*responses)
        client = fixtures.eight.EightClient(self.local, transport, lambda: 1000)
        client._accept_tokens(fixtures.tokens())
        fixtures.eight.save_private(self.local / "native-verification.json", {
            "target_hash": fixtures.eight.fingerprint(target),
            "activation_observed": True, "manual_cleanup_verified": True,
            "user_observed_app_and_bed": True})
        return client, transport

    def inspect_responses(self, before=None):
        return [fixtures.identity(), fixtures.summary(), fixtures.SETTINGS,
                active() if before is None else before]

    def run_probe(self, client, send=False):
        return probe.probe(client, send=send, sleep=lambda _: None)

    def test_default_is_read_only_for_active_and_inactive_sides(self):
        for before in (active(), fixtures.temperature("smart:final")):
            with self.subTest(state=before["devices"][0]["currentState"]["type"]):
                client, transport = self.client(*self.inspect_responses(before))
                record = self.run_probe(client)
                self.assertFalse(record["request_sent"])
                self.assertEqual(record["observations"], [])
                self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4)
                self.assertFalse((self.local / "retrigger-probe.json").exists())

    def test_cli_without_activate_flag_is_read_only(self):
        client, transport = self.client(*self.inspect_responses())
        with patch.object(probe, "EightClient", return_value=client), patch.object(sys, "argv", ["retrigger_probe.py"]):
            probe.main()
        self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4)

    def test_verified_active_side_gets_exactly_one_bodyless_put(self):
        client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=204),
                                       active(), active(), active(), fixtures.SETTINGS)
        record = self.run_probe(client, send=True)
        methods = [call[0] for call in transport.calls]
        self.assertEqual(methods, ["GET"] * 4 + ["PUT"] + ["GET"] * 4)
        put = transport.calls[4]
        self.assertTrue(put[1].endswith("/temperature/hot-flash-mode/activate"))
        self.assertNotIn("json", put[2])
        self.assertNotIn("data", put[2])
        self.assertEqual(record["request_result"], "accepted")
        self.assertTrue(record["settings_preserved"])
        self.assertTrue(all(item["configuration_preserved"] for item in record["observations"]))
        self.assertEqual(len(record["observations"]), 3)

    def test_previous_target_mismatch_cannot_send_activation(self):
        client, transport = self.client(*self.inspect_responses(), target={**fixtures.TARGET, "side": "right"})
        with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "Target differs"):
            self.run_probe(client, send=True)
        self.assertTrue(all(call[0] == "GET" for call in transport.calls))
        self.assertFalse((self.local / "retrigger-probe.json").exists())

    def test_inactive_verified_side_cannot_send_activation(self):
        client, transport = self.client(*self.inspect_responses(fixtures.temperature("smart:final")))
        with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "no longer active"):
            self.run_probe(client, send=True)
        self.assertTrue(all(call[0] == "GET" for call in transport.calls))
        self.assertFalse((self.local / "retrigger-probe.json").exists())

    def test_live_assignment_mismatch_stops_before_put(self):
        mismatched = fixtures.summary()
        mismatched["households"][0]["sets"][0]["devices"][0]["pairing"]["leftUserId"] = "synthetic-partner"
        client, transport = self.client(fixtures.identity(), mismatched)
        with self.assertRaises(fixtures.eight.DiagnosticError):
            self.run_probe(client, send=True)
        self.assertEqual([call[0] for call in transport.calls], ["GET", "GET"])

    def test_ambiguous_put_gets_reads_only_and_no_reset_claim(self):
        client, transport = self.client(*self.inspect_responses(), requests.Timeout("synthetic-secret-transport-detail"),
                                       active(), active(), active(), fixtures.SETTINGS)
        record = self.run_probe(client, send=True)
        self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4 + ["PUT"] + ["GET"] * 4)
        self.assertEqual(record["request_result"], "ambiguous")
        self.assertTrue(all(item["timestamps"] == record["before"]["timestamps"] for item in record["observations"]))
        self.assertNotIn("synthetic-secret-transport-detail", self.output.getvalue())
        for outcome in ("reset_confirmed", "timer_restarted", "reset_success", "success"):
            self.assertNotIn(outcome, record)
            self.assertNotIn(outcome, self.output.getvalue())

    def test_read_failure_after_ambiguous_put_preserves_intent_without_replay(self):
        client, transport = self.client(*self.inspect_responses(), requests.Timeout(), requests.Timeout())
        with self.assertRaises(fixtures.eight.ApiError):
            self.run_probe(client, send=True)
        saved = fixtures.eight.load_private(self.local / "retrigger-probe.json")
        self.assertTrue(saved["request_sent"])
        self.assertEqual(saved["request_result"], "ambiguous")
        self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4 + ["PUT", "GET"])

    def test_503_retry_after_blocks_all_direct_probe_confirmation_requests(self):
        client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=503, headers={"Retry-After": "120"}))
        with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "backoff"):
            self.run_probe(client, send=True)
        saved = fixtures.eight.load_private(self.local / "retrigger-probe.json")
        self.assertEqual(saved["request_result"], "ambiguous")
        self.assertEqual(saved["http_status"], 503)
        self.assertEqual(saved["observations"], [])
        self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4 + ["PUT"])

    def test_explicit_rejection_has_no_automatic_put_or_get_retry(self):
        for status in (401, 409):
            with self.subTest(status=status):
                client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=status, raw=b"synthetic-secret-body"))
                with self.assertRaises(fixtures.eight.ApiError) as caught:
                    self.run_probe(client, send=True)
                saved = fixtures.eight.load_private(self.local / "retrigger-probe.json")
                self.assertEqual(saved["request_result"], "rejected")
                self.assertEqual(saved["http_status"], status)
                self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4 + ["PUT"])
                self.assertNotIn("synthetic-secret-body", str(caught.exception))
                self.assertNotIn("synthetic-secret-body", self.output.getvalue())

    def test_timestamp_output_is_allowlisted_normalized_and_timezone_aware(self):
        aggregate = active()
        state = aggregate["devices"][0]["currentState"]
        state.update({"timestamp": "synthetic-secret-id", "startTime": "2026-09-17T10:00:00",
                      "endTime": 1234, "expiresAt": "x" * 41, "privateToken": "synthetic-secret-token"})
        state["instance"].update({"until": "2026-09-17T03:15:00-07:00", "privateUser": fixtures.USER})
        aggregate["devices"][0]["overrideLevels"]["hotFlash"] = {
            "endTime": "2026-09-17T10:15:00Z", "privateValue": fixtures.DEVICE}
        filtered = probe.timing(aggregate, fixtures.TARGET)
        self.assertEqual(filtered["timestamps"], {
            "currentState.started": "2026-09-17T10:00:00+00:00",
            "currentState.until": "2026-09-17T10:15:00+00:00",
            "currentState.instance.timestamp": "2026-09-17T10:00:00+00:00",
            "currentState.instance.until": "2026-09-17T03:15:00-07:00",
            "overrideLevels.hotFlash.endTime": "2026-09-17T10:15:00+00:00"})
        self.assertNotIn("synthetic", json.dumps(filtered))

    def test_non_object_optional_timestamp_sources_are_ignored(self):
        aggregate = active()
        aggregate["devices"][0]["currentState"]["instance"] = ["synthetic-secret"]
        aggregate["devices"][0]["overrideLevels"]["hotFlash"] = "synthetic-secret"
        self.assertEqual(set(probe.timing(aggregate, fixtures.TARGET)["timestamps"]),
                         {"currentState.started", "currentState.until"})

    def test_output_and_saved_record_omit_raw_identifiers_and_credentials(self):
        before = active()
        state = before["devices"][0]["currentState"]
        state.update({"email": "synthetic-email@example.invalid", "timestamp": "synthetic-access",
                      "instance": {"timestamp": "synthetic-refresh", "password": "synthetic-password"}})
        client, _ = self.client(*self.inspect_responses(before), fixtures.Response(status=204),
                               before, before, before, fixtures.SETTINGS)
        self.run_probe(client, send=True)
        combined = self.output.getvalue() + (self.local / "retrigger-probe.json").read_text()
        for secret in (fixtures.USER, fixtures.DEVICE, "synthetic-access", "synthetic-refresh",
                       "synthetic-password", "synthetic-email@example.invalid", "synthetic-schedule"):
            self.assertNotIn(secret, combined)

    def test_changed_settings_and_configuration_are_reported_false(self):
        changed = active()
        changed["schedules"][0]["enabled"] = False
        settings = {**fixtures.SETTINGS, "levelDelta": -90}
        client, _ = self.client(*self.inspect_responses(), fixtures.Response(status=204),
                               active(), changed, changed, settings)
        record = self.run_probe(client, send=True)
        self.assertEqual([item["configuration_preserved"] for item in record["observations"]], [True, False, False])
        self.assertFalse(record["settings_preserved"])
        saved = fixtures.eight.load_private(self.local / "retrigger-probe.json")
        self.assertFalse(saved["settings_preserved"])

    def test_restart_sends_off_then_reverifies_normal_state_then_bodyless_on(self):
        renewed = active("2026-09-17T10:05:00Z", "2026-09-17T10:20:00Z")
        client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=204),
                                       *self.inspect_responses(fixtures.temperature("smart:final")),
                                       fixtures.Response(status=204), renewed, renewed, renewed, fixtures.SETTINGS)
        record = probe.probe(client, restart=True, sleep=lambda _: None)
        self.assertEqual([call[0] for call in transport.calls],
                         ["GET"] * 4 + ["PUT"] + ["GET"] * 4 + ["PUT"] + ["GET"] * 4)
        writes = [call for call in transport.calls if call[0] == "PUT"]
        self.assertEqual([call[1].rsplit("/", 1)[-1] for call in writes], ["deactivate", "activate"])
        for call in writes:
            self.assertNotIn("json", call[2])
            self.assertNotIn("data", call[2])
        self.assertEqual(record["strategy"], "deactivate_then_activate")
        self.assertTrue(record["deactivation_confirmed"])
        self.assertEqual(record["deactivation_result"], "accepted")
        self.assertTrue(record["settings_preserved"])
        self.assertTrue(all(item["configuration_preserved"] for item in record["observations"]))
        self.assertEqual(record["observations"][0]["timestamps"]["currentState.until"], "2026-09-17T10:20:00+00:00")

    def test_restart_without_confirmed_normal_state_never_sends_on(self):
        for state in ("hotFlash", "unknown", "future-unrecognized-state"):
            with self.subTest(state=state):
                client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=204),
                                               *self.inspect_responses(fixtures.temperature(state)))
                with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "Deactivation unconfirmed"):
                    probe.probe(client, restart=True, sleep=lambda _: None)
                writes = [call[1].rsplit("/", 1)[-1] for call in transport.calls if call[0] == "PUT"]
                self.assertEqual(writes, ["deactivate"])
                self.assertFalse(fixtures.eight.load_private(self.local / "retrigger-probe.json")["request_sent"])

    def test_restart_rejected_deactivation_never_sends_on_or_retries(self):
        client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=409))
        with self.assertRaises(fixtures.eight.ApiError):
            probe.probe(client, restart=True, sleep=lambda _: None)
        self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4 + ["PUT"])
        self.assertTrue(transport.calls[-1][1].endswith("/deactivate"))
        saved = fixtures.eight.load_private(self.local / "retrigger-probe.json")
        self.assertEqual(saved["deactivation_result"], "rejected")
        self.assertFalse(saved["request_sent"])

    def test_restart_ambiguous_off_can_proceed_only_after_read_confirmation(self):
        for confirmed in (False, True):
            with self.subTest(confirmed=confirmed):
                second_state = fixtures.temperature("smart:final") if confirmed else active()
                responses = [*self.inspect_responses(), requests.Timeout("synthetic-secret-off-error"),
                             *self.inspect_responses(second_state)]
                if confirmed:
                    responses.extend([fixtures.Response(status=204), active(), active(), active(), fixtures.SETTINGS])
                client, transport = self.client(*responses)
                if confirmed:
                    record = probe.probe(client, restart=True, sleep=lambda _: None)
                    self.assertEqual(record["deactivation_result"], "ambiguous")
                    self.assertTrue(record["deactivation_confirmed"])
                else:
                    with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "Deactivation unconfirmed"):
                        probe.probe(client, restart=True, sleep=lambda _: None)
                writes = [call[1].rsplit("/", 1)[-1] for call in transport.calls if call[0] == "PUT"]
                self.assertEqual(writes, ["deactivate", "activate"] if confirmed else ["deactivate"])
                self.assertNotIn("synthetic-secret-off-error", self.output.getvalue())

    def test_restart_changed_target_settings_or_configuration_cancels_on(self):
        for changed in ("target", "settings", "configuration"):
            with self.subTest(changed=changed):
                current_identity, current_summary = fixtures.identity(), fixtures.summary()
                current_settings = dict(fixtures.SETTINGS)
                normal = fixtures.temperature("smart:final")
                if changed == "target":
                    current_identity = fixtures.identity("right")
                    device = current_summary["households"][0]["sets"][0]["devices"][0]
                    device["assignment"] = device["pairing"] = {"leftUserId": "partner", "rightUserId": fixtures.USER}
                    normal = fixtures.temperature("smart:final", side="right")
                elif changed == "settings":
                    current_settings["levelDelta"] = -90
                else:
                    normal["schedules"][0]["enabled"] = False
                client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=204),
                                               current_identity, current_summary, current_settings, normal)
                with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "changed after deactivation"):
                    probe.probe(client, restart=True, sleep=lambda _: None)
                self.assertEqual([call[1].rsplit("/", 1)[-1] for call in transport.calls if call[0] == "PUT"], ["deactivate"])

    def test_restart_stale_window_refuses_final_activation(self):
        client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=204),
                                       *self.inspect_responses(fixtures.temperature("smart:final")))
        now = [1000]
        client.clock = lambda: now[0]
        original_inspect = client.inspect
        inspect_calls = [0]

        def slow_second_inspection():
            result = original_inspect()
            inspect_calls[0] += 1
            if inspect_calls[0] == 2:
                now[0] += 21
            return result

        with patch.object(client, "inspect", side_effect=slow_second_inspection):
            with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "short request window"):
                probe.probe(client, restart=True, sleep=lambda _: None)
        self.assertEqual([call[1].rsplit("/", 1)[-1] for call in transport.calls if call[0] == "PUT"], ["deactivate"])

    def test_restart_ambiguous_final_activation_never_replays_either_write(self):
        client, transport = self.client(*self.inspect_responses(), fixtures.Response(status=204),
                                       *self.inspect_responses(fixtures.temperature("smart:final")),
                                       requests.Timeout(), active(), active(), active(), fixtures.SETTINGS)
        record = probe.probe(client, restart=True, sleep=lambda _: None)
        self.assertEqual(record["request_result"], "ambiguous")
        self.assertEqual([call[1].rsplit("/", 1)[-1] for call in transport.calls if call[0] == "PUT"], ["deactivate", "activate"])
        self.assertNotIn("reset_confirmed", record)

    def test_restart_503_retry_after_stops_at_each_mutation(self):
        for failure_point in ("deactivate", "activate"):
            with self.subTest(failure_point=failure_point):
                responses = self.inspect_responses()
                if failure_point == "activate":
                    responses += [fixtures.Response(status=204),
                                  *self.inspect_responses(fixtures.temperature("smart:final"))]
                responses.append(fixtures.Response(status=503, headers={"Retry-After": "120"}))
                client, transport = self.client(*responses)
                with self.assertRaisesRegex(fixtures.eight.DiagnosticError, "backoff"):
                    probe.probe(client, restart=True, sleep=lambda _: None)
                expected = ["GET"] * 4 + ["PUT"]
                if failure_point == "activate":
                    expected += ["GET"] * 4 + ["PUT"]
                self.assertEqual([call[0] for call in transport.calls], expected)
                self.assertTrue(transport.calls[-1][1].endswith("/" + failure_point))


if __name__ == "__main__":
    unittest.main()
