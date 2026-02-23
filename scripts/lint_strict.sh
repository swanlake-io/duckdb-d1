#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

LINT_VENV="${ROOT_DIR}/build/lint-venv"
if [[ ! -x "${LINT_VENV}/bin/python3" ]]; then
    python3 -m venv "${LINT_VENV}"
fi
"${LINT_VENV}/bin/python3" -m pip install --quiet --upgrade pip
"${LINT_VENV}/bin/python3" -m pip install --quiet "black>=24" "clang-format==11.0.1" "cmakelang" "clang-tidy"
export PATH="${LINT_VENV}/bin:${PATH}"

# Enforce deterministic formatting and strict static analysis on extension sources.
make format-check
TIDY_CHECKS='-*,clang-analyzer-*,bugprone-*,performance-*' make tidy-check

# Ensure no compiler warnings slip through for extension code.
TREAT_WARNINGS_AS_ERRORS=1 make release -j4

echo "Strict lint checks passed"
