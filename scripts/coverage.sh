#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"
CPU_COUNT="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
if [[ -n "${COVERAGE_BUILD_JOBS:-}" ]]; then
    BUILD_JOBS="${COVERAGE_BUILD_JOBS}"
elif [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    # Keep memory pressure predictable on GitHub-hosted runners.
    BUILD_JOBS=2
else
    BUILD_JOBS="${CPU_COUNT}"
fi

GENERATOR_ARGS=()
if [[ "${GEN:-}" == "ninja" || "${GEN:-}" == "Ninja" ]]; then
    GENERATOR_ARGS=(-G "Ninja")
fi

EXT_CMAKE_ARGS=()
if [[ -n "${EXT_FLAGS:-}" ]]; then
    # shellcheck disable=SC2206
    EXT_CMAKE_ARGS=(${EXT_FLAGS})
fi

# Configure and build only unittest target with gcov instrumentation.
cmake "${GENERATOR_ARGS[@]}" \
    -DFORCE_COLORED_OUTPUT=1 \
    -DEXTENSION_STATIC_BUILD=1 \
    -DDUCKDB_EXTENSION_CONFIGS="${ROOT_DIR}/extension_config.cmake" \
    "${EXT_CMAKE_ARGS[@]}" \
    -DUNITTEST_ROOT_DIRECTORY="${ROOT_DIR}/" \
    -DBENCHMARK_ROOT_DIRECTORY="${ROOT_DIR}/" \
    -DENABLE_UNITTEST_CPP_TESTS=FALSE \
    -DBUILD_EXTENSION_TEST_DEPS=default \
    -DCMAKE_C_FLAGS=--coverage \
    -DCMAKE_CXX_FLAGS=--coverage \
    -DCMAKE_BUILD_TYPE=Debug \
    -S "${ROOT_DIR}/duckdb/" \
    -B "${ROOT_DIR}/build/debug"
cmake --build "${ROOT_DIR}/build/debug" --config Debug --parallel "${BUILD_JOBS}" --target unittest

# Reset old coverage counters.
find "${ROOT_DIR}/build/debug" -name '*.gcda' -delete

# Run SQLLogic tests (all extension tests live under test/sql/*.test).
DUCKDB_UNITTEST_BIN="${ROOT_DIR}/build/debug/test/unittest" scripts/run_integration_tests.sh

# Resolve gcovr binary using an isolated virtualenv.
COVERAGE_VENV="${ROOT_DIR}/build/coverage-venv"
if [[ ! -x "${COVERAGE_VENV}/bin/python3" ]]; then
    python3 -m venv "${COVERAGE_VENV}"
fi
if ! "${COVERAGE_VENV}/bin/python3" -c "import gcovr" >/dev/null 2>&1; then
    "${COVERAGE_VENV}/bin/python3" -m pip install --quiet gcovr
fi
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
    --merge-mode-functions separate \
    --xml-pretty \
    --output "${ROOT_DIR}/build/coverage.xml" \
    --print-summary \
    --txt \
    --fail-under-line 50

echo "Coverage checks passed"
