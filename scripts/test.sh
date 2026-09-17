#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/tests
c++ -std=c++17 -Wall -Wextra -Werror tests/button_test.cpp -o build/tests/button_test
build/tests/button_test
tests/run_api_tests.sh
.venv/bin/python -m unittest discover -s tests -p 'test_*.py'
.venv/bin/python -m py_compile scripts/*.py
.venv/bin/python scripts/generate_fonts.py --check
