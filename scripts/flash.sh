#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -ne 1 ]; then
  echo 'Usage: scripts/flash.sh /dev/cu.usbmodemXXXX' >&2
  exit 2
fi
FQBN='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,DebugLevel=none'
exec .tools/arduino-cli --config-file arduino-cli.yaml upload \
  --fqbn "$FQBN" --port "$1" --input-dir build/atoms3r firmware/eight_sleep_button
