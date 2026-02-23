# duckdb-d1

DuckDB extension for querying and executing SQL against Cloudflare D1 over HTTP.

## Features

- Secrets-based authentication (`TYPE d1`)
- Native storage extension:
  - `ATTACH ... (TYPE d1, SECRET ..., [READ_ONLY], [schema_cache_ttl ...])`
  - attached-table `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE AS`
  - attached DDL: `CREATE TABLE/VIEW/INDEX`, `ALTER TABLE` (rename/add/drop/rename column), `DROP TABLE/VIEW/INDEX`
- Read functions:
  - `d1_query(secret_name, sql, params := ..., all_varchar := false)`
  - `d1_raw(secret_name, sql, params := ..., all_varchar := false)`
  - `d1_tables(secret_name, include_views := true, all_varchar := true)`
  - `d1_scan(secret_name, table_name, filter := ..., max_rows := ..., all_varchar := false)`
  - `d1_attach(secret_name, overwrite := false, include_views := true, schema := 'main')`
- Write functions:
  - `d1_execute(secret_name, sql, params := ...)`
  - `d1_batch_execute(secret_name, batch)`
- HTTP transport based on DuckDB-bundled `cpp-httplib` (OpenSSL)

## Build

```bash
make release
```

Main binaries:

- `build/release/duckdb`
- `build/release/unittest`
- `build/release/extension/d1/d1.duckdb_extension`

## Quick Start

```sql
CREATE SECRET d1_prod (
  TYPE d1,
  account_id 'your-account-id',
  database_id 'your-database-id',
  api_token 'your-api-token',
  endpoint 'https://api.cloudflare.com/client/v4'
);

SELECT * FROM d1_query('d1_prod', 'SELECT 42 AS answer');

SELECT *
FROM d1_execute('d1_prod', 'INSERT INTO events(id, name) VALUES (?, ?)', params := [1, 'demo']);

SELECT *
FROM d1_batch_execute(
  'd1_prod',
  '[{"sql":"UPDATE events SET name = ? WHERE id = ?","params":["updated",1]},"DELETE FROM events WHERE id = 1"]'
);

SELECT name, type
FROM d1_tables('d1_prod')
ORDER BY name;

SELECT *
FROM d1_scan('d1_prod', 'events', filter := 'id = 1', max_rows := 10);

-- Create local DuckDB views that point to remote D1 objects.
SELECT *
FROM d1_attach('d1_prod', overwrite := true, include_views := false, schema := 'd1_prod_schema');

SELECT * FROM d1_prod_schema.events;

-- Native attached-catalog mode (recommended for table-level workflows)
ATTACH '' AS d1_remote (
  TYPE d1,
  SECRET d1_prod,
  schema_cache_ttl 30
);

SELECT count(*) FROM d1_remote.events;
INSERT INTO d1_remote.events(id, name) VALUES (1, 'demo');
UPDATE d1_remote.events SET name = 'updated' WHERE id = 1;
DELETE FROM d1_remote.events WHERE id = 1;

CREATE TABLE d1_remote.events_copy AS SELECT * FROM d1_remote.events;
CREATE INDEX idx_events_name ON d1_remote.events(name);
```

## Compatibility Matrix

| Feature | Status | Notes |
|---|---|---|
| Secret auth (`TYPE d1`) | Supported | `account_id`, `database_id`, `api_token`, optional `endpoint` |
| Table functions (`d1_query`, `d1_raw`, `d1_execute`, `d1_batch_execute`) | Supported | Read/write over HTTP API |
| Native `ATTACH ... TYPE d1` | Supported | Single exposed schema: `main` |
| Attached `SELECT` | Supported | Projection + filter pushdown for common predicates |
| Attached `INSERT` | Supported | `RETURNING` not supported |
| Attached `UPDATE` | Supported | `RETURNING` and `SET DEFAULT` not supported |
| Attached `DELETE` | Supported | `RETURNING` not supported |
| Attached `CREATE TABLE AS` | Supported | Uses remote DDL + row inserts |
| Attached `CREATE TABLE` | Supported | Existing table conflict modes supported |
| Attached `CREATE VIEW` | Supported | Empty view SQL is rejected |
| Attached `CREATE INDEX` | Supported | Expression list emitted as SQLite-compatible SQL |
| Attached `ALTER TABLE` | Partially supported | rename table/column, add/drop column |
| Attached `DROP TABLE/VIEW/INDEX` | Supported | Uses remote DDL path |
| Multi-schema D1 catalogs | Not supported | D1 catalog exposes only `main` |
| Full DuckDB transaction parity on remote D1 | Not supported | D1 semantics are statement auto-commit / API batch-based |

## Operational Notes

- D1 attached catalogs are remote HTTP-backed; set `schema_cache_ttl` for metadata refresh behavior.
- Use `READ_ONLY` attach mode for safety when needed.
- External access must be enabled (`enable_external_access = true`) to attach D1 databases.
- DML row targeting for `UPDATE`/`DELETE` uses row identifiers from attached scans; unsupported complex cases error explicitly.

## Testing

SQL tests:

```bash
make test
```

HTTP integration tests (real Cloudflare D1):

```bash
make integration-test
```

Live D1 integration tests (real Cloudflare D1):

```bash
export D1_ACCOUNT_ID=...
export D1_DATABASE_ID=...
export D1_API_TOKEN=...
# optional, defaults to https://api.cloudflare.com/client/v4
export D1_ENDPOINT=https://api.cloudflare.com/client/v4
make live-d1-test
```

## Lint

Strict lint checks:

```bash
make lint-strict
```

This runs format checks, clang-tidy (`clang-analyzer`, `bugprone`, `performance`) and a warnings-as-errors build.

## Coverage

Coverage (fails if line coverage < 80% for extension source files):

```bash
make coverage
```

Outputs:

- `build/coverage.xml`

## CI

`MainDistributionPipeline.yml` includes:

- Distribution build
- Code quality checks
- Strict lint job
- SQL + HTTP integration tests
- Coverage threshold enforcement (`>= 80%`)
