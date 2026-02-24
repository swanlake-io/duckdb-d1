#pragma once

#include "d1/types.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {
namespace d1 {

struct D1AttachedScanBindData : public FunctionData {
	D1AttachedScanBindData(D1Config config, string table_name, vector<string> column_names,
	                       vector<LogicalType> column_types, bool is_view, optional_ptr<TableCatalogEntry> table)
	    : config(std::move(config)), table_name(std::move(table_name)), column_names(std::move(column_names)),
	      column_types(std::move(column_types)), is_view(is_view), table(table) {
	}

	D1Config config;
	string table_name;
	vector<string> column_names;
	vector<LogicalType> column_types;
	bool is_view;
	optional_ptr<TableCatalogEntry> table;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;
};

class D1AttachedScanFunction : public TableFunction {
public:
	D1AttachedScanFunction();
};

} // namespace d1
} // namespace duckdb
