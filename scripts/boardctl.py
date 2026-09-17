#!/usr/bin/env python3
"""Non-secret USB diagnostics. No command can activate the bed."""
import argparse
import json
import time

import serial

FIELDS = {"event", "firmware", "configured", "wifi", "time_synced", "busy", "test_mode",
          "presses", "completed", "uptime_s", "hardware", "free_heap", "api", "code",
          "simulated", "activation", "hostname"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("status", "monitor", "feedback", "test-on", "test-off", "disconnect", "erase"))
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--state", choices=("started", "reset", "busy", "failure", "diagnostics"),
                        help="For feedback only: five-second visual preview, without activating the bed")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error("seconds must be 1–3600")
    if args.state and args.command != "feedback":
        parser.error("--state is only valid with feedback")
    if args.command == "erase" and input("Type ERASE to remove credentials from the board: ") != "ERASE":
        return
    with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=2) as device:
        if args.command != "monitor":
            packet = {"op": args.command}
            if args.command == "feedback" and args.state:
                packet["state"] = args.state
            if args.command in ("test-on", "test-off"):
                packet = {"op": "test_mode", "enabled": args.command == "test-on"}
            device.write((json.dumps(packet) + "\n").encode())
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = device.readline(8192)
            try:
                item = json.loads(line)
            except (ValueError, UnicodeError):
                continue
            if isinstance(item, dict) and "event" in item:
                # Discard all unknown fields, including any echoed provisioning payload.
                output = {key: value for key, value in item.items() if key in FIELDS
                          and isinstance(value, (str, bool, int))
                          and (not isinstance(value, str) or len(value) <= 300)}
                print(json.dumps(output), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (serial.SerialException, OSError):
        raise SystemExit("USB unavailable. Check port/data cable and close other serial monitors.") from None
