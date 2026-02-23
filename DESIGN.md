# duckdb-d1: Research Notes and Technical Design

Date: 2026-02-23
Author: Codex (research + architecture proposal)

## 1. Goal

Build a DuckDB extension that can query Cloudflare D1 over HTTP while preserving as much DuckDB UX as possible.

The key decision requested:
- fork `duckdb-sqlite`
- fork `duckdb-postgres`
- or start from `extension-template`

## 2. Executive Decision

Use `extension-template` as the base, and selectively reuse patterns from:
- `duckdb-sqlite` for SQLite schema semantics/introspection ideas
- `duckdb-postgres` for remote-extension patterns (secrets, attach wiring, cache/pool patterns)

Do not fork either connector directly.

Why:
- D1 is HTTP API + SQLite SQL semantics.
- `duckdb-sqlite` is tightly coupled to local `sqlite3_*` C API and local transaction lifecycle.
- `duckdb-postgres` is tightly coupled to Postgres wire protocol (`libpq`, `COPY`, `CTID`, snapshots) and has large protocol/build baggage.

## 3. Research Sources

### 3.1 Local codebases inspected

- `/Users/wangfenjin/github/cloudflare-go/d1`
- `/Users/wangfenjin/github/duckdb-sqlite`
- `/Users/wangfenjin/github/duckdb-postgres`
- `/Users/wangfenjin/github/extension-template`
- `/Users/wangfenjin/github/duckdb` (core APIs used by extensions)

### 3.2 Cloudflare docs inspected

- D1 Query endpoint: https://developers.cloudflare.com/api/resources/d1/subresources/database/methods/query/
- D1 Raw endpoint: https://developers.cloudflare.com/api/resources/d1/subresources/database/methods/raw/
- D1 Workers Binding API (D1Database): https://developers.cloudflare.com/d1/worker-api/d1-database/
- D1 Workers Binding API (overview + type conversion): https://developers.cloudflare.com/d1/worker-api/
- D1 Return objects: https://developers.cloudflare.com/d1/worker-api/return-object/
- D1 SQL statements/PRAGMA compatibility: https://developers.cloudflare.com/d1/sql-api/sql-statements/
- D1 Limits: https://developers.cloudflare.com/d1/platform/limits/
- D1 tutorial note on REST API usage/rate limits:
  https://developers.cloudflare.com/d1/tutorials/build-an-api-to-access-d1/

## 4. Detailed Findings

## 4.1 Cloudflare D1 API + cloudflare-go SDK findings

### 4.1.1 Endpoint surface and payload shape

From `cloudflare-go/d1`:
- `Query`: `POST /accounts/{account_id}/d1/database/{database_id}/query`
  (`/Users/wangfenjin/github/cloudflare-go/d1/database.go:220`)
- `Raw`: `POST /accounts/{account_id}/d1/database/{database_id}/raw`
  (`/Users/wangfenjin/github/cloudflare-go/d1/database.go:252`)

Body unions support both:
- single query (`sql` + optional `params`)
- batch object (`batch: [{sql, params}, ...]`)
(`/Users/wangfenjin/github/cloudflare-go/d1/database.go:1542`)

Result models in SDK:
- `QueryResult` => `meta + results + success`
  (`/Users/wangfenjin/github/cloudflare-go/d1/database.go:283`)
- `DatabaseRawResponse` => `meta + results.columns + results.rows + success`
  (`/Users/wangfenjin/github/cloudflare-go/d1/database.go:737`)

### 4.1.2 Metadata available per statement

Both query/raw include useful metadata:
- `changed_db`, `changes`, `duration`, `last_row_id`
- `rows_read`, `rows_written`
- `served_by_colo`, `served_by_region`, `served_by_primary`
- `size_after`, `timings.sql_duration_ms`
(`cloudflare-go` refs: `database.go:307`, `database.go:762`)

This is valuable for:
- exposing operational metrics in DuckDB helper functions
- adaptive query chunking decisions

### 4.1.3 Transaction behavior constraints

From Workers D1 docs:
- D1 "operates in auto-commit" but `batch()` is transactional and rolls back on failure.
  (Docs lines around:
  `https://developers.cloudflare.com/d1/worker-api/d1-database/`, lines 207-209 in crawled view.)

Implication:
- DuckDB-style multi-statement transaction semantics cannot be assumed to map 1:1.
- For writes we should prefer explicit batch APIs and avoid pretending full remote ACID parity.

