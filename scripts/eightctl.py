#!/usr/bin/env python3
"""Local, deliberately narrow Eight Sleep diagnostic. Never logs secrets."""
from __future__ import annotations

import argparse
from email.utils import parsedate_to_datetime
import getpass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile
import time
from urllib.parse import quote

import requests

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / ".local"
CLIENT_ID = "0894c7f33bb94800a03f1f4df13a4f38"
CLIENT_SECRET = "f0954a3ed5763ba3d06834c73731a32f15f168f47d4f164751275def86db0c76"
AUTH = "https://auth-api.8slp.net/v1/tokens"
APP = "https://app-api.8slp.net/v1"
CLIENT = "https://client-api.8slp.net/v1"
MAX_BODY = 512 * 1024


class DiagnosticError(Exception):
    """Safe to print; never include a server body, URL, or transport exception."""


class ApiError(DiagnosticError):
    def __init__(self, status: int = 0, ambiguous: bool = False, retry_after: int = 0):
        self.status, self.ambiguous, self.retry_after = status, ambiguous, retry_after
        text = f"Server returned HTTP {status}." if status else "Network/TLS request failed."
        if status in (400, 401, 403):
            text += " Verify access or run login again; no password is stored."
        if status == 429:
            text += " Rate limited; wait before trying again."
        if ambiguous:
            text += " The request may have reached the server; it will not be replayed."
        super().__init__(text)


def private_dir(path: Path) -> None:
    if path.is_symlink():
        raise DiagnosticError("Refusing a symlink for the private state directory.")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.chmod(0o700)


