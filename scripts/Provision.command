#!/bin/zsh
set -eu
cd "${0:A:h:h}"
print 'Eight Sleep button — Wi-Fi provisioning'
print 'Enter your 2.4 GHz Wi-Fi details below. Both fields are hidden.'
print 'The account password is not needed or transferred to the device.'
ports=(/dev/cu.usbmodem*(N))
if (( ${#ports} != 1 )); then
  print 'Expected one USB serial device. Use eightctl.py provision --port PORT to select it explicitly.'
  exit 1
fi
.venv/bin/python scripts/eightctl.py provision --port "$ports[1]"
print '\nProvisioning finished. Return to Codex for the physical cooling test.'
read -r '?Press Return to close this window.'