### 4.1.4 SQL semantics and schema introspection compatibility

D1 supports SQLite-like SQL and key PRAGMA/schema introspection paths:
- `PRAGMA table_list`, `PRAGMA table_info`, `PRAGMA index_list`, etc.
- direct `sqlite_master` querying
(`https://developers.cloudflare.com/d1/sql-api/sql-statements/`)

Implication:
- schema discovery logic from `duckdb-sqlite` is reusable conceptually.

### 4.1.5 Type conversion caveats

From Workers Binding API docs:
- booleans stored as INTEGER (`0/1`)
- JS number precision caveat for 64-bit integer values
- `ArrayBuffer`/views map to BLOB with conversion caveats
(`https://developers.cloudflare.com/d1/worker-api/` and
`https://developers.cloudflare.com/d1/worker-api/return-object/`)

Implication:
- strict, explicit type policy is required in extension.
- start conservative and avoid lossy coercions silently.

### 4.1.6 Platform limits affecting connector design

From D1 limits:
- max SQL statement length: 100 KB
- max bound params: 100
- max query duration: 30s
- max row/BLOB size: 2MB
(`https://developers.cloudflare.com/d1/platform/limits/`)

Also:
- D1 DB is single-threaded per database and processes one query at a time.
- can return overloaded errors under queue pressure.
(`https://developers.cloudflare.com/d1/platform/limits/`)

Implication:
- read scans must use chunking and conservative concurrency defaults.
- retry/backoff must handle overload and transient HTTP failures.

### 4.1.7 REST API positioning

Cloudflare tutorial explicitly says:
- D1 built-in REST API is best for administrative use, global Cloudflare API rate limits apply.
(`https://developers.cloudflare.com/d1/tutorials/build-an-api-to-access-d1/`, line 131 in crawled view)

Implication:
- extension should support custom `endpoint` (proxy Worker mode) in addition to direct Cloudflare API.

## 4.2 `duckdb-sqlite` findings

High-level:
- excellent SQLite semantic fit
- transport/runtime fit is poor for D1

Evidence:
- local DB open/prepare/execute via `sqlite3_open_v2`, `sqlite3_prepare_v2`, `sqlite3_exec`
  (`/Users/wangfenjin/github/duckdb-sqlite/src/sqlite_db.cpp:47`)
- schema introspection using `sqlite_master` + `PRAGMA table_info`
  (`sqlite_db.cpp:118`, `sqlite_db.cpp:181`)
- transaction lifecycle assumes local connection:
  `BEGIN TRANSACTION`, `COMMIT`, `ROLLBACK`
  (`/Users/wangfenjin/github/duckdb-sqlite/src/storage/sqlite_transaction.cpp:35`)
- write path tied to `rowid` predicates for update/delete
  (`sqlite_update.cpp:42`, `sqlite_delete.cpp:34`)

Fit assessment:
- Reuse: schema/affinity ideas, attach/catalog wiring patterns.
- Not reusable as-is: core execution, transaction manager, scan path.

## 4.3 `duckdb-postgres` findings

High-level:
- best source of remote connector architecture patterns
- worst protocol fit for D1

Evidence:
- heavy protocol-specific scanner:
  libpq + snapshot + `COPY (...) TO STDOUT` + CTID logic
  (`/Users/wangfenjin/github/duckdb-postgres/src/postgres_scanner.cpp:3`,
   `postgres_scanner.cpp:67`,
   `postgres_scanner.cpp:287`)
- transaction manager tightly maps to Postgres isolation/BEGIN/COMMIT/ROLLBACK
  (`/Users/wangfenjin/github/duckdb-postgres/src/storage/postgres_transaction.cpp:39`)
- secret registration pattern is mature and reusable
  (`/Users/wangfenjin/github/duckdb-postgres/src/postgres_extension.cpp:151`)
- build complexity very high, vendoring PostgreSQL sources in CMake
  (`/Users/wangfenjin/github/duckdb-postgres/CMakeLists.txt:121`)

Fit assessment:
- Reuse: secret/provider pattern, attach option parsing style, connection pool/cache ideas.
- Not reusable as-is: scanner/protocol/DML internals, build chain.

## 4.4 `extension-template` findings

High-level:
- clean minimal baseline, least technical debt
- ideal for D1-native transport + semantics

