#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
JSON_INCLUDE="$PWD/.arduino/user/libraries/ArduinoJson/src"
if [ ! -f "$JSON_INCLUDE/ArduinoJson.h" ]; then
  echo "Install the pinned Arduino dependencies first (scripts/bootstrap.sh)." >&2
  exit 1
fi
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/eight-sleep-api-tests.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -Wno-missing-field-initializers -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 \
  -I tests/firmware_api/stubs -I "$JSON_INCLUDE" -I firmware/eight_sleep_button \
  firmware/eight_sleep_button/EightSleepClient.cpp tests/firmware_api/client_test.cpp \
  -o "$TEST_BUILD/client_test"
"$TEST_BUILD/client_test"
