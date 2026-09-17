#!/bin/zsh
set -eu
cd "${0:A:h:h}"
print 'Eight Sleep button — local account setup'
print 'Email and password entry are hidden. Nothing you type is sent to Codex.'
print 'This step only logs in and reads your device assignment/settings.'
.venv/bin/python scripts/eightctl.py login
print '\nRead-only setup finished. Return to Codex to coordinate the cooling test.'
read -r '?Press Return to close this window.'