Evidence:
- minimal CMake and extension entrypoint only
  (`/Users/wangfenjin/github/extension-template/CMakeLists.txt:4`,
   `/Users/wangfenjin/github/extension-template/src/quack_extension.cpp:29`)

Fit assessment:
- best foundation for a new connector with custom HTTP contract.

## 4.5 DuckDB core API findings relevant to implementation

### 4.5.1 Storage extension hooks

- `StorageExtension` exposes:
  - `attach`
  - `create_transaction_manager`
  (`/Users/wangfenjin/github/duckdb/src/include/duckdb/storage/storage_extension.hpp:34`)

- attach options and db type are available in `AttachOptions`
  (`/Users/wangfenjin/github/duckdb/src/include/duckdb/main/attached_database.hpp:53`)

### 4.5.2 Secret system is extension-friendly

- register secret type/provider through extension loader
  (`extension_loader.hpp:66`, `extension_loader.hpp:92`)
- use `KeyValueSecret` for connector credentials
  (`secret.hpp:164`)

### 4.5.3 HTTP utility caveat

- DuckDB `HTTPUtil` exists (`http_util.hpp:223`) and supports retries.
- But default client in current DuckDB checkout does not implement POST/PUT/HEAD/DELETE in `HTTPLibClient`.
  (`/Users/wangfenjin/github/duckdb/src/main/http/http_util.cpp:167-181`)

Implication:
- For D1 (POST-based API), do not rely on default `HTTPUtil` client directly.
- Implement explicit transport layer in extension using `cpp-httplib` (or equivalent), with retry/backoff policy.
- Ensure HTTPS support is enabled for `https://api.cloudflare.com` (in `cpp-httplib`, TLS backend must be enabled).

## 5. Option Analysis and Decision Matrix

Scoring: 1 (poor) to 5 (excellent)

| Criterion | `duckdb-sqlite` fork | `duckdb-postgres` fork | `extension-template` base |
|---|---:|---:|---:|
| D1 protocol fit (HTTP) | 1 | 1 | 5 |
| SQL semantic fit (SQLite-like) | 4 | 2 | 4 |
| Build complexity risk | 2 | 1 | 5 |
| Rework required | 2 | 1 | 4 |
| Long-term maintainability | 2 | 2 | 5 |
| Overall | 11 | 7 | 23 |

Decision:
- Start from `extension-template`.
- Cherry-pick targeted patterns from sqlite/postgres connectors.

## 6. Proposed Architecture

## 6.1 Scope boundaries

In scope (initial):
- D1 query/read support over HTTP
- secrets-based authentication
- table functions first
- read-only attach in second step

Out of scope (initial):
- full parity with DuckDB remote transactional writes
- full D1 control plane (create/delete/import/export/time-travel endpoints)

## 6.2 Layered design

Layer A: Core transport/auth
- `D1Client` with:
  - endpoint construction
  - auth headers (`Bearer <token>`)
  - retries/backoff/timeouts
  - JSON encode/decode
  - Cloudflare envelope error mapping

Layer B: D1 protocol model
- request builders:
  - single query (`sql`, `params`)
  - batch (`batch:[...]`)
- response model:
  - statement results array
  - `meta`
  - raw shape (`columns`, `rows`)

Layer C: DuckDB integration surfaces
- secrets (`TYPE d1`)
- table functions (`d1_query`, `d1_raw`, `d1_execute`, optional `d1_batch_execute`)
- storage extension (`ATTACH ... TYPE d1`) read-only first

Layer D: optimizer/pushdown/caching
- schema cache
- filter/projection pushdown
- execution chunking for long scans

## 6.3 Recommended module layout

```
src/
  d1_extension.cpp
  d1_secret.cpp
  d1_options.hpp

  client/
    d1_client.hpp
    d1_client.cpp
    d1_http_transport.hpp
    d1_http_transport_httplib.cpp
    d1_json.hpp
    d1_json.cpp
    d1_error.hpp
    d1_error.cpp

  protocol/
    d1_request.hpp
    d1_response.hpp
    d1_type_mapping.hpp
    d1_type_mapping.cpp

  functions/
    d1_query.cpp
    d1_raw.cpp
    d1_execute.cpp
    d1_batch_execute.cpp

  storage/
    d1_storage_extension.cpp
    d1_catalog.cpp
    d1_schema_set.cpp
    d1_table_set.cpp
    d1_table_entry.cpp
    d1_transaction_manager.cpp
    d1_scan.cpp
    d1_filter_pushdown.cpp

test/
  sql/
    d1_query.test
    d1_execute.test
    d1_attach_readonly.test
    d1_pushdown.test
  integration/
    d1_live.test
```

