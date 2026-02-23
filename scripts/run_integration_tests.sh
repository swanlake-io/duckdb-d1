#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${ROOT_DIR}/build/release/duckdb}"

if [[ ! -x "${DUCKDB_BIN}" ]]; then
    echo "DuckDB binary not found: ${DUCKDB_BIN}" >&2
    exit 1
fi

DUCKDB_BIN="${DUCKDB_BIN}" "${ROOT_DIR}/scripts/run_live_d1_tests.sh"
