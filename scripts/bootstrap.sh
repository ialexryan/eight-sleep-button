#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .tools
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) archive='arduino-cli_1.5.1_macOS_ARM64.tar.gz' ;;
  Darwin-x86_64) archive='arduino-cli_1.5.1_macOS_64bit.tar.gz' ;;
  *) echo 'This bootstrap script currently supports macOS. Install Arduino CLI 1.5.1 manually on other hosts.' >&2; exit 1 ;;
esac
if [ ! -x .tools/arduino-cli ]; then
  curl -fL "https://github.com/arduino/arduino-cli/releases/download/v1.5.1/$archive" -o ".tools/$archive"
  curl -fL 'https://github.com/arduino/arduino-cli/releases/download/v1.5.1/1.5.1-checksums.txt' -o .tools/arduino-checksums.txt
  (cd .tools && awk -v file="$archive" '$2 == file' arduino-checksums.txt | shasum -a 256 -c -)
  tar -xzf ".tools/$archive" -C .tools arduino-cli
fi
UV_CACHE_DIR="$PWD/.tools/uv-cache" uv venv --python 3.14 .venv
UV_CACHE_DIR="$PWD/.tools/uv-cache" uv pip sync --python .venv/bin/python requirements-lock.txt
.tools/arduino-cli --config-file arduino-cli.yaml core update-index
.tools/arduino-cli --config-file arduino-cli.yaml core install esp32:esp32@3.3.11
.tools/arduino-cli --config-file arduino-cli.yaml lib install M5GFX@0.2.29 M5Unified@0.2.22 ArduinoJson@7.4.3