## 7. User-Facing API Proposal

## 7.1 Secrets

Register secret type:
- `TYPE d1`
- default provider: `config`

Named parameters:
- `account_id` (required unless embedded in endpoint/path mode)
- `database_id` (required unless embedded in attach path)
- `api_token` (required)
- `endpoint` (optional, default `https://api.cloudflare.com/client/v4`)
- `api_mode` (optional: `cloudflare_api` default, `proxy_worker`)

Example:
```sql
CREATE SECRET d1_prod (
  TYPE d1,
  account_id '...',
  database_id '...',
  api_token '...',
  endpoint 'https://api.cloudflare.com/client/v4'
);
```

## 7.2 Table functions (Phase 1)

Recommended:
- `d1_query(secret_name, sql, params := NULL)` -> relation
  - object-style result path (`/query`)
- `d1_raw(secret_name, sql, params := NULL)` -> relation
  - array-style result path (`/raw`)
- `d1_execute(secret_name, sql, params := NULL)` -> metadata rows
  - includes `changes`, `rows_read`, `rows_written`, `duration`, etc.
- `d1_batch_execute(secret_name, queries)` -> metadata rows per statement

Semantics:
- `d1_query`/`d1_raw` should enforce single result schema unless explicit multi-result mode is provided.
- if multiple statement results are returned with incompatible schemas, throw explicit binder/runtime error.

## 7.3 Attach (Phase 2)

Read-only attach first:
```sql
ATTACH 'd1://<account_id>/<database_id>' AS myd1 (TYPE d1, SECRET d1_prod, READ_ONLY);
SELECT * FROM myd1.some_table;
```

Attach options:
- `secret`
- `schema_cache_ttl`
- `max_parallel_requests` (default 1)
- `chunk_size`

## 8. Type Mapping Policy

Initial safe policy:
- Table function default mode: typed conversion with strict fallback to `VARCHAR` on uncertain values.
- Config flag:
  - `d1_all_varchar = true/false` (default `false` for raw/query functions, `true` fallback in attach if mixed types detected)

Proposed mappings:
- integer -> `BIGINT` when in range
- floating -> `DOUBLE`
- string -> `VARCHAR`
- boolean-like -> `BOOLEAN` (if unambiguous), else `TINYINT`/`BIGINT`
- null -> nullable column
- blob payloads -> `BLOB` only when binary representation is unambiguous; otherwise `VARCHAR` with warning/error policy

Important constraints:
- D1/Workers docs call out JS precision caveats for large integers.
- avoid silent truncation; add strict mode that errors on lossy conversion.

## 9. Query Planning and Pushdown

## 9.1 Phase 1 (functions)

- no pushdown needed; user-provided SQL executes remotely.
- return data as delivered by D1 endpoint.

## 9.2 Phase 2 (attach read-only)

Implement pushdown incrementally:
- projection pushdown: yes
- filter pushdown: simple comparisons and conjunctions first
- limit pushdown: yes
- complex expressions: fallback to local filter

Parallelism defaults:
- conservative (`max_threads = 1`) due D1 per-database single-threaded execution/queue behavior.

Chunked scanning strategy:
- rowid pagination when possible:
  - `WHERE rowid > ? ORDER BY rowid LIMIT ?`
- fallback:
  - keyset pagination on primary key if discoverable
  - then `LIMIT/OFFSET` as last resort

## 10. Transaction and Write Semantics

## 10.1 Initial position

- Phase 1 + 2: read-first, explicit execution functions for writes.
- do not claim full transactional parity for attached-table DML.

## 10.2 Explicit write API (Phase 3)

- expose `d1_batch_execute` to align with D1 documented batch rollback semantics.
- for attached-table DML pushdown:
  - gate behind config flag
  - support only safe, well-bounded statements
  - reject unsupported transaction scenarios with clear errors

## 11. Error Handling Strategy

Map errors into DuckDB exception hierarchy with context:
- HTTP transport errors:
  - timeout
  - connection errors
  - throttling (`429`)
  - server errors (`5xx`)
- D1 logical/API errors:
  - `success=false` with Cloudflare error objects
  - per-statement failure in batch results
- shape/contract errors:
  - unexpected JSON envelope
  - missing `result`/`meta`/`rows`

