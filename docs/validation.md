# Validation record

Keep simulated, compiled, and observed results separate. No account identifiers, tokens, Wi-Fi details, or raw account responses belong in this public record.

## Development environment and board

- macOS on Apple Silicon; project-local Arduino CLI 1.5.1.
- USB serial enumeration confirmed a data cable.
- esptool identified ESP32-S3-PICO-1 (LGA56), silicon revision v0.2, 8 MB flash (XMC), and embedded 8 MB PSRAM.
- Flashed firmware and verified flash hash. Runtime reports **ST7735**, an attached Voice Base codec, microphone disabled, and zero startup activations.
- The connected board is the S3R. A Lite was ordered; no Lite hardware test is claimed.

## Checks completed

- Full S3R firmware build with the pinned dependencies.
- Native button logic: boot-held suppression, debounce, one short-press event, long-press suppression, busy-press discard, and millis rollover.
- Mocked laptop API tests: identity/side mismatch, missing schema, bodyless activation, already-active suppression, ambiguous timeout/408 confirmation, token refresh/rotation, 401 recovery, rate limits, natural termination, and configuration preservation.
- Native firmware API client tests use the real C++ client with mocked ESP32 HTTP/Wi-Fi/time and real ArduinoJson. They exercise token rotation, backoff, stale presses, timeout confirmation, response limits, and exactly one activation write.
- 64 firmware API mock scenarios and 57 laptop/diagnostic mock tests pass.
- Actual USB regression: a 7 KB non-secret status burst is accepted; oversized input is rejected and parsing recovers. This caught and fixed the core's 256-byte default receive-buffer limitation before successful provisioning.
- Read-only account authentication and current device/side checks succeeded on the intended account. Existing cooling settings were read and left unchanged.

## Required live acceptance checks

- [x] Native activation confirmed in the API, app, and on the intended bed side.
- [ ] Existing finite cycle ends naturally; reported settings and schedule configuration preserved.
- [x] Display/button/base identification and unobtrusive screen feedback observed on the connected board. Optional audio remains off and untested.
- [x] Physical press invokes cooling with API confirmation. USB reported `Confirmed` in live mode; the user saw Rapid Cooling in the app and felt their side respond.
- [x] Initial firmware's already-active behavior: subsequent presses returned `AlreadyActive`; the user confirmed the countdown did not restart. This has since been superseded by the requested intentional timer-reset behavior below.
- [x] Wi-Fi disconnect/recovery; no delayed activation. Forced disconnection/reconnection on the real board restored Wi-Fi and preserved the accepted/completed request counts.
- [x] Board refresh-token exchange succeeds across software reset/reflash using saved credentials. Physical wall-power cold-start acceptance remains separate.
- [ ] Power interruption and boot-held button produce no activation.
- [ ] Operation on a USB wall supply with the laptop disconnected.
- [ ] Hours-idle responsiveness.

Physical test with simulated network result: one click and a quick double-click each produced exactly one accepted request/result. The user observed amber progress, green “TEST only,” and return to a dark screen. This validates physical handling and feedback; it does not validate a physical press reaching the bed.

Unconfigured cold-start test: the user unplugged/reconnected USB while holding the face, then released it. The screen stayed dark; runtime reported zero accepted/completed presses after boot. Provisioned cold-start acceptance is separate.

The user requested an expedited test instead of waiting through the full native cycle. Natural expiry remains unverified; manual test cleanup must not be described as natural expiry.

Manual cleanup succeeded through the native deactivate endpoint. The API reported return to the original normal operating state and unchanged cooling settings, schedules, and temperature configuration.

Provisioning succeeded after the USB buffer fix. Runtime independently confirmed Wi-Fi, synchronized time, and successful refresh-token authentication over verified HTTPS. No account password was sent to the board. The laptop token copy was removed after transfer.

