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
- Read-only account authentication and current device/side checks succeeded on the intended account. Existing cooling settings were read and left unchanged.

## Required live acceptance checks

- [x] Native activation confirmed in the API, app, and on the intended bed side.
- [ ] Existing finite cycle ends naturally; reported settings and schedule configuration preserved.
- [x] Display/button/base identification and unobtrusive screen feedback observed on the connected board. Optional audio remains off and untested.
- [ ] Physical press invokes cooling with API confirmation.
- [ ] Duplicate presses do not restart the cooling timer.
- [ ] Wi-Fi disconnect/recovery; no delayed activation.
- [ ] Board refresh-token exchange succeeds and survives reset.
- [ ] Power interruption and boot-held button produce no activation.
- [ ] Operation on a USB wall supply with the laptop disconnected.
- [ ] Hours-idle responsiveness.

Physical test with simulated network result: one click and a quick double-click each produced exactly one accepted request/result. The user observed amber progress, green “TEST only,” and return to a dark screen. This validates physical handling and feedback; it does not validate a physical press reaching the bed.

The last two checks require actual elapsed time/physical operation. A successful build or mocked response is not evidence for them.

## API observations

The native feature still uses the legacy `hot-flash-mode` naming. Authentication returns camelCase `userId`; identity comes from `client-api`, while cooling uses `app-api`. The reference's Java client does not implement activation, so its documentation alone is not a live validation result.

Live settings use a duration string in `H:MM:SS` form. Preserve `levelDelta` exactly; do not reinterpret it as degrees or copy an example value. The diagnostic validates assignment and pairing rather than guessing left/right.