Include in message:
- account/database identifiers (redacted/token-safe)
- endpoint path
- statement index for batch
- retry attempts and final status

## 12. Security and Secrets

- API token must only come from secret manager or explicit function argument in development mode.
- redact `api_token` in all logs/errors.
- support secret lookup via `CREATE SECRET` + `ATTACH ... SECRET`.
- avoid writing token to on-disk temp files.

## 13. Testing Plan

## 13.1 Unit tests

- request serialization:
  - single query
  - batch payload
  - params encoding
- response parser:
  - query/raw success envelopes
  - error envelopes
  - mixed/missing field behavior
- type mapping:
  - numeric boundaries
  - null/mixed columns

## 13.2 SQL tests (offline)

- function registration and binder behavior
- deterministic parser behavior with invalid-endpoint error assertions
- attach/binder guardrails that do not require a live network call

## 13.3 Integration tests (live D1, opt-in)

Environment variables:
- `D1_ACCOUNT_ID`
- `D1_DATABASE_ID`
- `D1_API_TOKEN`
- optional `D1_ENDPOINT`

Cases:
- schema introspection via `sqlite_master` and PRAGMAs
- query/raw equivalence on simple tables
- batch rollback behavior verification
- overload/retry behavior (if testable)

## 14. Delivery Plan (Phased)

## Phase 0: Bootstrap (1-2 days)

- initialize from `extension-template`
- rename extension to `d1`
- set up CI and test skeleton
- add dependency management for chosen HTTP transport

Exit criteria:
- extension loads
- placeholder function and tests run

## Phase 1: D1 client + secrets + table functions (5-8 days)

- implement secret type/provider `d1`
- implement HTTP transport + JSON parser + retries
- implement `d1_query`, `d1_raw`, `d1_execute`
- add SQL tests for binder/runtime validation and live integration coverage

Exit criteria:
- can query live D1 with token/account/database via function
- stable error mapping

## Phase 2: Read-only attach storage extension (7-12 days)

- implement `ATTACH TYPE d1`
- schema/table discovery via SQLite introspection SQL
- read scan path with projection/filter pushdown v1
- chunking and schema cache

Exit criteria:
- `SELECT` from attached D1 tables works reliably
- documented unsupported cases

## Phase 3: Controlled write support (7-10 days)

- implement explicit batch write function
- optionally add limited attached-table DML pushdown
- transaction semantics guardrails and clear errors

Exit criteria:
- predictable write behavior aligned to D1 semantics

## Phase 4: Hardening + docs (3-6 days)

- observability metrics/log hooks
- performance tuning and regression tests
- user docs/examples + compatibility table

## 15. Key Risks and Mitigations

1. REST API rate limits and suitability for high-throughput query traffic
- Mitigation: support custom proxy endpoint mode; document intended usage.

2. D1 single-threaded DB and 30s query cap
- Mitigation: conservative concurrency defaults + chunked scans + retries.

3. Type fidelity (especially BIGINT/boolean/blob)
- Mitigation: strict conversion mode + conservative fallback + explicit docs.

4. Transaction mismatch with DuckDB expectations
- Mitigation: phased read-first strategy, explicit batch write API, guarded DML pushdown.

5. HTTP transport dependency stability
- Mitigation: keep transport behind `D1Transport` interface; isolate implementation.

## 16. Open Questions to Resolve Early (Prototype Checklist)

1. REST `params` accepted scalar types:
- confirm whether non-string values are accepted consistently on `/query` and `/raw`.

2. Binary/BLOB behavior in REST responses:
- confirm exact encoding and round-trip strategy.

3. Multi-statement behavior on `/query` with mixed result schemas:
- decide strict handling contract in table functions.

4. Best endpoint strategy for production:
- direct Cloudflare API vs proxy Worker; benchmark and rate-limit behavior.

5. DuckDB JSON parser choice:
- evaluate internal helper vs external dependency for robust envelope parsing.

## 17. Fork Decision Recap

- Do not fork `duckdb-postgres`: protocol/build mismatch too large.
- Do not fork `duckdb-sqlite`: local sqlite C API and transaction assumptions require invasive replacement.
- Start from `extension-template`, then import only the specific proven ideas needed.

That gives the lowest long-term complexity and the highest chance of a clean D1-native extension.

## 18. Feature-Complete Execution Plan (Postgres/SQLite Parity Track)

