#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace d1 {

struct D1Config {
	string endpoint;
	string account_id;
	string database_id;
	string api_token;

	void Validate() const;
};

struct D1StatementMeta {
	idx_t statement_index = 0;
	bool success = true;
	bool changed_db = false;
	double changes = 0;
	double duration = 0;
	double last_row_id = 0;
	double rows_read = 0;
	double rows_written = 0;
	bool served_by_primary = false;
	string served_by_region;
	string served_by_colo;
	double size_after = 0;
	double sql_duration_ms = 0;
	string error;
};

struct D1TabularResult {
	vector<string> columns;
	vector<vector<Value>> rows;
};

struct D1StatementRequest {
	string sql;
	vector<Value> params;
};

} // namespace d1
} // namespace duckdb
