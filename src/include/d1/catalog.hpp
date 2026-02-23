#pragma once

#include "d1/types.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {
namespace d1 {

class D1SchemaEntry;

class D1Catalog : public Catalog {
public:
	D1Catalog(AttachedDatabase &db, D1Config config, AccessMode access_mode, idx_t schema_cache_ttl_seconds);
	~D1Catalog() override;

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "d1";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override;

	const D1Config &GetConfig() const {
		return config;
	}
	AccessMode GetAccessMode() const {
		return access_mode;
	}
	bool IsReadOnlyCatalog() const {
		return access_mode == AccessMode::READ_ONLY;
	}
	idx_t GetSchemaCacheTTLSeconds() const {
		return schema_cache_ttl_seconds;
	}

	void InvalidateSchemaCache();

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	D1Config config;
	AccessMode access_mode;
	idx_t schema_cache_ttl_seconds;
	unique_ptr<D1SchemaEntry> main_schema;
};

} // namespace d1
} // namespace duckdb
