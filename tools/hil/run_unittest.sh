#!/usr/bin/env bash
# Runs a unittest module from this directory. See BUILD.bazel for why this is not a py_test.
set -euo pipefail
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
exec python3 -m unittest "$1" -v
