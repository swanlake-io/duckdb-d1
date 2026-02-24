#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${ROOT_DIR}/build/release/duckdb}"
DUCKDB_UNITTEST_BIN="${DUCKDB_UNITTEST_BIN:-}"

if [[ -n "${DUCKDB_BIN}" && ! -x "${DUCKDB_BIN}" && -z "${DUCKDB_UNITTEST_BIN}" ]]; then
    echo "DuckDB binary not found: ${DUCKDB_BIN}" >&2
    exit 1
fi
if [[ -n "${DUCKDB_UNITTEST_BIN}" && ! -x "${DUCKDB_UNITTEST_BIN}" ]]; then
    echo "DuckDB unittest binary not found: ${DUCKDB_UNITTEST_BIN}" >&2
    exit 1
fi

DUCKDB_BIN="${DUCKDB_BIN}" DUCKDB_UNITTEST_BIN="${DUCKDB_UNITTEST_BIN}" "${ROOT_DIR}/scripts/run_live_d1_tests.sh"
