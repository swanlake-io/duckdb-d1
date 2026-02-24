#include "d1/schema_entry.hpp"

#include "d1/catalog.hpp"
#include "d1/client.hpp"
#include "d1/sql_utils.hpp"
#include "d1/table_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

#include <algorithm>

namespace duckdb {
namespace d1 {

namespace {

struct RemoteColumnInfo {
	string name;
	LogicalType type;
	idx_t pk_position = 0;
	bool not_null = false;
};

D1Catalog &GetD1Catalog(Catalog &catalog) {
	return catalog.Cast<D1Catalog>();
}

void EnsureWriteAllowed(D1Catalog &catalog) {
	if (catalog.IsReadOnlyCatalog()) {
		throw BinderException("D1 catalog is attached in READ_ONLY mode");
	}
}

void ThrowOnStatementFailure(const vector<D1StatementMeta> &metas, const string &sql) {
	for (auto &meta : metas) {
		if (!meta.success) {
			if (!meta.error.empty()) {
				throw IOException("D1 statement failed for SQL \"%s\": %s", sql, meta.error);
			}
			throw IOException("D1 statement failed for SQL \"%s\"", sql);
		}
	}
}

void ExecuteDDL(D1Catalog &catalog, const string &sql) {
	D1Client client(catalog.GetConfig());
	auto metas = client.Execute(sql, {});
	ThrowOnStatementFailure(metas, sql);
}

idx_t FindColumnIndex(const D1TabularResult &result, const string &name) {
	for (idx_t i = 0; i < result.columns.size(); i++) {
		if (StringUtil::CIEquals(result.columns[i], name)) {
			return i;
		}
	}
	throw IOException("D1 metadata response is missing required column \"%s\"", name);
}

bool TryFindColumnIndex(const D1TabularResult &result, const string &name, idx_t &out) {
	for (idx_t i = 0; i < result.columns.size(); i++) {
		if (StringUtil::CIEquals(result.columns[i], name)) {
			out = i;
			return true;
		}
	}
	return false;
}

LogicalType ProbeValueType(const D1TabularResult &result, idx_t column_index) {
	for (auto &row : result.rows) {
		if (column_index >= row.size()) {
			continue;
		}
		if (row[column_index].IsNull()) {
			continue;
		}
		return LogicalTypeToSQLiteType(row[column_index].type());
	}
	return LogicalType::VARCHAR;
}

bool TryLoadRemoteColumnsFromPragma(D1Catalog &catalog, const string &table_name, vector<RemoteColumnInfo> &columns) {
	D1Client client(catalog.GetConfig());
	auto sql = StringUtil::Format("PRAGMA table_info(%s)", QuoteSQLString(table_name));

	D1TabularResult result;
	try {
		result = client.Query(sql, {}, true);
	} catch (const Exception &) {
		// Some environments block PRAGMA metadata introspection. Defer to SELECT-based probing.
		return false;
	}

	idx_t name_idx = DConstants::INVALID_INDEX;
	idx_t type_idx = DConstants::INVALID_INDEX;
	idx_t notnull_idx = DConstants::INVALID_INDEX;
	idx_t pk_idx = DConstants::INVALID_INDEX;
	if (!TryFindColumnIndex(result, "name", name_idx) || !TryFindColumnIndex(result, "type", type_idx) ||
	    !TryFindColumnIndex(result, "notnull", notnull_idx) || !TryFindColumnIndex(result, "pk", pk_idx)) {
		return false;
	}

	columns.clear();
	columns.reserve(result.rows.size());
	for (auto &row : result.rows) {
		if (name_idx >= row.size() || row[name_idx].IsNull()) {
			continue;
		}
		RemoteColumnInfo info;
		info.name = row[name_idx].ToString();
		if (type_idx < row.size() && !row[type_idx].IsNull()) {
			info.type = SQLiteTypeToLogicalType(row[type_idx].ToString());
		} else {
			info.type = LogicalType::VARCHAR;
		}
		if (notnull_idx < row.size() && !row[notnull_idx].IsNull()) {
			Value not_null_cast;
			if (row[notnull_idx].DefaultTryCastAs(LogicalType::BIGINT, not_null_cast, nullptr)) {
				info.not_null = not_null_cast.GetValue<int64_t>() != 0;
			}
		}
		if (pk_idx < row.size() && !row[pk_idx].IsNull()) {
			Value pk_cast;
			if (row[pk_idx].DefaultTryCastAs(LogicalType::BIGINT, pk_cast, nullptr)) {
				auto pk_val = pk_cast.GetValue<int64_t>();
				if (pk_val > 0) {
					info.pk_position = static_cast<idx_t>(pk_val);
				}
			}
		}
		columns.push_back(std::move(info));
	}
	return !columns.empty();
}

vector<RemoteColumnInfo> LoadRemoteColumnsFromProbe(D1Catalog &catalog, const string &table_name) {
	D1Client client(catalog.GetConfig());
	auto sql = StringUtil::Format("SELECT * FROM %s LIMIT 1", QuoteMultipartIdentifier(table_name));
	auto result = client.Query(sql, {}, true);
	if (result.columns.empty()) {
		throw IOException("D1 metadata probing failed for \"%s\": response has no columns", table_name);
	}

	vector<RemoteColumnInfo> columns;
	columns.reserve(result.columns.size());
	for (idx_t i = 0; i < result.columns.size(); i++) {
		RemoteColumnInfo info;
		info.name = result.columns[i];
		if (info.name.empty()) {
			info.name = StringUtil::Format("column_%llu", i + 1);
		}
		info.type = ProbeValueType(result, i);
		columns.push_back(std::move(info));
	}
	return columns;
}

vector<RemoteColumnInfo> LoadRemoteColumns(D1Catalog &catalog, const string &table_name) {
	vector<RemoteColumnInfo> columns;
	if (TryLoadRemoteColumnsFromPragma(catalog, table_name, columns)) {
		return columns;
	}
	return LoadRemoteColumnsFromProbe(catalog, table_name);
}

unique_ptr<CreateTableInfo> BuildCreateTableInfo(const string &catalog_name, const string &schema_name,
                                                 const string &table_name, const vector<RemoteColumnInfo> &columns) {
	auto info = make_uniq<CreateTableInfo>(catalog_name, schema_name, table_name);
	vector<pair<idx_t, string>> primary_key_columns;
	for (idx_t i = 0; i < columns.size(); i++) {
		auto &column = columns[i];
		info->columns.AddColumn(ColumnDefinition(column.name, column.type));
		if (column.not_null) {
			info->constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(i)));
		}
		if (column.pk_position > 0) {
			primary_key_columns.emplace_back(column.pk_position, column.name);
		}
	}
	if (!primary_key_columns.empty()) {
		std::sort(
		    primary_key_columns.begin(), primary_key_columns.end(),
		    [&](const pair<idx_t, string> &lhs, const pair<idx_t, string> &rhs) { return lhs.first < rhs.first; });
		vector<string> pk_names;
		pk_names.reserve(primary_key_columns.size());
		for (auto &entry : primary_key_columns) {
			pk_names.push_back(entry.second);
		}
		if (pk_names.size() == 1) {
			auto index = info->columns.GetColumn(pk_names[0]).Logical();
			info->constraints.push_back(make_uniq<UniqueConstraint>(index, pk_names[0], true));
		} else {
			info->constraints.push_back(make_uniq<UniqueConstraint>(pk_names, true));
		}
	}
	return info;
}

unique_ptr<CreateTableInfo> BuildCreateTableInfoFromBound(BoundCreateTableInfo &bound_info) {
	auto &base = bound_info.Base();
	auto info = make_uniq<CreateTableInfo>(base.catalog, base.schema, base.table);
	for (auto &column : base.columns.Logical()) {
		info->columns.AddColumn(column.Copy());
	}
	for (auto &constraint : base.constraints) {
		info->constraints.push_back(constraint->Copy());
	}
	info->on_conflict = base.on_conflict;
	return info;
}

unique_ptr<D1TableEntry> LoadRemoteTableEntry(D1SchemaEntry &schema, D1Catalog &catalog, const string &table_name,
                                              bool is_view) {
	auto columns = LoadRemoteColumns(catalog, table_name);
	auto create_info = BuildCreateTableInfo(catalog.GetName(), schema.name, table_name, columns);
	return make_uniq<D1TableEntry>(catalog, schema, *create_info, catalog.GetConfig(), is_view);
}

string BuildCreateTableSQL(CreateTableInfo &info) {
	for (idx_t i = 0; i < info.columns.LogicalColumnCount(); i++) {
		auto &column = info.columns.GetColumnMutable(LogicalIndex(i));
		column.SetType(LogicalTypeToSQLiteType(column.GetType()));
	}
	string sql = "CREATE TABLE ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		sql += "IF NOT EXISTS ";
	}
	sql += QuoteIdentifierPart(info.table);
	sql += TableCatalogEntry::ColumnsToSQL(info.columns, info.constraints);
	return sql;
}

