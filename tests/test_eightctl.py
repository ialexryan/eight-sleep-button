"""Offline safety contracts. All responses and credentials are synthetic."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import tempfile
import unittest

import requests

SPEC = importlib.util.spec_from_file_location("eightctl", Path(__file__).resolve().parents[1] / "scripts/eightctl.py")
eight = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(eight)

USER, DEVICE = "synthetic-user", "synthetic-device"
TARGET = {"user_id": USER, "device_id": DEVICE, "side": "left"}
SETTINGS = {"enabled": False, "levelDelta": -100, "hotFlashDuration": "0:15:00"}


def tokens(access="synthetic-access", refresh="synthetic-refresh", user=USER, expires=72000):
    return {"access_token": access, "refresh_token": refresh, "userId": user, "expires_in": expires}


def identity(side="left"):
    return {"user": {"userId": USER, "currentDevice": {"id": DEVICE, "side": side}}}


def summary():
    return {"currentSet": "synthetic-set", "households": [{"sets": [{"setId": "synthetic-set", "devices": [
        {"deviceId": DEVICE, "specialization": "pod", "assignment": {"leftUserId": USER, "rightUserId": "partner"},
         "pairing": {"leftUserId": USER, "rightUserId": "partner"}}]}]}]}


def temperature(state="smart:final", side="left"):
    return {"devices": [{"device": {"deviceId": DEVICE, "side": side, "specialization": "pod"},
                         "currentState": {"type": state}, "overrideLevels": {}}],
            "temperatureSettings": [{"name": "pod", "finalSleepLevel": -10}],
            "schedules": [{"id": "synthetic-schedule", "time": "21:00:00", "enabled": True}]}


class Response:
    def __init__(self, value=None, status=200, headers=None, raw=None):
        self.status_code, self.headers = status, headers or {}
        self.payload = raw if raw is not None else (json.dumps(value).encode() if value is not None else b"")

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def iter_content(self, size):
        for start in range(0, len(self.payload), size):
            yield self.payload[start:start + size]


class Transport:
    def __init__(self, *replies):
        self.replies, self.calls = list(replies), []

    def request(self, method, url, **kwargs):
        self.calls.append((method, url, kwargs))
        if not self.replies:
            raise AssertionError("Unexpected API call")
        reply = self.replies.pop(0)
        if isinstance(reply, Exception):
            raise reply
        return reply if isinstance(reply, Response) else Response(reply)


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.local = Path(self.directory.name) / ".local"
        self.now = 1000.0

    def client(self, *replies):
        transport = Transport(*replies)
        client = eight.EightClient(self.local, transport, lambda: self.now)
        client._accept_tokens(tokens())
        return client, transport

    def test_hidden_secret_persistence_never_saves_password(self):
        client, transport = self.client(tokens())
        client.login("synthetic-email", "synthetic-password")
        saved = client.session_path.read_text()
        self.assertNotIn("synthetic-password", saved)
        self.assertNotIn("synthetic-email", saved)
        self.assertEqual(stat.S_IMODE(client.session_path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.local.stat().st_mode), 0o700)
        self.assertEqual(client.tokens["expires_at"], 73000)
        self.assertFalse(transport.calls[0][2]["allow_redirects"])
        self.assertNotIn("verify", transport.calls[0][2])  # requests verifies TLS by default.

    def test_rotated_refresh_is_persisted_and_password_not_retried(self):
        client, transport = self.client(tokens(refresh="rotated-synthetic"))
        client.refresh()
        self.assertEqual(eight.load_private(client.session_path)["refresh_token"], "rotated-synthetic")
        self.assertEqual(transport.calls[0][2]["json"]["grant_type"], "refresh_token")
        self.assertNotIn("password", transport.calls[0][2]["json"])

    def test_refresh_missing_rotation_preserves_existing(self):
        reply = tokens()
        del reply["refresh_token"]
        client, _ = self.client(reply)
        client.refresh()
        self.assertEqual(client.tokens["refresh_token"], "synthetic-refresh")

    def test_refresh_identity_change_does_not_replace_session(self):
        client, _ = self.client(tokens(user="different-synthetic-user"))
        with self.assertRaisesRegex(eight.DiagnosticError, "identity changed"):
            client.refresh()
        self.assertEqual(client.tokens["user_id"], USER)

    def test_refresh_without_user_id_retains_established_identity_and_rotation(self):
        reply = tokens(access="rotated-access", refresh="rotated-refresh")
        del reply["userId"]
        client, _ = self.client(reply)
        client.refresh()
        saved = eight.load_private(client.session_path)
        self.assertEqual(saved["user_id"], USER)
        self.assertEqual(saved["access_token"], "rotated-access")
        self.assertEqual(saved["refresh_token"], "rotated-refresh")

    def test_initial_login_without_user_id_is_rejected(self):
        reply = tokens()
        del reply["userId"]
        client = eight.EightClient(self.local, Transport(reply), lambda: self.now)
        with self.assertRaisesRegex(eight.DiagnosticError, "user identity"):
            client.login("synthetic-email", "synthetic-password")
        self.assertFalse(client.session_path.exists())

    def test_refresh_explicit_invalid_user_id_cannot_use_fallback(self):
        for user in (None, "", False):
            with self.subTest(user=user):
                client, _ = self.client(tokens(user=user))
                before = client.session_path.read_bytes()
                with self.assertRaisesRegex(eight.DiagnosticError, "user identity"):
                    client.refresh()
                self.assertEqual(client.session_path.read_bytes(), before)

    def test_refresh_failure_does_not_password_fallback(self):
        client, transport = self.client(Response(status=401, raw=b"secret-server-body"))
        with self.assertRaises(eight.ApiError) as raised:
            client.refresh()
        self.assertNotIn("secret-server-body", str(raised.exception))
        self.assertEqual(len(transport.calls), 1)

    def test_expired_token_refreshes_once_before_read(self):
        client, transport = self.client(tokens(access="new-access"), {"ok": True})
        client.tokens["expires_at"] = self.now + 30
        self.assertEqual(client.request("GET", "/test"), {"ok": True})
        self.assertEqual([call[0] for call in transport.calls], ["POST", "GET"])
        self.assertEqual(transport.calls[1][2]["headers"]["Authorization"], "Bearer new-access")

    def test_read_401_refreshes_only_once(self):
        client, transport = self.client(Response(status=401), tokens(), Response(status=401))
        with self.assertRaises(eight.ApiError):
            client.request("GET", "/test")
        self.assertEqual([call[0] for call in transport.calls], ["GET", "POST", "GET"])

    def test_put_401_is_never_replayed(self):
        client, transport = self.client(Response(status=401))
        with self.assertRaises(eight.ApiError):
            client.request("PUT", "/test")
        self.assertEqual(len(transport.calls), 1)

    def test_rate_limit_survives_process_reload(self):
        client, transport = self.client(Response(status=429, headers={"Retry-After": "120"}))
        with self.assertRaises(eight.ApiError):
            client.request("GET", "/test")
        second = eight.EightClient(self.local, transport, lambda: self.now).load()
        with self.assertRaisesRegex(eight.DiagnosticError, "rate limit"):
            second.request("GET", "/test")
        self.assertEqual(len(transport.calls), 1)

    def test_http_date_retry_after_is_honored(self):
        client, _ = self.client(Response(status=429, headers={"Retry-After": "Thu, 01 Jan 1970 00:20:00 GMT"}))
        with self.assertRaises(eight.ApiError):
            client.request("GET", "/test")
        self.assertEqual(client.blocked_until, 1200)

    def test_inspect_only_reads_and_redacted_status_has_no_identifiers(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature())
        target, settings, aggregate = client.inspect()
        self.assertEqual(target, TARGET)
        self.assertEqual(settings, SETTINGS)
        status = (self.local / "diagnostic-status.json").read_text()
        for secret in (USER, DEVICE, "synthetic-access", "synthetic-refresh", "partner"):
            self.assertNotIn(secret, status)
        self.assertTrue(all(call[0] == "GET" for call in transport.calls))

    def test_assignment_mismatch_blocks_before_settings_or_write(self):
        mismatched = summary()
        mismatched["households"][0]["sets"][0]["devices"][0]["assignment"]["leftUserId"] = "partner"
        client, transport = self.client(identity(), mismatched)
        with self.assertRaisesRegex(eight.DiagnosticError, "Cannot verify"):
            client.inspect()
        self.assertEqual(len(transport.calls), 2)

    def test_other_side_or_absent_state_is_not_confirmation(self):
        with self.assertRaises(eight.DiagnosticError):
            eight.selected_state(temperature("hotFlash", side="right"), TARGET)
        with self.assertRaises(eight.DiagnosticError):
            eight.selected_state({"devices": []}, TARGET)

    def test_already_active_sends_no_put(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature("hotFlash"))
        with contextlib.redirect_stdout(io.StringIO()):
            result = eight.activate(client, TARGET, SETTINGS, temperature())
        self.assertEqual(result, "already_active")
        self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4)

    def test_activation_bodyless_put_then_confirm_and_watch_termination(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature(), Response(status=204), temperature("hotFlash"), temperature(), SETTINGS)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(eight.activate(client, TARGET, SETTINGS, temperature()), "confirmed")
            report = eight.watch(client)
        self.assertTrue(report["settings_preserved"])
        self.assertTrue(report["schedule_configuration_preserved"])
        verification = eight.load_private(self.local / "native-verification.json")
        self.assertTrue(verification["termination_observed"])
        self.assertFalse(verification["user_observed_app_and_bed"])
        self.assertEqual(verification["target_hash"], eight.fingerprint(TARGET))
        put = [call for call in transport.calls if call[0] == "PUT"]
        self.assertEqual(len(put), 1)
        self.assertTrue(put[0][1].endswith("/temperature/hot-flash-mode/activate"))
        self.assertNotIn("json", put[0][2])
        self.assertNotIn("data", put[0][2])

    def test_timeout_verifies_without_replaying_put(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature(), requests.Timeout("sensitive transport detail"), temperature("hotFlash"))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = eight.activate(client, TARGET, SETTINGS, temperature())
        self.assertEqual(result, "confirmed")
        self.assertEqual([call[0] for call in transport.calls], ["GET"] * 4 + ["PUT", "GET"])
        self.assertNotIn("sensitive transport detail", output.getvalue())

    def test_unconfirmed_activation_does_bounded_reads_only(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature(), Response(status=204), *[temperature() for _ in range(4)])
        with self.assertRaisesRegex(eight.DiagnosticError, "unconfirmed"):
            eight.activate(client, TARGET, SETTINGS, temperature(), sleep=lambda _: None)
        self.assertEqual([call[0] for call in transport.calls].count("PUT"), 1)
        self.assertEqual(len(transport.calls), 9)

    def test_missing_schedule_configuration_does_not_claim_preservation(self):
        for aggregate in ({}, {"schedules": None, "temperatureSettings": None}, {"schedules": []}):
            with self.subTest(aggregate=aggregate), self.assertRaisesRegex(eight.DiagnosticError, "preservation cannot be verified"):
                eight.config_fingerprint(aggregate)

    def test_changed_schedule_cannot_create_successful_verification(self):
        changed = temperature()
        changed["schedules"][0]["enabled"] = False
        client, _ = self.client(identity(), summary(), SETTINGS, temperature(), Response(status=204), temperature("hotFlash"), changed, SETTINGS)
        with contextlib.redirect_stdout(io.StringIO()):
            eight.activate(client, TARGET, SETTINGS, temperature())
            with self.assertRaisesRegex(eight.DiagnosticError, "Configuration differs"):
                eight.watch(client)
        self.assertFalse((self.local / "native-verification.json").exists())

    def test_manual_cleanup_bodyless_once_and_never_claims_natural_expiry(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature(), Response(status=204), temperature("hotFlash"),
                                        identity(), summary(), SETTINGS, temperature("hotFlash"), Response(status=204), temperature(), SETTINGS)
        with contextlib.redirect_stdout(io.StringIO()):
            eight.activate(client, TARGET, SETTINGS, temperature())
            report = eight.deactivate(client, observed=True)
        self.assertTrue(report["manual_cleanup_verified"])
        self.assertTrue(report["user_observed_app_and_bed"])
        self.assertFalse(report["termination_observed"])
        self.assertFalse(report["natural_expiry_verified"])
        self.assertEqual(report["termination_kind"], "manual")
        cleanup = [call for call in transport.calls if call[1].endswith("/deactivate")]
        self.assertEqual(len(cleanup), 1)
        self.assertNotIn("json", cleanup[0][2])
        self.assertNotIn("data", cleanup[0][2])

    def test_cleanup_timeout_checks_state_without_replay(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature(), Response(status=204), temperature("hotFlash"),
                                        identity(), summary(), SETTINGS, temperature("hotFlash"), requests.Timeout(), temperature(), SETTINGS)
        with contextlib.redirect_stdout(io.StringIO()):
            eight.activate(client, TARGET, SETTINGS, temperature())
            report = eight.deactivate(client)
        self.assertTrue(report["manual_cleanup_verified"])
        self.assertFalse(report["user_observed_app_and_bed"])
        self.assertEqual(len([call for call in transport.calls if call[1].endswith("/deactivate")]), 1)

    def test_cleanup_wrong_target_does_not_send_put(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature())
        eight.save_private(self.local / "activation.json", {"confirmed_at": 1, "target": {**TARGET, "side": "right"}})
        with self.assertRaisesRegex(eight.DiagnosticError, "assignment changed"):
            eight.deactivate(client)
        self.assertTrue(all(call[0] == "GET" for call in transport.calls))

    def test_http408_activation_is_checked_without_replay(self):
        client, transport = self.client(identity(), summary(), SETTINGS, temperature(), Response(status=408), temperature("hotFlash"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(eight.activate(client, TARGET, SETTINGS, temperature()), "confirmed")
        self.assertEqual([call[0] for call in transport.calls].count("PUT"), 1)

    def test_assignment_changed_during_prompt_prevents_activation(self):
        changed = summary()
        changed["households"][0]["sets"][0]["devices"][0]["pairing"]["leftUserId"] = "partner"
        client, transport = self.client(identity(), changed)
        with self.assertRaises(eight.DiagnosticError):
            eight.activate(client, TARGET, SETTINGS, temperature())
        self.assertTrue(all(call[0] == "GET" for call in transport.calls))

    def test_invalid_refresh_expiry_does_not_persist(self):
        client, _ = self.client()
        for expiry in (None, False, -1, float("nan"), float("inf")):
            with self.subTest(expiry=expiry), self.assertRaises(eight.DiagnosticError):
                client._accept_tokens(tokens(expires=expiry))
        self.assertEqual(eight.load_private(client.session_path)["expires_at"], 73000)

    def test_unknown_side_and_away_are_rejected(self):
        for side in ("away", "none", "other", None):
            with self.subTest(side=side), self.assertRaises(eight.DiagnosticError):
                eight.verify_assignment(USER, identity(side), summary())

    def test_solo_requires_both_assignments_and_pairings(self):
        value = summary()
        with self.assertRaises(eight.DiagnosticError):
            eight.verify_assignment(USER, identity("solo"), value)
        device = value["households"][0]["sets"][0]["devices"][0]
        device["assignment"]["rightUserId"] = USER
        device["pairing"]["rightUserId"] = USER
        self.assertEqual(eight.verify_assignment(USER, identity("solo"), value)["side"], "solo")

    def test_private_state_rejects_symlink(self):
        self.local.mkdir()
        victim = Path(self.directory.name) / "victim"
        victim.write_text("untouched")
        link = self.local / "session.json"
        link.symlink_to(victim)
        with self.assertRaises(eight.DiagnosticError):
            eight.save_private(link, {"token": "synthetic"})
        self.assertEqual(victim.read_text(), "untouched")

    def test_response_size_limit(self):
        client, _ = self.client(Response(raw=b"x" * (eight.MAX_BODY + 1)))
        with self.assertRaisesRegex(eight.DiagnosticError, "bounded response"):
            client.request("GET", "/test")


if __name__ == "__main__":
    unittest.main()