The user deferred the wall-power check while refining feedback. Success feedback now spells out “Rapid Cooling” on two readable lines and distinguishes “Started” from “Already active.” The revised firmware compiled, passed the existing tests, was flashed, and rebooted/authenticated with zero startup requests. Visual review of the revised wording is separate from the earlier physical screen check.

Typography revision: replaced the scaled default bitmap font and decorative icon
with Barlow Semi Condensed SemiBold grayscale VLW fonts. Title size is 41 pixels
(38 for “Sending”), captions 21 pixels; actual ink bounds are smaller than those
nominal font sizes. The widest lines are 121–123 pixels on the 128-pixel display.
The generator's layout preview and an independent review verified bounds and
font format. The S3R build and existing safety/API tests passed. Font regeneration
matches the checked-in data. The flashed board rebooted, synchronized time, and
authenticated with zero accepted/completed presses. Backlight stays at 8/255;
display ink has higher contrast, and Lite LED behavior is unchanged. The user
observed the actual screen and confirmed that the revised text is much easier to
read with nothing cut off. Overnight/dark-room use remains separate.
Six USB visual previews were acknowledged by the real board; afterward it remained
in live mode, authenticated and idle, with zero accepted/completed presses. These
previews exercise font rendering without sending a cooling request.

The last two checks require actual elapsed time/physical operation. A successful build or mocked response is not evidence for them.

## API observations

### Timer-reset experiment

The user subsequently requested that another deliberate press reset an active
cycle's expiration. A live, bodyless activation while already active returned
**HTTP 409 Conflict**. Readback showed unchanged `currentState.started`, `until`,
and `instance.timestamp`. The earlier duplicate-press test had skipped that write
in firmware; it did not establish how the server handles duplicate activation.

A single native deactivate then confirmed normal-state readback, followed by one
native activate, produced a fresh `hotFlash` start and a later expiration. The
new start-to-expiration span matched the existing configured duration. Three
readbacks agreed. Cooling settings and schedule/temperature configuration were
unchanged. These are laptop-side live API observations; updated firmware and
physical-button acceptance are recorded separately when completed.
The user independently confirmed that the app showed a fresh countdown after
the off-then-on experiment.

The diagnostic defaults to read-only and writes only with explicit `--activate`
or `--restart`. Mocked probe tests cover narrow write ordering, target
checks, rejected/ambiguous requests, stale windows, timing output and preservation
checks. No automatic mutation retries or settings writes were introduced.

Firmware 0.2.0 implements the off/read/on sequence for an intentional press while
active. Success requires later start/expiry, the original cycle span within one
second, and a start near the activation request. Missing timing prevents the
restart. An incomplete restart reports “Check app” and cannot queue a delayed
activation. Native tests also cover stale presses, partial failures, unchanged
expiry, mismatched sides, invalid timing, and Retry-After on rejected/service
responses. Server backoff applies to confirmation reads as well as writes.

The full pinned S3R build and all automated checks passed. Firmware 0.2.0 was
flashed and rebooted with zero accepted/completed presses, live mode enabled,
Wi-Fi/time ready, and successful account authentication. The station interface
reports hostname `eight-sleep-button`. Physical restart-button observations are
separate from this startup check.

### Existing observations

The native feature still uses the legacy `hot-flash-mode` naming. Authentication returns camelCase `userId`; identity comes from `client-api`, while cooling uses `app-api`. The reference's Java client does not implement activation, so its documentation alone is not a live validation result.

Live settings use a duration string in `H:MM:SS` form. Preserve `levelDelta` exactly; do not reinterpret it as degrees or copy an example value. The diagnostic validates assignment and pairing rather than guessing left/right.

**Observed refresh quirk:** this account's refresh response omits `userId`, despite the reference describing the same shape as password login. Retain the setup-verified user ID when refresh omits it; reject a conflicting supplied ID. The firmware still verifies live user/device/side before every activation. Token expiry accepts a numeric JSON integer or decimal; strings and booleans are rejected.