void UnqualifyColumnReferences(ParsedExpression &expr) {
	if (expr.type == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		auto base_name = std::move(colref.column_names.back());
		colref.column_names = {std::move(base_name)};
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, UnqualifyColumnReferences);
}

string BuildCreateIndexSQL(CreateIndexInfo &info, TableCatalogEntry &table) {
	string sql = "CREATE";
	if (info.constraint_type == IndexConstraintType::UNIQUE) {
		sql += " UNIQUE";
	}
	sql += " INDEX ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		sql += "IF NOT EXISTS ";
	}
	sql += QuoteIdentifierPart(info.index_name);
	sql += " ON ";
	sql += QuoteIdentifierPart(table.name);
	sql += "(";
	for (idx_t i = 0; i < info.parsed_expressions.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		UnqualifyColumnReferences(*info.parsed_expressions[i]);
		sql += info.parsed_expressions[i]->ToString();
	}
	sql += ")";
	return sql;
}

string BuildCreateViewSQL(CreateViewInfo &info) {
	string sql = "CREATE VIEW ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		sql += "IF NOT EXISTS ";
	}
	sql += QuoteIdentifierPart(info.view_name);
	if (!info.aliases.empty()) {
		sql += "(";
		for (idx_t i = 0; i < info.aliases.size(); i++) {
			if (i > 0) {
				sql += ", ";
			}
			sql += QuoteIdentifierPart(info.aliases[i]);
		}
		sql += ") ";
	} else {
		sql += " ";
	}
	sql += "AS ";
	sql += info.query->ToString();
	return sql;
}

} // namespace

