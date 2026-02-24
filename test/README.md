# Testing this extension
This directory contains all tests for the extension. The `sql` directory holds [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html), and all validation is kept in these `.test` files.

`test/sql/d1.test` expects live D1 credentials:
- `D1_ACCOUNT_ID`
- `D1_DATABASE_ID`
- `D1_API_TOKEN`
- optional `D1_ENDPOINT` (defaults to `https://api.cloudflare.com/client/v4`)

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

To run the live SQL suite directly:
```bash
make integration-test
```
