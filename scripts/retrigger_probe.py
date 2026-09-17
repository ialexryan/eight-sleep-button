#!/usr/bin/env python3
"""Narrow live experiment: read an active cycle, optionally send ONE activation.

Only --restart deactivates first. Never change settings or automatically retry a PUT. Output contains
only the selected mode, valid timing fields and request outcomes, never raw bodies.
"""
import argparse
from datetime import datetime
import json
import time

from eightctl import (ApiError, DiagnosticError, EightClient, config_fingerprint,
                      duration_seconds, fingerprint, load_private, safe_state,
                      save_private, selected_state)


def timing(aggregate, target):
    state = selected_state(aggregate, target)
    device = next(item for item in aggregate["devices"]
                  if item["device"]["deviceId"] == target["device_id"]
                  and item["device"]["side"] == target["side"]
                  and item["device"]["specialization"] == "pod")
    result = {"state": safe_state(state), "timestamps": {}}
    # Only known timing keys may be emitted; arbitrary strings/IDs are omitted.
    fields = ("started", "until", "timestamp", "startTime", "endTime", "expiresAt")
    sources = {"currentState": device["currentState"],
               "currentState.instance": device["currentState"].get("instance"),
               "overrideLevels.hotFlash": device.get("overrideLevels", {}).get("hotFlash")}
    for prefix, source in sources.items():
        if not isinstance(source, dict):
            continue
        for key in fields:
            value = source.get(key)
            if not isinstance(value, str) or len(value) > 40:
                continue
            try:
                stamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
            except ValueError:
                continue
            if stamp.tzinfo is not None:
                result["timestamps"][prefix + "." + key] = stamp.isoformat()
    return result


def probe(client, send=False, sleep=time.sleep, restart=False):
    started_at = client.clock()
    verified = load_private(client.local / "native-verification.json")
    target, settings, before = client.inspect()
    if fingerprint(target) != verified.get("target_hash"):
        raise DiagnosticError("Target differs from the previously verified button side; no activation sent.")
    initial = timing(before, target)
    record = {"before": initial, "duration_seconds": duration_seconds(settings["hotFlashDuration"]),
              "request_sent": False, "observations": []}
    print(json.dumps(record), flush=True)
    if not send and not restart:
        return record
    if initial["state"] != "hotFlash":
        raise DiagnosticError("Rapid Cooling is no longer active; no activation sent. Coordinate a fresh test.")
    baseline = config_fingerprint(before)
    if restart:
        record["strategy"] = "deactivate_then_activate"
        record["deactivation_result"] = "unknown"
        save_private(client.local / "retrigger-probe.json", record)
        try:
            client.request("PUT", client.user_path("/temperature/hot-flash-mode/deactivate"))
            record["deactivation_result"] = "accepted"
        except ApiError as error:
            record["deactivation_result"] = "ambiguous" if error.ambiguous else "rejected"
            save_private(client.local / "retrigger-probe.json", record)
            if not error.ambiguous:
                raise
        # An ambiguous deactivation is only allowed to proceed after a safe read
        # proves it is off. Never send another deactivation.
        target_now, settings_now, inactive = client.inspect()
        if target_now != target or settings_now != settings or config_fingerprint(inactive) != baseline:
            raise DiagnosticError("Target or settings changed after deactivation; activation cancelled.")
        state = safe_state(selected_state(inactive, target))
        if state in ("hotFlash", "unknown", "unrecognized"):
            raise DiagnosticError("Deactivation unconfirmed; activation cancelled. Check the app.")
        record["deactivation_confirmed"] = True
        print(json.dumps({"deactivation_confirmed": True, "state": state}), flush=True)
    if client.clock() - started_at > 20:
        raise DiagnosticError("Experiment exceeded its short request window; activation cancelled. Check the app.")
    record["request_at"] = client.clock()
    record["request_sent"] = True
    record["request_result"] = "unknown"
    # Persist intent before the write, so an interrupted run is not mistaken for
    # a request that was never sent. --activate is an explicit experiment only.
    save_private(client.local / "retrigger-probe.json", record)
    try:
        client.request("PUT", client.user_path("/temperature/hot-flash-mode/activate"))
        record["request_result"] = "accepted"
    except ApiError as error:
        record["request_result"] = "ambiguous" if error.ambiguous else "rejected"
        record["http_status"] = error.status
        save_private(client.local / "retrigger-probe.json", record)
        if not error.ambiguous:
            raise
    save_private(client.local / "retrigger-probe.json", record)
    # Read snapshots only. An unchanged active state does NOT prove timer reset.
    for attempt in range(3):
        if attempt:
            sleep(2)
        after = client.temperature()
        observation = timing(after, target)
        observation["configuration_preserved"] = config_fingerprint(after) == baseline
        record["observations"].append(observation)
        save_private(client.local / "retrigger-probe.json", record)
        print(json.dumps({"request_result": record["request_result"], "after": observation}), flush=True)
    record["settings_preserved"] = client.settings() == settings
    save_private(client.local / "retrigger-probe.json", record)
    print(json.dumps({"settings_preserved": record["settings_preserved"]}), flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--activate", action="store_true", help="Send one real activation while cooling is already active")
    actions.add_argument("--restart", action="store_true", help="Deactivate once, verify off, then activate once")
    args = parser.parse_args()
    try:
        probe(EightClient(), send=args.activate, restart=args.restart)
    except DiagnosticError as error:
        raise SystemExit(str(error)) from None
    except (OSError, ValueError, TypeError, KeyError, AttributeError, StopIteration):
        raise SystemExit("Unexpected local state or response; no secret details printed. Do not retry a possibly sent request.") from None


if __name__ == "__main__":
    main()
