#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

LINT_VENV="${ROOT_DIR}/build/lint-venv"
if [[ ! -x "${LINT_VENV}/bin/python3" ]]; then
    python3 -m venv "${LINT_VENV}"
fi
if ! "${LINT_VENV}/bin/python3" -c "import black, cmakelang" >/dev/null 2>&1; then
    "${LINT_VENV}/bin/python3" -m pip install --quiet "black>=24" "cmakelang"
fi
export PATH="${LINT_VENV}/bin:${PATH}"

if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy is required for strict lint checks" >&2
    exit 1
fi

if ! command -v clang-format-11 >/dev/null 2>&1 && ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format (preferably clang-format-11) is required for strict lint checks" >&2
    exit 1
fi

# Enforce deterministic formatting and strict static analysis on extension sources.
make format-check
TIDY_CHECKS='-*,clang-analyzer-*,bugprone-*,performance-*' make tidy-check

# Ensure no compiler warnings slip through for extension code.
TREAT_WARNINGS_AS_ERRORS=1 make release -j4

echo "Strict lint checks passed"
