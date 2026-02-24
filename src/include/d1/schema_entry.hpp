#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include <chrono>

namespace duckdb {
namespace d1 {

class D1TableEntry;

class D1SchemaEntry : public SchemaCatalogEntry {
public:
	D1SchemaEntry(Catalog &catalog, CreateSchemaInfo &info, idx_t cache_ttl_seconds);

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	void InvalidateCache();

private:
	void EnsureCacheFresh(ClientContext &context);
	void RefreshFromRemote(ClientContext &context);
	void RegisterOrReplaceEntry(unique_ptr<CatalogEntry> entry);

	optional_ptr<CatalogEntry> LookupCachedEntry(const string &name);
	void EraseCachedEntry(const string &name);

private:
	mutex cache_lock;
	case_insensitive_map_t<unique_ptr<CatalogEntry>> entries;
	idx_t cache_ttl_seconds;
	std::chrono::steady_clock::time_point last_refresh;
	bool loaded = false;
};

} // namespace d1
} // namespace duckdb