D1SchemaEntry::D1SchemaEntry(Catalog &catalog, CreateSchemaInfo &info, idx_t cache_ttl_seconds)
    : SchemaCatalogEntry(catalog, info), cache_ttl_seconds(cache_ttl_seconds),
      last_refresh(std::chrono::steady_clock::time_point::min()) {
}

void D1SchemaEntry::InvalidateCache() {
	lock_guard<mutex> guard(cache_lock);
	entries.clear();
	loaded = false;
	last_refresh = std::chrono::steady_clock::time_point::min();
}

void D1SchemaEntry::EnsureCacheFresh(ClientContext &context) {
	bool refresh = false;
	{
		lock_guard<mutex> guard(cache_lock);
		if (!loaded) {
			refresh = true;
		} else if (cache_ttl_seconds == 0) {
			refresh = true;
		} else {
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_refresh).count();
			refresh = elapsed >= static_cast<int64_t>(cache_ttl_seconds);
		}
	}
	if (refresh) {
		RefreshFromRemote(context);
	}
}

void D1SchemaEntry::RefreshFromRemote(ClientContext &context) {
	auto &d1_catalog = GetD1Catalog(catalog);
	D1Client client(d1_catalog.GetConfig());
	auto sql = "SELECT name, type, sql FROM sqlite_master "
	           "WHERE name NOT LIKE 'sqlite_%' "
	           "AND name NOT GLOB '_cf_*' "
	           "AND type IN ('table', 'view') "
	           "ORDER BY name";
	auto result = client.Query(sql, {}, true);
	auto name_idx = FindColumnIndex(result, "name");
	auto type_idx = FindColumnIndex(result, "type");

	case_insensitive_map_t<unique_ptr<CatalogEntry>> new_entries;
	for (auto &row : result.rows) {
		if (name_idx >= row.size() || row[name_idx].IsNull()) {
			continue;
		}
		auto object_name = row[name_idx].ToString();
		if (object_name.empty()) {
			continue;
		}
		if (StringUtil::StartsWith(object_name, "_cf_")) {
			continue;
		}
		bool is_view = false;
		if (type_idx < row.size() && !row[type_idx].IsNull()) {
			is_view = StringUtil::CIEquals(row[type_idx].ToString(), "view");
		}
		auto entry = LoadRemoteTableEntry(*this, d1_catalog, object_name, is_view);
		new_entries[object_name] = std::move(entry);
	}

	lock_guard<mutex> guard(cache_lock);
	entries = std::move(new_entries);
	loaded = true;
	last_refresh = std::chrono::steady_clock::now();
}