def save_private(path: Path, data: dict) -> None:
    private_dir(path.parent)
    if path.is_symlink():
        raise DiagnosticError("Refusing a symlink for a private state file.")
    fd, temporary = tempfile.mkstemp(prefix=".write-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            os.fchmod(stream.fileno(), 0o600)
            json.dump(data, stream, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def load_private(path: Path) -> dict:
    if path.is_symlink():
        raise DiagnosticError("Refusing a symlink for a private state file.")
    try:
        path.chmod(0o600)
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise DiagnosticError("No local session. Run: .venv/bin/python scripts/eightctl.py login") from None
    except (ValueError, OSError):
        raise DiagnosticError("Cannot read private state; run login again.") from None
    if not isinstance(data, dict):
        raise DiagnosticError("Invalid private state; run login again.")
    return data


def require_text(value, label: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 8192:
        raise DiagnosticError(f"Missing or invalid {label} in API response.")
    return value


def fingerprint(data) -> str:
    return hashlib.sha256(json.dumps(data, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def duration_seconds(value) -> int:
    if not isinstance(value, str):
        raise DiagnosticError("Cooling duration is absent; inspect API settings before activation.")
    match = re.fullmatch(r"(\d{1,3}):([0-5]\d):([0-5]\d)(?:\.\d+)?", value)
    if not match:
        raise DiagnosticError("Unrecognized cooling duration; inspect API settings before activation.")
    seconds = int(match[1]) * 3600 + int(match[2]) * 60 + int(match[3])
    if not 1 <= seconds <= 3600:
        raise DiagnosticError("Cooling duration is outside the supported finite test window (1–3600 seconds).")
    return seconds


class EightClient:
    def __init__(self, local: Path = LOCAL, transport=None, clock=time.time):
        self.local, self.clock = local, clock
        self.http = transport or requests.Session()
        self.tokens = None
        self.blocked_until = 0.0

    @property
    def session_path(self):
        return self.local / "session.json"

    def load(self):
        self.tokens = load_private(self.session_path)
        self.blocked_until = float(self.tokens.get("blocked_until", 0))
        return self

    def _exchange(self, method: str, url: str, body=None, token=None):
        if self.clock() < self.blocked_until:
            raise DiagnosticError("A previous service rate limit/backoff is still active; wait before retrying.")
        headers = {"Accept": "application/json", "User-Agent": "eight-sleep-remote/0.1"}
        if token:
            headers["Authorization"] = "Bearer " + token
        arguments = dict(headers=headers, timeout=(5, 12), allow_redirects=False, stream=True)
        if body is not None:
            arguments["json"] = body
        # No json={}, null, or invented temperature payload for a bodyless PUT.
        try:
            with self.http.request(method, url, **arguments) as response:
                status = response.status_code
                if not 200 <= status < 300:
                    retry_after = 0
                    # A server-unavailable response may carry the same retry
                    # instruction as 429. Persist it before reporting an
                    # ambiguous PUT, so read-only confirmation also waits.
                    if status == 429 or (500 <= status < 600 and response.headers.get("Retry-After")):
                        raw = response.headers.get("Retry-After", "60")
                        if raw.isdigit():
                            retry_after = max(60, int(raw))
                        else:
                            try:
                                retry_after = max(60, math.ceil(parsedate_to_datetime(raw).timestamp() - self.clock()))
                            except (TypeError, ValueError, OverflowError):
                                retry_after = 60
                        self.blocked_until = self.clock() + retry_after
                        if self.tokens:
                            self.tokens["blocked_until"] = self.blocked_until
                            save_private(self.session_path, self.tokens)
                    raise ApiError(status, ambiguous=(method == "PUT" and (status == 408 or status >= 500)), retry_after=retry_after)
                payload = bytearray()
                for chunk in response.iter_content(8192):
                    payload.extend(chunk)
                    if len(payload) > MAX_BODY:
                        raise DiagnosticError("API response exceeds the bounded response size.")
                if not payload:
                    return None
                try:
                    return json.loads(payload)
                except (ValueError, UnicodeError):
                    # A successful activation may have a plain-text or empty body.
                    if method == "PUT":
                        return None
                    raise DiagnosticError("API returned an unexpected response format.") from None
        except requests.RequestException:
            raise ApiError(ambiguous=(method == "PUT")) from None

    def _accept_tokens(self, result, previous=None):
        if not isinstance(result, dict):
            raise DiagnosticError("Unexpected token response; authenticate again.")
        access = require_text(result.get("access_token"), "access token")
        refresh = require_text(result.get("refresh_token") or (previous or {}).get("refresh_token"), "refresh token")
        # Live refresh responses may omit userId. Retain the established identity
        # only when the field is absent; an explicitly invalid or changed identity
        # must still fail. Initial password login always requires userId.
        identity = result["userId"] if "userId" in result else (previous or {}).get("user_id")
        user = require_text(identity, "user identity")
        expiry = result.get("expires_in")
        if isinstance(expiry, bool) or not isinstance(expiry, (int, float)) or not math.isfinite(expiry) or expiry <= 0:
            raise DiagnosticError("API returned an invalid token expiry.")
        if previous and user != previous.get("user_id"):
            raise DiagnosticError("Refreshed identity changed; refusing to use this session.")
        updated = {"access_token": access, "refresh_token": refresh, "user_id": user,
                   "expires_at": self.clock() + expiry}
        # Persist rotated credentials before replacing the current in-memory set.
        save_private(self.session_path, updated)
        self.tokens = updated

    def login(self, username: str, password: str):
        result = self._exchange("POST", AUTH, {"client_id": CLIENT_ID, "client_secret": CLIENT_SECRET,
                                "grant_type": "password", "username": username, "password": password})
        self._accept_tokens(result)

    def refresh(self):
        if not self.tokens:
            self.load()
        result = self._exchange("POST", AUTH, {"client_id": CLIENT_ID, "client_secret": CLIENT_SECRET,
                                "grant_type": "refresh_token", "refresh_token": self.tokens["refresh_token"]})
        self._accept_tokens(result, self.tokens)

    def request(self, method: str, path: str, host=APP):
        if not self.tokens:
            self.load()
        if self.clock() + 120 >= self.tokens.get("expires_at", 0):
            self.refresh()
        try:
            return self._exchange(method, host + path, token=self.tokens["access_token"])
        except ApiError as error:
            # Only safe reads retry after an explicit authentication rejection.
            if error.status != 401 or method != "GET":
                raise
            self.refresh()
            return self._exchange(method, host + path, token=self.tokens["access_token"])

    def user_path(self, suffix: str):
        if not self.tokens:
            self.load()
        return "/users/" + quote(self.tokens["user_id"], safe="") + suffix

    def temperature(self):
        return self.request("GET", self.user_path("/temperature/all"))

    def settings(self):
        value = self.request("GET", self.user_path("/temperature/hot-flash-mode"))
        if not isinstance(value, dict) or not isinstance(value.get("enabled"), bool):
            raise DiagnosticError("Unrecognized rapid-cooling settings response.")
        duration_seconds(value.get("hotFlashDuration"))
        if type(value.get("levelDelta")) is not int:
            raise DiagnosticError("Unrecognized rapid-cooling level setting.")
        return {key: value[key] for key in ("enabled", "levelDelta", "hotFlashDuration")}

    def inspect(self):
        identity = self.request("GET", "/users/me", host=CLIENT)
        summary = self.request("GET", "/household/users/" + quote(self.tokens["user_id"], safe="") + "/summary")
        target = verify_assignment(self.tokens["user_id"], identity, summary)
        settings, aggregate = self.settings(), self.temperature()
        state = selected_state(aggregate, target)
        save_private(self.local / "target.json", target)
        report = {"stage": "inspected", "account_verified": True, "assignment_verified": True,
                  "side": target["side"], "specialization": "pod", "state": safe_state(state),
                  "settings": settings, "timestamp": int(self.clock())}
        save_private(self.local / "diagnostic-status.json", report)
        return target, settings, aggregate


def verify_assignment(user_id, identity, summary):
    try:
        user = identity["user"]
        current = user["currentDevice"]
        device_id, side = current["id"], current["side"]
        if user["userId"] != user_id or side not in ("left", "right", "solo"):
            raise ValueError
        require_text(device_id, "current device")
        matches = [device for household in summary["households"] for bed_set in household["sets"]
                   if bed_set["setId"] == summary["currentSet"] for device in bed_set["devices"]
                   if device["deviceId"] == device_id and device["specialization"] == "pod"]
        if len(matches) != 1:
            raise ValueError
        device = matches[0]
        expected_sides = ("left", "right") if side == "solo" else (side,)
        for expected_side in expected_sides:
            key = expected_side + "UserId"
            if device["assignment"].get(key) != user_id or device["pairing"].get(key) != user_id:
                raise ValueError
    except (KeyError, TypeError, ValueError, AttributeError):
        raise DiagnosticError("Cannot verify your current pod and assigned/paired side. No bed settings changed.") from None
    return {"user_id": user_id, "device_id": device_id, "side": side}


def selected_state(aggregate, target):
    try:
        matches = [entry for entry in aggregate["devices"]
                   if entry["device"]["deviceId"] == target["device_id"]
                   and entry["device"]["side"] == target["side"]
                   and entry["device"]["specialization"] == "pod"]
        if len(matches) != 1 or not isinstance(matches[0]["currentState"]["type"], str):
            raise ValueError
        return matches[0]["currentState"]["type"]
    except (KeyError, TypeError, ValueError, AttributeError):
        raise DiagnosticError("Cannot identify current temperature state for the verified pod side.") from None


def safe_state(state):
    known = {"smart", "smart:initial", "smart:bedtime", "smart:final", "alarm", "off", "timeBased", "hotFlash", "nap", "unknown"}
    return state if state in known else "unrecognized"


def config_fingerprint(aggregate):
    if not isinstance(aggregate, dict) or any(not isinstance(aggregate.get(key), list)
                                            for key in ("schedules", "temperatureSettings")):
        raise DiagnosticError("Schedule/temperature configuration is unavailable; preservation cannot be verified.")
    return fingerprint({key: aggregate[key] for key in ("schedules", "temperatureSettings")})


def print_inspection(target, settings, aggregate):
    print(f"Verified your account, pod, and {target['side']} side.")
    print(f"Current state: {safe_state(selected_state(aggregate, target))}.")
    print("Existing rapid-cooling settings: " + json.dumps(settings, sort_keys=True))
    print("Settings have only been read; no bed settings were changed.")


def activate(client, target, settings, aggregate, sleep=time.sleep):
    # Re-verify identity AND assignment after the prompt, then read current settings/state.
    fresh_target, fresh_settings, aggregate = client.inspect()
    if fresh_target != target or fresh_settings != settings:
        raise DiagnosticError("Pod assignment or cooling settings changed during confirmation. Inspect and try again.")
    state = selected_state(aggregate, target)
    if state == "hotFlash":
        print("Rapid Cooling is already active. No activation sent.")
        return "already_active"
    baseline = {"settings": settings, "configuration_hash": config_fingerprint(aggregate),
                "before_state": safe_state(state), "started_at": int(client.clock()),
                "duration_seconds": duration_seconds(settings["hotFlashDuration"]), "target": target}
    save_private(client.local / "activation.json", baseline)
    request_result = "accepted"
    try:
        client.request("PUT", client.user_path("/temperature/hot-flash-mode/activate"))
    except ApiError as error:
        if not error.ambiguous:
            raise
        request_result = "ambiguous"
        print(str(error))
    # At most four safe reads. No retry of the activation PUT, even after a timeout.
    for attempt in range(4):
        if attempt:
            sleep(2)
        try:
            observed = selected_state(client.temperature(), target)
        except DiagnosticError:
            observed = None
        if observed == "hotFlash":
            baseline["confirmed_at"] = int(client.clock())
            save_private(client.local / "activation.json", baseline)
            save_private(client.local / "diagnostic-status.json", {"stage": "activation_confirmed", "side": target["side"],
                         "state": "hotFlash", "request_result": request_result, "timestamp": int(client.clock())})
            print("Confirmed: your pod side reports hotFlash. Check Rapid Cooling in the app and feel the bed.")
            return "confirmed"
    save_private(client.local / "diagnostic-status.json", {"stage": "activation_unconfirmed", "request_result": request_result,
                 "timestamp": int(client.clock())})
    raise DiagnosticError("Activation is unconfirmed. Check the app; no activation retry was sent.")


def watch(client, timeout=None, interval=10, sleep=time.sleep):
    baseline = load_private(client.local / "activation.json")
    if not baseline.get("confirmed_at"):
        raise DiagnosticError("No confirmed activation to watch; inspect the app first.")
    target = baseline["target"]
    limit = timeout or baseline["duration_seconds"] + 120
    deadline = client.clock() + limit
    print("Watching read-only for cooling to end and checking preserved settings/schedules.")
    errors = 0
    while client.clock() <= deadline:
        try:
            aggregate = client.temperature()
            state = selected_state(aggregate, target)
            errors = 0
        except DiagnosticError as error:
            errors += 1
            if errors >= 3 or isinstance(error, ApiError) and error.status in (401, 403, 429):
                raise
            sleep(interval)
            continue
        if state != "hotFlash":
            same_settings = client.settings() == baseline["settings"]
            same_configuration = config_fingerprint(aggregate) == baseline["configuration_hash"]
            report = {"stage": "cooling_ended", "state": safe_state(state), "settings_preserved": same_settings,
                      "schedule_configuration_preserved": same_configuration, "timestamp": int(client.clock()),
                      "elapsed_seconds": int(client.clock()) - baseline["started_at"]}
            save_private(client.local / "diagnostic-status.json", report)
            print(f"Cooling ended; current state: {safe_state(state)}.")
            print(f"Cooling settings preserved: {same_settings}; schedule/temperature configuration preserved: {same_configuration}.")
            if not same_settings or not same_configuration:
                raise DiagnosticError("Configuration differs from the baseline; investigate before enabling the button.")
            verification = {**report, "target_hash": fingerprint(target), "activation_observed": True,
                            "termination_observed": True, "termination_kind": "natural",
                            "natural_expiry_verified": True, "user_observed_app_and_bed": False}
            save_private(client.local / "native-verification.json", verification)
            return report
        sleep(interval)
    raise DiagnosticError("Still hotFlash at the watch deadline; check the app. No settings were changed.")


def confirm_observation(client):
    print("API activation, natural termination, and preserved configuration are verified.")
    print("Confirm only if you also saw Rapid Cooling in the app, felt cooling on your side, and saw normal operation resume.")
    confirmed = sys.stdin.isatty() and input("Type CONFIRM to record those app/bed observations: ") == "CONFIRM"
    verification = load_private(client.local / "native-verification.json")
    verification["user_observed_app_and_bed"] = confirmed
    save_private(client.local / "native-verification.json", verification)
    if confirmed:
        print("Native cooling behavior verified. The board can now be provisioned for button testing.")
    else:
        print("API evidence saved; app/bed observations are still required before provisioning.")


def deactivate(client, observed=False, sleep=time.sleep):
    """Expedited, explicitly requested cleanup; never claims natural expiry."""
    baseline = load_private(client.local / "activation.json")
    if not baseline.get("confirmed_at"):
        raise DiagnosticError("No confirmed test activation to clean up; inspect the app first.")
    target, settings, aggregate = client.inspect()
    if target != baseline["target"]:
        raise DiagnosticError("Pod assignment changed since activation; refusing to control a different side.")
    state = selected_state(aggregate, target)
    sent = state == "hotFlash"
    if sent:
        try:
            client.request("PUT", client.user_path("/temperature/hot-flash-mode/deactivate"))
        except ApiError as error:
            if not error.ambiguous:
                raise
            print(str(error))
        for attempt in range(4):
            if attempt:
                sleep(2)
            try:
                aggregate = client.temperature()
                state = selected_state(aggregate, target)
            except DiagnosticError:
                state = "hotFlash"
            if state != "hotFlash":
                break
        else:
            raise DiagnosticError("Cleanup is unconfirmed. Check the app; no deactivation retry was sent.")
    settings = client.settings()
    same_settings = settings == baseline["settings"]
    same_configuration = config_fingerprint(aggregate) == baseline["configuration_hash"]
    report = {"stage": "cooling_deactivated", "state": safe_state(state),
              "settings_preserved": same_settings, "schedule_configuration_preserved": same_configuration,
              "timestamp": int(client.clock()), "target_hash": fingerprint(target),
              "activation_observed": True, "termination_observed": False,
              "termination_kind": "manual" if sent else "already_inactive",
              "natural_expiry_verified": False, "manual_cleanup_verified": True,
              "deactivation_request_sent": sent, "user_observed_app_and_bed": bool(observed)}
    save_private(client.local / "diagnostic-status.json", report)
    if not same_settings or not same_configuration:
        raise DiagnosticError("Cooling is inactive but configuration differs from the baseline; investigate before provisioning.")
    save_private(client.local / "native-verification.json", report)
    print("Confirmed: Rapid Cooling is inactive and original cooling settings/schedule configuration are preserved.")
    print("This expedited test does not verify natural timer expiry.")
    if observed:
        print("Your reported app/bed observations were recorded. The board can now be provisioned.")
    else:
        print("App/bed observations are not yet recorded; use --observed only after actually checking both.")
    return report


def provision(client, port, audio=False):
    try:
        import serial
    except ImportError:
        raise DiagnosticError("Install the project's pinned Python dependencies for serial provisioning.") from None
    target, settings, aggregate = client.inspect()
    print_inspection(target, settings, aggregate)
    try:
        verification = load_private(client.local / "native-verification.json")
    except DiagnosticError:
        raise DiagnosticError("Complete an observed activation plus watch or manual deactivate test before provisioning the board.") from None
    cleanup_verified = (verification.get("termination_observed") is True
                        or verification.get("manual_cleanup_verified") is True)
    if (verification.get("target_hash") != fingerprint(target) or not cleanup_verified
            or not all(verification.get(key) is True for key in (
                "activation_observed", "user_observed_app_and_bed",
                "settings_preserved", "schedule_configuration_preserved"))):
        raise DiagnosticError("Cooling activation/cleanup and app/bed observations for this side are not verified. Complete watch or deactivate first.")
    if not sys.stdin.isatty():
        raise DiagnosticError("Provisioning requires a local interactive terminal.")
    ssid = getpass.getpass("2.4 GHz Wi-Fi network name (hidden): ")
    password = getpass.getpass("Wi-Fi password (hidden): ")
    packet = {"op": "provision", "ssid": ssid, "password": password,
              "refresh_token": client.tokens["refresh_token"], **target, "audio": audio}
    if not ssid or len(ssid.encode()) > 32 or len(password.encode()) > 63:
        raise DiagnosticError("Invalid Wi-Fi name/password length.")
    try:
        with serial.Serial(port, 115200, timeout=1, write_timeout=5) as device:
            time.sleep(2)
            device.reset_input_buffer()
            wire = (json.dumps(packet, separators=(",", ":")) + "\n").encode()
            if len(wire) > 8192:
                raise DiagnosticError("Provisioning record exceeds the device's supported size.")
            # Pace USB bursts as well as sizing the firmware's receive queue.
            for offset in range(0, len(wire), 128):
                device.write(wire[offset:offset + 128])
                time.sleep(0.02)
            device.flush()
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                # Never echo any serial line; a device might echo provisioning data.
                line = device.readline(16384)
                try:
                    response = json.loads(line)
                except (ValueError, UnicodeError):
                    continue
                if isinstance(response, dict) and (response.get("event") == "provisioned" or
                        response.get("op") == "provision" and response.get("ok") is True):
                    # The board now owns refresh-token rotation. Keeping a second
                    # active copy on the laptop risks invalidating that credential.
                    client.session_path.unlink(missing_ok=True)
                    client.tokens = None
                    print("Device acknowledged provisioning. No account password was sent or saved.")
                    print("Local tokens removed; the board owns this session. Run login for future laptop diagnostics.")
                    return
                if isinstance(response, dict) and response.get("event") in (
                        "hold_button_to_configure", "invalid_config", "storage_error", "busy"):
                    raise DiagnosticError("Device did not accept provisioning. Hold its face for two seconds and retry; check status if it persists.")
                if isinstance(response, dict) and response.get("op") == "provision" and response.get("ok") is False:
                    raise DiagnosticError("Device rejected provisioning. Check firmware diagnostics.")
    except (serial.SerialException, OSError):
        raise DiagnosticError("Serial provisioning failed; check the selected data cable and port.") from None
    raise DiagnosticError("No provisioning acknowledgement received; secret serial data was not echoed.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("login", "inspect", "refresh"):
        sub.add_parser(name)
    activating = sub.add_parser("activate")
    activating.add_argument("--watch", action="store_true", help="Also wait for natural expiry and record observations")
    cleanup = sub.add_parser("deactivate", help="End the confirmed test cycle now; does not verify natural expiry")
    cleanup.add_argument("--observed", action="store_true", help="Record that you actually observed Rapid Cooling in the app and felt cooling on your side")
    watching = sub.add_parser("watch")
    watching.add_argument("--timeout", type=int, help="Read-only watch duration in seconds")
    provisioning = sub.add_parser("provision")
    provisioning.add_argument("--port", required=True)
    provisioning.add_argument("--audio", action="store_true", help="Enable optional quiet confirmation tone")
    args = parser.parse_args(argv)
    client = EightClient()
    try:
        if args.command == "login":
            if not sys.stdin.isatty():
                raise DiagnosticError("Login requires a local interactive terminal; do not put credentials in command arguments.")
            username = getpass.getpass("Eight Sleep email (hidden): ")
            password = getpass.getpass("Eight Sleep password (hidden): ")
            client.login(username, password)
            del username, password
            print("Login succeeded. Only tokens were saved locally with owner-only permissions.")
            print_inspection(*client.inspect())
        elif args.command == "inspect":
            print_inspection(*client.inspect())
        elif args.command == "refresh":
            client.refresh()
            print("Refresh succeeded; rotated refresh credentials saved securely.")
        elif args.command == "activate":
            target, settings, aggregate = client.inspect()
            print_inspection(target, settings, aggregate)
            if not sys.stdin.isatty():
                raise DiagnosticError("A live test requires a local interactive terminal.")
            print("Open the Eight Sleep app to observe your side. This sends one real Rapid Cooling request.")
            if input("Type COOL to activate your existing finite cooling cycle: ") != "COOL":
                print("Cancelled; no activation sent.")
                return 0
            result = activate(client, target, settings, aggregate)
            if result == "confirmed" and args.watch:
                watch(client)
                confirm_observation(client)
            elif result == "confirmed":
                print("Activation verified. After checking app/bed, use deactivate --observed for short-test cleanup, or watch to verify natural expiry.")
        elif args.command == "watch":
            if args.timeout is not None and not 1 <= args.timeout <= 7200:
                raise DiagnosticError("Watch timeout must be between 1 and 7200 seconds.")
            watch(client, args.timeout)
            confirm_observation(client)
        elif args.command == "deactivate":
            deactivate(client, args.observed)
        elif args.command == "provision":
            provision(client, args.port, args.audio)
        return 0
    except KeyboardInterrupt:
        print("\nStopped locally. Any active cooling cycle remains under Eight Sleep's control.", file=sys.stderr)
        return 130
    except DiagnosticError as error:
        print("Error: " + str(error), file=sys.stderr)
        try:
            save_private(client.local / "diagnostic-status.json", {"stage": "error", "message": str(error), "timestamp": int(time.time())})
        except (OSError, DiagnosticError):
            pass
        return 1
    except (OSError, KeyError, TypeError, ValueError, AttributeError, OverflowError):
        message = "Unexpected local state or response. No secret details were printed; inspect and retry manually."
        print("Error: " + message, file=sys.stderr)
        try:
            save_private(client.local / "diagnostic-status.json", {"stage": "error", "message": message, "timestamp": int(time.time())})
        except (OSError, DiagnosticError):
            pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
