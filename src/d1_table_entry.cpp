#include "d1/table_entry.hpp"

#include "d1/catalog.hpp"
#include "d1/scan.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"

namespace duckdb {
namespace d1 {

D1TableEntry::D1TableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, D1Config config,
                           bool is_view)
    : TableCatalogEntry(catalog, schema, info), config(std::move(config)), is_view(is_view) {
}

unique_ptr<BaseStatistics> D1TableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	(void)context;
	(void)column_id;
	return nullptr;
}

TableFunction D1TableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	(void)context;
	vector<string> names;
	vector<LogicalType> types;
	for (auto &column : columns.Logical()) {
		names.push_back(column.GetName());
		types.push_back(column.GetType());
	}
	bind_data = make_uniq<D1AttachedScanBindData>(config, name, std::move(names), std::move(types), is_view, this);
	return D1AttachedScanFunction();
}

TableStorageInfo D1TableEntry::GetStorageInfo(ClientContext &context) {
	(void)context;
	TableStorageInfo info;
	info.cardinality = 10000;
	for (auto &constraint : constraints) {
		if (constraint->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto &unique = constraint->Cast<UniqueConstraint>();
		IndexInfo index_info;
		index_info.is_unique = true;
		index_info.is_primary = unique.IsPrimaryKey();
		index_info.is_foreign = false;
		if (unique.HasIndex()) {
			index_info.column_set.insert(unique.GetIndex().index);
		} else {
			for (auto &column_name : unique.GetColumnNames()) {
				index_info.column_set.insert(columns.GetColumn(column_name).Logical().index);
			}
		}
		info.index_info.push_back(std::move(index_info));
	}
	return info;
}

void D1TableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                         LogicalUpdate &update, ClientContext &context) {
	(void)binder;
	(void)get;
	(void)proj;
	(void)update;
	(void)context;
}

} // namespace d1
} // namespace duckdb