void D1SchemaEntry::RegisterOrReplaceEntry(unique_ptr<CatalogEntry> entry) {
	lock_guard<mutex> guard(cache_lock);
	entries[entry->name] = std::move(entry);
	loaded = true;
	last_refresh = std::chrono::steady_clock::now();
}

optional_ptr<CatalogEntry> D1SchemaEntry::LookupCachedEntry(const string &name) {
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

void D1SchemaEntry::EraseCachedEntry(const string &name) {
	lock_guard<mutex> guard(cache_lock);
	auto entry = entries.find(name);
	if (entry != entries.end()) {
		entries.erase(entry);
	}
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	auto &d1_catalog = GetD1Catalog(catalog);
	EnsureWriteAllowed(d1_catalog);
	auto &context = transaction.GetContext();
	EnsureCacheFresh(context);
	auto table_name = info.Base().table;
	if (auto existing = LookupCachedEntry(table_name)) {
		switch (info.Base().on_conflict) {
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return existing;
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			ExecuteDDL(d1_catalog, StringUtil::Format("DROP TABLE IF EXISTS %s", QuoteIdentifierPart(table_name)));
			EraseCachedEntry(table_name);
			break;
		case OnCreateConflict::ERROR_ON_CONFLICT:
		default:
			throw BinderException("Failed to create D1 table \"%s\": table already exists", table_name);
		}
	}

	auto create_sql_info = unique_ptr_cast<CreateInfo, CreateTableInfo>(info.Base().Copy());
	auto create_sql = BuildCreateTableSQL(create_sql_info->Cast<CreateTableInfo>());
	ExecuteDDL(d1_catalog, create_sql);

	auto table_info = BuildCreateTableInfoFromBound(info);
	auto entry = make_uniq<D1TableEntry>(catalog, *this, *table_info, d1_catalog.GetConfig(), false);
	auto entry_ptr = entry.get();
	RegisterOrReplaceEntry(std::move(entry));
	return entry_ptr;
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs do not support creating functions");
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                      TableCatalogEntry &table) {
	auto &d1_catalog = GetD1Catalog(catalog);
	EnsureWriteAllowed(d1_catalog);
	auto sql = BuildCreateIndexSQL(info, table);
	ExecuteDDL(d1_catalog, sql);
	return nullptr;
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	auto &d1_catalog = GetD1Catalog(catalog);
	EnsureWriteAllowed(d1_catalog);
	if (info.sql.empty()) {
		throw BinderException("Cannot create D1 view from empty SQL statement");
	}
	auto sql = BuildCreateViewSQL(info);
	ExecuteDDL(d1_catalog, sql);
	InvalidateCache();
	return nullptr;
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs do not support creating sequences");
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                              CreateTableFunctionInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs do not support creating table functions");
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                             CreateCopyFunctionInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs do not support creating copy functions");
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                               CreatePragmaFunctionInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs do not support creating pragma functions");
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs do not support creating collations");
}

optional_ptr<CatalogEntry> D1SchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs do not support creating types");
}

