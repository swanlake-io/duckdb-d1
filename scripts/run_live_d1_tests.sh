#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${ROOT_DIR}/build/release/duckdb}"

derive_unittest_bin() {
    local db_bin="$1"
    local db_dir
    db_dir="$(dirname "${db_bin}")"
    echo "${db_dir}/test/unittest"
}

DUCKDB_UNITTEST_BIN="${DUCKDB_UNITTEST_BIN:-$(derive_unittest_bin "${DUCKDB_BIN}")}"
if [[ ! -x "${DUCKDB_UNITTEST_BIN}" ]]; then
    # Fallback to release unittest when DUCKDB_BIN does not map to a build directory.
    DUCKDB_UNITTEST_BIN="${ROOT_DIR}/build/release/test/unittest"
fi

if [[ ! -x "${DUCKDB_UNITTEST_BIN}" ]]; then
    echo "DuckDB unittest binary not found: ${DUCKDB_UNITTEST_BIN}" >&2
    exit 1
fi

for required in D1_ACCOUNT_ID D1_DATABASE_ID D1_API_TOKEN; do
    if [[ -z "${!required:-}" ]]; then
        echo "Missing required environment variable: ${required}" >&2
        exit 1
    fi
done

D1_ENDPOINT="${D1_ENDPOINT:-https://api.cloudflare.com/client/v4}"
if [[ -z "${D1_ENDPOINT}" ]]; then
    D1_ENDPOINT="https://api.cloudflare.com/client/v4"
fi

export D1_ENDPOINT
export D1_RUN_TAG="${D1_RUN_TAG:-${GITHUB_RUN_ID:-local}_${RANDOM}}"

mapfile -t SQL_TEST_FILES < <(find "${ROOT_DIR}/test/sql" -type f -name '*.test' | sort)
if [[ "${#SQL_TEST_FILES[@]}" -eq 0 ]]; then
    echo "No SQLLogic test files found under ${ROOT_DIR}/test/sql" >&2
    exit 1
fi

"${DUCKDB_UNITTEST_BIN}" "${SQL_TEST_FILES[@]}"

echo "Live D1 integration tests passed"
