#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

# Rebuild with gcov instrumentation.
EXT_DEBUG_FLAGS='-DCMAKE_C_FLAGS=--coverage -DCMAKE_CXX_FLAGS=--coverage' make debug -j4

# Reset old coverage counters.
find "${ROOT_DIR}/build/debug" -name '*.gcda' -delete

# Run sqllogictest suite and HTTP integration suite.
make test_debug
DUCKDB_BIN="${ROOT_DIR}/build/debug/duckdb" scripts/run_integration_tests.sh

# Resolve gcovr binary using an isolated virtualenv.
COVERAGE_VENV="${ROOT_DIR}/build/coverage-venv"
if [[ ! -x "${COVERAGE_VENV}/bin/python3" ]]; then
    python3 -m venv "${COVERAGE_VENV}"
fi
"${COVERAGE_VENV}/bin/python3" -m pip install --quiet --upgrade pip
"${COVERAGE_VENV}/bin/python3" -m pip install --quiet gcovr
GCOVR_BIN="${COVERAGE_VENV}/bin/gcovr"

"${GCOVR_BIN}" \
    --root "${ROOT_DIR}" \
    --filter "${ROOT_DIR}/src" \
    --exclude "${ROOT_DIR}/duckdb" \
    --exclude ".*CMakeCCompilerId.*" \
    --exclude ".*CMakeCXXCompilerId.*" \
    --object-directory "${ROOT_DIR}/build/debug/extension/d1" \
    --gcov-ignore-errors source_not_found \
    --gcov-ignore-errors no_working_dir_found \
    --xml-pretty \
    --output "${ROOT_DIR}/build/coverage.xml" \
    --print-summary \
    --txt \
    --fail-under-line 80

echo "Coverage checks passed"