This section is the implementation contract for reaching a feature set comparable to `duckdb-postgres` and `duckdb-sqlite`.
Execution mode is "no-question": keep delivering sequentially and only stop at hard external blockers.

### 18.1 Current baseline (already implemented)

1. Extension bootstrap from template.
2. Secret type/provider (`TYPE d1`) and config resolution.
3. HTTP transport (`cpp-httplib` + OpenSSL), retries, response parsing.
4. Core table functions:
   `d1_query`, `d1_raw`, `d1_execute`, `d1_batch_execute`, `d1_tables`, `d1_scan`, `d1_attach`.
5. Strict lint, integration tests, and coverage gate (`>= 80%`) in CI.

### 18.2 Phase-by-phase buildout

#### Phase A: Storage-extension skeleton (`ATTACH ... TYPE d1`)

1. Add `src/include/d1/storage.hpp` and `src/d1_storage.cpp` with `D1StorageExtension : StorageExtension`.
2. Register storage extension in `src/d1_extension.cpp`:
   `StorageExtension::Register(config, "d1", make_shared_ptr<D1StorageExtension>())`.
3. Implement attach callback:
   parse attach options (`secret`, `endpoint`, `account_id`, `database_id`, `api_token`, `schema_cache_ttl`, `read_only`).
4. Create initial `D1Catalog` object from attach callback.
5. Use `DuckTransactionManager` in first iteration (read-only attach path) to reduce risk.
6. Add SQL tests:
   `ATTACH '' AS d1_db (TYPE d1, SECRET d1_live)` + `SHOW TABLES` + `SELECT`.
7. Exit criteria:
   attached catalog exists and can resolve schemas/tables for reads.

#### Phase B: Catalog and schema discovery parity

1. Add `D1Catalog`, `D1SchemaEntry`, `D1TableEntry` classes.
2. Implement schema discovery using D1 SQLite metadata queries:
   `sqlite_master`, `pragma table_info`, `pragma index_list`, `pragma index_info`.
3. Add cache layers:
   per-catalog schema cache with TTL and explicit invalidation hooks.
4. Add view support in lookup and scan (same behavior as SQLite extension expectations).
5. Implement `LookupSchema`, `ScanSchemas`, schema `LookupEntry`, `Scan`.
6. SQL tests for:
   table lookup, view lookup, index metadata lookup, cache refresh behavior.
7. Exit criteria:
   `SHOW TABLES`, `DESCRIBE`, and direct `SELECT` against attached objects are stable.

#### Phase C: Attached table scan path with pushdown

1. Implement `D1TableEntry::GetScanFunction` to route scans through D1 remote SQL.
2. Add bind data/state classes with:
   projected columns, pushed filters, pushed limit, SQL text, inferred output types.
3. Implement pushdown rules:
   projection pushdown (required),
   simple filter pushdown (`=`, `<>`, `<`, `<=`, `>`, `>=`, `IN`, `IS NULL`, conjunctions),
   limit pushdown.
4. Add fallback rules:
   non-pushable expressions remain local in DuckDB.
5. Implement cardinality heuristics for planning.
6. Add SQL tests comparing pushed vs local-filter equivalent outputs.
7. Exit criteria:
   attached reads perform with projection/filter/limit pushdown and deterministic correctness.

#### Phase D: Attached write path (DML parity)

1. Implement catalog planning hooks in `D1Catalog`:
   `PlanInsert`, `PlanDelete`, `PlanUpdate`, `PlanCreateTableAs`.
2. Add physical operators for D1 write execution (modeled after sqlite/postgres extension patterns):
   `D1Insert`, `D1Delete`, `D1Update`.
3. Use parameterized SQL and batch execution where practical.
4. Enforce guardrails:
   unsupported clauses (`RETURNING`, unsupported conflict actions, unsupported defaults) error clearly.
5. Ensure row-identity strategy for update/delete:
   prefer primary key predicates; use rowid only when available and safe.
6. SQL tests:
   `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE AS`, and `COPY FROM/TO` compatibility checks.
7. Exit criteria:
   write operations on attached tables behave predictably and match documented constraints.

#### Phase E: DDL parity for attached catalog

1. Implement schema entry methods:
   `CreateTable`, `CreateView`, `CreateIndex`, `Alter`, `DropEntry`.
