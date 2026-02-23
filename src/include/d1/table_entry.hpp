#pragma once

#include "d1/types.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {
namespace d1 {

class D1TableEntry : public TableCatalogEntry {
public:
	D1TableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, D1Config config, bool is_view);

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;
	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	const D1Config &GetConfig() const {
		return config;
	}
	bool IsView() const {
		return is_view;
	}

private:
	D1Config config;
	bool is_view;
};

} // namespace d1
} // namespace duckdb
