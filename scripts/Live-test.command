#!/bin/zsh
set -eu
cd "${0:A:h:h}"
print 'Eight Sleep button — live Rapid Cooling test'
print 'Have the Eight Sleep app open on your side before typing COOL.'
print 'This confirms activation without waiting for the full cooling timer.'
.venv/bin/python scripts/eightctl.py activate
print '\nTest finished. Return to Codex with what you observed in the app/on the bed.'
read -r '?Press Return to close this window.'
