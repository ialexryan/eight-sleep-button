"""Hardware regression for USB provisioning frame sizes; sends no secrets/writes."""
import argparse
import json
import time
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
args = parser.parse_args()


def status_reply(device):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            value = json.loads(device.readline(8192))
        except (ValueError, UnicodeError):
            continue
        if isinstance(value, dict) and value.get("event") == "status":
            return True
    return False


with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=2) as device:
    device.reset_input_buffer()
    wire = (json.dumps({"op": "status", "padding": "x" * 7000}) + "\n").encode()
    device.write(wire)
    assert status_reply(device), "Large bounded USB record was dropped"
    # An oversized line must be ignored and framing must recover at its newline.
    oversized = (json.dumps({"op": "status", "padding": "x" * 8500}) + "\n").encode()
    for offset in range(0, len(oversized), 128):
        device.write(oversized[offset:offset + 128])
        time.sleep(0.01)
    assert not status_reply(device), "Oversized USB record was not rejected"
    device.write(b'{"op":"status"}\n')
    assert status_reply(device), "USB parser did not recover after oversized input"
print("PASS: 7 KB USB burst, oversized rejection, and framing recovery on hardware")
