# Eight Sleep Button

A standalone, quiet M5Stack nightstand button for Eight Sleep's finite **Rapid Cooling** cycle (formerly **Hot Flash Mode**). USB power + 2.4 GHz Wi-Fi + Eight Sleep's cloud are sufficient after setup. No phone, laptop, Home Assistant, broker, or server is required at runtime.

Uses an unofficial API. This project is unaffiliated with Eight Sleep. It sends the native bodyless `PUT /v1/users/{userId}/temperature/hot-flash-mode/activate`; it never implements cooling by changing a permanent setpoint, schedule, alarm, or Autopilot setting.

**Validation:** built and flashed on an AtomS3R, with live Rapid Cooling activation and native timer-reset API checks. See [validation](docs/validation.md) for observed hardware results and remaining wall-power/overnight checks.

## Hardware

- Primary build: **AtomS3R C126**, ESP32-S3-PICO-1, 8 MB flash, 8 MB OPI PSRAM.
- Optional **Atomic Voice Base A149**. Quiet confirmation tones are opt-in. The microphone is disabled and never sampled. Removing the base does not affect cooling.
- **AtomS3 Lite C124** feedback backend is prepared. It needs a separate no-PSRAM build and hardware validation before use; do not flash the S3R build onto a Lite.
- The screen face is a physical button on GPIO41, not a touchscreen. The reset/download button is separate.

M5GFX supports both the older GC9107 and the May 2026 ST7735 panel revisions. Runtime diagnostics report the detected panel. Hardware feedback, button logic, and the API client are separate modules.

The device advertises the Wi-Fi hostname **`eight-sleep-button`**. USB `status`
reports the active interface hostname. The name takes effect after flashing this
firmware and reconnecting to Wi-Fi; a router's saved device label may differ.

## Build and flash

Prerequisites on macOS: Xcode command-line tools, `uv`, and Internet access. The bootstrap installs one project-local Arduino CLI stack, with all generated/downloaded files ignored by Git.

```sh
scripts/bootstrap.sh
scripts/test.sh
scripts/build.sh
ls /dev/cu.usbmodem*
scripts/flash.sh /dev/cu.usbmodem1101
.venv/bin/python scripts/boardctl.py status --port /dev/cu.usbmodem1101
```

Pinned versions: Arduino CLI **1.5.1**, Arduino-ESP32 **3.3.11**, M5Unified **0.2.22**, M5GFX **0.2.29**, ArduinoJson **7.4.3**. Python dependencies are in `requirements-lock.txt`. USB port names can change after reset/reconnection. Close serial monitors before flashing/provisioning.

## Account setup and native API test

Run these commands in your own interactive Terminal. Email, account password, Wi-Fi name, and Wi-Fi password are hidden during entry. Never put them in command arguments, chat, source, or an example file.

```sh
.venv/bin/python scripts/eightctl.py login
.venv/bin/python scripts/eightctl.py inspect
.venv/bin/python scripts/eightctl.py activate
```

`login` uses the account password once and retains only tokens in ignored `.local/session.json` with owner-only permissions. `inspect` verifies the authenticated user, current pod, assignment and pairing, and reads the existing cooling settings without changing them.

`activate` waits for `COOL`, rechecks the target and settings, sends **one** activation, and requires the selected device's `currentState.type` to become `hotFlash`. It returns after confirmation, without waiting through the cooling timer. Keep the app available for this initial validation and verify that your side cools.

For an expedited setup, after observing Rapid Cooling in the app and on the bed, end the test using the native deactivate endpoint:

```sh
.venv/bin/python scripts/eightctl.py deactivate --observed
```

This verifies manual cleanup and preserved cooling settings/schedule configuration. It **does not verify natural timer expiry**. The `--observed` flag records your actual app/bed observations; use it only after those checks.

To verify natural expiry instead, observe the running cycle with:

```sh
.venv/bin/python scripts/eightctl.py watch
```

`watch` checks for the cycle leaving `hotFlash` and unchanged reported schedule/temperature configuration, then prompts for app/bed observations. It refuses to claim preservation if required fields are missing. `activate --watch` combines activation and this longer check.

After verified activation and either manual cleanup or natural completion, provision Wi-Fi and the refresh token over USB:

```sh
.venv/bin/python scripts/eightctl.py provision --port /dev/cu.usbmodem1101
# Optional quiet tones (omit --audio for silence):
.venv/bin/python scripts/eightctl.py provision --port /dev/cu.usbmodem1101 --audio
```

For a device that is already configured, hold its face for two seconds before provisioning; this opens a 60-second USB configuration window. Provisioning restarts the board without activating cooling. The board owns refresh-token rotation afterward, and the laptop deletes its local token copy on acknowledgement. Use a fresh `login` for future laptop diagnostics. Never run two refresh clients against the same token set.

## Use