void D1SchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	auto &d1_catalog = GetD1Catalog(catalog);
	EnsureWriteAllowed(d1_catalog);
	if (info.type != AlterType::ALTER_TABLE) {
		throw BinderException("Only ALTER TABLE is supported for D1 catalogs");
	}
	auto &alter = info.Cast<AlterTableInfo>();
	string sql;
	switch (alter.alter_table_type) {
	case AlterTableType::RENAME_TABLE: {
		auto &rename_info = alter.Cast<RenameTableInfo>();
		sql = StringUtil::Format("ALTER TABLE %s RENAME TO %s", QuoteIdentifierPart(rename_info.name),
		                         QuoteIdentifierPart(rename_info.new_table_name));
		break;
	}
	case AlterTableType::RENAME_COLUMN: {
		auto &rename_column = alter.Cast<RenameColumnInfo>();
		sql = StringUtil::Format("ALTER TABLE %s RENAME COLUMN %s TO %s", QuoteIdentifierPart(rename_column.name),
		                         QuoteIdentifierPart(rename_column.old_name),
		                         QuoteIdentifierPart(rename_column.new_name));
		break;
	}
	case AlterTableType::ADD_COLUMN: {
		auto &add_column = alter.Cast<AddColumnInfo>();
		sql = StringUtil::Format("ALTER TABLE %s ADD COLUMN %s %s", QuoteIdentifierPart(add_column.name),
		                         QuoteIdentifierPart(add_column.new_column.Name()),
		                         LogicalTypeToSQLiteType(add_column.new_column.Type()).ToString());
		break;
	}
	case AlterTableType::REMOVE_COLUMN: {
		auto &remove_column = alter.Cast<RemoveColumnInfo>();
		sql = StringUtil::Format("ALTER TABLE %s DROP COLUMN %s", QuoteIdentifierPart(remove_column.name),
		                         QuoteIdentifierPart(remove_column.removed_column));
		break;
	}
	default:
		throw BinderException("Unsupported ALTER TABLE operation for D1 catalogs");
	}
	ExecuteDDL(d1_catalog, sql);
	InvalidateCache();
}

void D1SchemaEntry::Scan(ClientContext &context, CatalogType type,
                         const std::function<void(CatalogEntry &)> &callback) {
	EnsureCacheFresh(context);
	if (type != CatalogType::TABLE_ENTRY && type != CatalogType::VIEW_ENTRY) {
		return;
	}
	lock_guard<mutex> guard(cache_lock);
	for (auto &entry : entries) {
		callback(*entry.second);
	}
}

void D1SchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	(void)type;
	(void)callback;
	throw NotImplementedException("D1SchemaEntry::Scan without context is not supported");
}

void D1SchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	auto &d1_catalog = GetD1Catalog(catalog);
	EnsureWriteAllowed(d1_catalog);
	string type_name;
	switch (info.type) {
	case CatalogType::TABLE_ENTRY:
		type_name = "TABLE";
		break;
	case CatalogType::VIEW_ENTRY:
		type_name = "VIEW";
		break;
	case CatalogType::INDEX_ENTRY:
		type_name = "INDEX";
		break;
	default:
		throw BinderException("Unsupported DROP entry type for D1 catalog");
	}
	string sql = "DROP " + type_name + " ";
	if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
		sql += "IF EXISTS ";
	}
	sql += QuoteIdentifierPart(info.name);
	ExecuteDDL(d1_catalog, sql);
	EraseCachedEntry(info.name);
}

optional_ptr<CatalogEntry> D1SchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                      const EntryLookupInfo &lookup_info) {
	auto catalog_type = lookup_info.GetCatalogType();
	if (catalog_type != CatalogType::TABLE_ENTRY && catalog_type != CatalogType::VIEW_ENTRY &&
	    catalog_type != CatalogType::INDEX_ENTRY) {
		return nullptr;
	}
	auto &context = transaction.GetContext();
	EnsureCacheFresh(context);
	lock_guard<mutex> guard(cache_lock);
	auto entry = entries.find(lookup_info.GetEntryName());
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

} // namespace d1
} // namespace duckdb