2. Generate SQLite-compatible DDL SQL and execute remotely via D1 client.
3. Refresh/invalidate catalog cache after DDL changes.
4. SQL tests:
   `CREATE TABLE`, `ALTER TABLE` (supported subsets), `DROP TABLE`, `CREATE VIEW`, `CREATE INDEX`.
5. Exit criteria:
   major DDL flows supported with clear unsupported-surface errors.

#### Phase F: Transaction and batching model

1. Introduce dedicated `D1Transaction` and `D1TransactionManager`.
2. Model semantics around D1 reality:
   auto-commit per statement; explicit transactional grouping via batch.
3. Implement deterministic behavior for DuckDB `BEGIN/COMMIT/ROLLBACK` against attached D1:
   either map to staged batch mode or explicit clear error policy when semantics diverge.
4. Add integration tests for multi-statement write behavior and rollback semantics.
5. Exit criteria:
   transaction behavior is explicit, documented, and test-verified.

#### Phase G: Copy/import-export and utility parity

1. Add copy helpers:
   `COPY attached_tbl TO ...` and `COPY attached_tbl FROM ...` through existing DML operators.
2. Add maintenance/utility functions:
   cache clear, metadata refresh, connection diagnostics, remote explain helper.
3. Add extension options:
   max retries, timeout, pushdown level, schema cache TTL, max parallelism.
4. Exit criteria:
   operational ergonomics similar to sqlite/postgres extensions.

#### Phase H: Hardening and release readiness

1. Expand integration tests for real D1 opt-in env.
2. Add concurrency and stress tests (queue overload, retries, throttling behavior).
3. Raise coverage target from 80% to 85% after write-path stabilization.
4. Add compatibility matrix table in README:
   feature supported / partially supported / unsupported.
5. Add benchmark script:
   table scan latency, pushdown effectiveness, write throughput.
6. Exit criteria:
   stable CI, documented behavior, release tag candidate.

### 18.3 Implementation order inside each phase

1. Add minimal class/interface skeleton.
2. Add deterministic SQL tests for binder/parser/error behavior first.
3. Implement runtime logic to satisfy tests.
4. Add integration tests for HTTP-path validation.
5. Run gates:
   `make test`, `make integration-test`, `make lint-strict`, `make coverage`.
6. Merge only when all gates pass.

### 18.4 Non-negotiable quality gates

1. No regression in existing function behavior.
2. Lint must pass with warnings treated as errors.
3. Coverage must remain >= 80% at all times; target >= 85% in hardening phase.
4. All new feature surfaces must have:
   SQL tests, integration tests, and README documentation.

### 18.5 Definition of "feature complete" for this project

The extension is considered feature complete when all of the following are true:

1. `ATTACH ... (TYPE d1)` supports practical read/write workflows at table level.
2. Attached-table `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE AS`, and key DDL are available with explicit constraints.
3. Pushdown, caching, retries, and error mapping are production-safe and documented.
4. CI includes strict lint, unit/integration tests, and coverage gates passing consistently.
5. User-facing docs include compatibility matrix and operational guidance equivalent in depth to postgres/sqlite extension docs.

### 18.6 Implementation Status (2026-02-23)

Current execution status against this plan:

- Phase A (`ATTACH ... TYPE d1` skeleton): Completed.
- Phase B (catalog/schema discovery + cache): Completed for single-schema (`main`) D1 catalog.
- Phase C (attached scan + projection/filter pushdown): Completed for common filters (`=`, `<>`, `<`, `<=`, `>`, `>=`, `IN`, `IS NULL`, `IS NOT NULL`, conjunctions) with rowid support.
- Phase D (attached DML): Completed for `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE AS` with explicit unsupported-surface errors (`RETURNING`, `SET DEFAULT`, `ON CONFLICT`).
- Phase E (attached DDL): Completed for `CREATE TABLE/VIEW/INDEX`, `ALTER TABLE` subset, `DROP TABLE/VIEW/INDEX` with cache invalidation.
- Phase F (transaction model): Implemented explicit D1 transaction manager with statement-scoped semantics.
- Phase G (utility parity): Partially completed via helper function surface (`d1_tables`, `d1_scan`, `d1_attach`); advanced maintenance helpers remain optional future work.
- Phase H (hardening/release readiness): In progress; strict lint, SQL/integration tests, and coverage gate are passing.

Quality gates verified in workspace:

- `make test`: pass
- `make integration-test`: pass
- `make lint-strict`: pass
- `make coverage`: pass (`lines: 80.1% (2114/2640)`)