- **Short press and release:** activate native Rapid Cooling on the verified account's side. If already active, reset its timer with one native deactivate, a read confirming it stopped, then one native activation. The existing configured duration and cooling level are preserved.
- **Long hold (1.5 seconds):** brief local diagnostics and a USB setup window; no bed change.
- Screen/backlight stays off when idle. Amber “Sending” means request in progress. Green **“Rapid Cooling”** with **“Started”** means a new cycle was confirmed; **“Timer reset”** means a restart with a later expiration was confirmed. Red means failure. “Check app” means the result is uncertain; do not assume a timeout means the request failed.
- A press during a request is discarded. Offline presses are discarded. No press is queued for later reconnection.
- Boot, reset, reconnection, flashing, and a button held during boot never activate cooling.

A new deliberate press can reset an active cycle. Button bounce and presses during
a request are still discarded. A restart has two writes: if cooling stops but the
new activation fails, it can remain off. Check the app after uncertain feedback;
the firmware never queues a delayed restart or automatically repeats either write.

The S3R uses large semibold text with native grayscale antialiasing and high-contrast
colored lettering on black. “Rapid Cooling” fills two lines, with a larger result
caption below. The backlight remains at 8/255 and turns off after feedback. The
licensed font and reproducible generator are documented in [display fonts](assets/fonts/README.md).

## Security and maintenance

- Certificate-verified HTTPS uses the ESP-IDF root certificate bundle and a synchronized clock. There is no insecure TLS fallback.
- Wi-Fi credentials and the refresh token live in an atomic NVS record on the board. The account password is never sent to it. Access tokens remain in RAM. Rotated refresh tokens are persisted before use.
- NVS is **not encrypted** in this development setup. Physical access can reveal stored credentials. No security eFuses are burned; ordinary recovery remains possible.
- `.local/` contains sensitive setup state and must remain private. Git ignores it, firmware binaries, logs, and downloaded toolchains. Firmware images themselves contain no personal credentials.
- Invalid refresh credentials require running local setup again. Temporary failures use bounded backoff; there is no password-login loop. Read-only 401 recovery is bounded; activation PUTs are never replayed automatically.
- Rebuild/flash when an API or certificate trust change requires it. There is no remotely exposed configuration server or OTA update service.

## Diagnostics and recovery

```sh
.venv/bin/python scripts/boardctl.py status
.venv/bin/python scripts/boardctl.py monitor --seconds 60
.venv/bin/python scripts/boardctl.py feedback
# Five-second visual previews; no bed command, tone, or change to button mode:
.venv/bin/python scripts/boardctl.py feedback --state started
.venv/bin/python scripts/boardctl.py feedback --state reset
.venv/bin/python scripts/boardctl.py feedback --state busy
.venv/bin/python scripts/boardctl.py feedback --state failure
```

For the USB transfer regression (read-only, no secrets or bed commands):

```sh
.venv/bin/python tests/usb_transfer_test.py --port /dev/cu.usbmodem1101
```

For a simulated physical-button test, long-hold first if configured, then run `boardctl.py test-on`. Physical short presses exercise debounce/overlap and display a result marked “TEST only”; **no cloud requests occur in this mode**. Turn it off with a long hold followed by `boardctl.py test-off`, or reset the device. Do not count this as a live cooling test.

`boardctl.py disconnect` (after a long hold) tests Wi-Fi recovery without issuing a bed command. `boardctl.py erase` (after a long hold, plus typed `ERASE`) removes the board's configuration. Reflashing normally preserves NVS credentials.

If USB enumeration fails, try a known data-capable cable. For the AtomS3R, hold the **reset/download button** about two seconds until its internal green LED lights, then release to enter download mode. Re-list serial ports and retry flashing. A blank screen during normal idle is intentional; use the face long hold or `feedback` to check it.

If “Setup needed” appears, provision the board. If Wi-Fi is unavailable, check 2.4 GHz coverage and password. If time is unsynchronized, allow NTP through the network. If credentials are rejected, log in and provision again. If a request is uncertain, check the app before another press; the firmware checks existing cooling state before any further activation.

For the narrow timer-reset experiment (after local `login`),
`scripts/retrigger_probe.py` reads timing by default. `--activate` deliberately sends
one activation while already active; `--restart` tests native off-then-on with
readback. These are live tests on the previously verified side. Both flags require
an active cycle and neither retries a write or changes settings. A normal
`eightctl.py activate` remains a non-resetting setup diagnostic.

## References

- [Unofficial API specification, reviewed revision](https://github.com/j03wang/openhab-eightsleep/blob/9ea84513719ff68558d0bb87a2090451149d5921/docs/eightsleep-api.md)
- [AtomS3R hardware and download mode](https://docs.m5stack.com/en/core/AtomS3R)
- [Atomic Voice Base](https://docs.m5stack.com/en/atom/Atomic%20Echo%20Base)
- [M5Unified 0.2.22](https://github.com/m5stack/M5Unified/tree/0.2.22), [M5GFX 0.2.29](https://github.com/m5stack/M5GFX/tree/0.2.29)
