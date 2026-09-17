#!/bin/zsh
set -eu
cd "${0:A:h:h}"
print 'Eight Sleep button — live Rapid Cooling test'
print 'Have the Eight Sleep app open on your side before typing COOL.'
print 'The utility will observe the existing cooling cycle until it ends.'
.venv/bin/python scripts/eightctl.py activate
print '\nTest finished. Return to Codex with what you observed in the app/on the bed.'
read -r '?Press Return to close this window.'
