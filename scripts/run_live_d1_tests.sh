#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${ROOT_DIR}/build/release/duckdb}"

if [[ ! -x "${DUCKDB_BIN}" ]]; then
    echo "DuckDB binary not found: ${DUCKDB_BIN}" >&2
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

RUN_TAG="${GITHUB_RUN_ID:-local}_$RANDOM"
TABLE_NAME="duckdb_d1_ci_${RUN_TAG}"
INDEX_NAME="idx_${TABLE_NAME}_name"
SCHEMA_NAME="d1_live_views_${RUN_TAG}"

cleanup() {
    local cleanup_sql
    read -r -d '' cleanup_sql <<SQL || true
LOAD '${ROOT_DIR}/build/release/extension/d1/d1.duckdb_extension';
CREATE SECRET d1_live_cleanup (
    TYPE d1,
    account_id '${D1_ACCOUNT_ID}',
    database_id '${D1_DATABASE_ID}',
    api_token '${D1_API_TOKEN}',
    endpoint '${D1_ENDPOINT}'
);
ATTACH '' AS d1_live_cleanup (TYPE d1, SECRET d1_live_cleanup, schema_cache_ttl 0);
DROP INDEX IF EXISTS d1_live_cleanup.${INDEX_NAME};
DROP TABLE IF EXISTS d1_live_cleanup.${TABLE_NAME};
DETACH d1_live_cleanup;
SQL
    printf '%s\n' "${cleanup_sql}" | "${DUCKDB_BIN}" -csv -noheader >/dev/null 2>&1 || true
}
trap cleanup EXIT

read -r -d '' SQL <<SQL || true
LOAD '${ROOT_DIR}/build/release/extension/d1/d1.duckdb_extension';
CREATE SECRET d1_live (
    TYPE d1,
    account_id '${D1_ACCOUNT_ID}',
    database_id '${D1_DATABASE_ID}',
    api_token '${D1_API_TOKEN}',
    endpoint '${D1_ENDPOINT}'
);
ATTACH '' AS d1_live_attached (TYPE d1, SECRET d1_live, schema_cache_ttl 0);
CREATE TABLE d1_live_attached.${TABLE_NAME}(id INTEGER, name TEXT);
INSERT INTO d1_live_attached.${TABLE_NAME} VALUES (1, 'alice');
INSERT INTO d1_live_attached.${TABLE_NAME} VALUES (2, 'bob');
SELECT 'query', answer FROM d1_query('d1_live', 'SELECT 42 AS answer');
SELECT 'raw', answer FROM d1_raw('d1_live', 'SELECT 42 AS answer');
SELECT 'execute', success::INT FROM d1_execute('d1_live', 'SELECT 1');
SELECT 'batch', count(*) FROM d1_batch_execute('d1_live', '["select 1", "select 2"]');
SELECT 'scan', count(*) FROM d1_scan('d1_live', '${TABLE_NAME}');
SELECT 'attach_fn', success::INT
FROM d1_attach('d1_live', overwrite := true, include_views := false, schema := '${SCHEMA_NAME}');
SELECT 'attach_view_scan', count(*) FROM ${SCHEMA_NAME}.${TABLE_NAME};
SELECT 'count', count(*) FROM d1_live_attached.${TABLE_NAME};
UPDATE d1_live_attached.${TABLE_NAME} SET name = 'alice-updated' WHERE id = 1;
SELECT 'updated', name FROM d1_live_attached.${TABLE_NAME} WHERE id = 1;
DELETE FROM d1_live_attached.${TABLE_NAME} WHERE id = 2;
SELECT 'after_delete', count(*) FROM d1_live_attached.${TABLE_NAME};
CREATE INDEX ${INDEX_NAME} ON d1_live_attached.${TABLE_NAME}(name);
SELECT 'index_created', 1;
DROP INDEX IF EXISTS d1_live_attached.${INDEX_NAME};
DROP TABLE d1_live_attached.${TABLE_NAME};
SELECT 'drop_done', 1;
DETACH d1_live_attached;
SQL

OUTPUT="$(printf '%s\n' "${SQL}" | "${DUCKDB_BIN}" -csv -noheader)"

assert_line() {
    local expected="$1"
    if ! grep -Fxq "${expected}" <<<"${OUTPUT}"; then
        echo "Live D1 integration assertion failed. Expected line: ${expected}" >&2
        echo "Actual output:" >&2
        echo "${OUTPUT}" >&2
        exit 1
    fi
}

assert_line "count,2"
assert_line "query,42"
assert_line "raw,42"
assert_line "execute,1"
assert_line "batch,2"
assert_line "scan,2"
assert_line "attach_fn,1"
assert_line "attach_view_scan,2"
assert_line "updated,alice-updated"
assert_line "after_delete,1"
assert_line "index_created,1"
assert_line "drop_done,1"

echo "Live D1 integration tests passed"
