#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,DebugLevel=none'
exec .tools/arduino-cli --config-file arduino-cli.yaml compile \
  --fqbn "$FQBN" --warnings all --build-path "$PWD/build/atoms3r" \
  firmware/eight_sleep_button
