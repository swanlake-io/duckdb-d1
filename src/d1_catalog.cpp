#include "d1/catalog.hpp"

#include "d1/client.hpp"
#include "d1/schema_entry.hpp"
#include "d1/sql_utils.hpp"
#include "d1/table_entry.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {
namespace d1 {

namespace {

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

class D1Insert : public PhysicalOperator {
public:
	D1Insert(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table,
	         physical_index_vector_t<idx_t> column_index_map)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(&table), schema(nullptr),
	      column_index_map(std::move(column_index_map)) {
	}

	D1Insert(PhysicalPlan &physical_plan, LogicalOperator &op, SchemaCatalogEntry &schema,
	         unique_ptr<BoundCreateTableInfo> info)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(nullptr),
	      schema(&schema), info(std::move(info)) {
	}

	optional_ptr<TableCatalogEntry> table;
	optional_ptr<SchemaCatalogEntry> schema;
	unique_ptr<BoundCreateTableInfo> info;
	physical_index_vector_t<idx_t> column_index_map;

	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override {
		return table ? "D1_INSERT" : "D1_CREATE_TABLE_AS";
	}
	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["Table Name"] = table ? table->name : info->Base().table;
		return result;
	}
};

class D1Delete : public PhysicalOperator {
public:
	D1Delete(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table, idx_t row_id_index)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(table),
	      row_id_index(row_id_index) {
	}

	TableCatalogEntry &table;
	idx_t row_id_index;

	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override {
		return "D1_DELETE";
	}
	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["Table Name"] = table.name;
		return result;
	}
};

class D1Update : public PhysicalOperator {
public:
	D1Update(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table, vector<PhysicalIndex> columns)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(table),
	      columns(std::move(columns)) {
	}

	TableCatalogEntry &table;
	vector<PhysicalIndex> columns;

	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override {
		return "D1_UPDATE";
	}
	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["Table Name"] = table.name;
		return result;
	}
};

struct InsertColumnMapping {
	idx_t source_column_index;
	idx_t physical_column_index;
};

class D1InsertGlobalState : public GlobalSinkState {
public:
	D1InsertGlobalState(D1TableEntry &table, string insert_sql, vector<InsertColumnMapping> column_mappings)
	    : table(table), client(table.GetConfig()), insert_sql(std::move(insert_sql)),
	      column_mappings(std::move(column_mappings)) {
	}

	D1TableEntry &table;
	D1Client client;
	string insert_sql;
	vector<InsertColumnMapping> column_mappings;
	idx_t insert_count = 0;
};

class D1DeleteGlobalState : public GlobalSinkState {
public:
	explicit D1DeleteGlobalState(D1TableEntry &table)
	    : table(table), client(table.GetConfig()),
	      delete_sql(StringUtil::Format("DELETE FROM %s WHERE rowid = ?", QuoteIdentifierPart(table.name))) {
	}

	D1TableEntry &table;
	D1Client client;
	string delete_sql;
	idx_t delete_count = 0;
};

class D1UpdateGlobalState : public GlobalSinkState {
public:
	D1UpdateGlobalState(D1TableEntry &table, string update_sql)
	    : table(table), client(table.GetConfig()), update_sql(std::move(update_sql)) {
	}

	D1TableEntry &table;
	D1Client client;
	string update_sql;
	idx_t update_count = 0;
};

vector<InsertColumnMapping> GetInsertColumnMappings(const D1Insert &insert, D1TableEntry &entry) {
	vector<InsertColumnMapping> mappings;
	auto &columns = entry.GetColumns();
	if (insert.column_index_map.empty()) {
		mappings.reserve(columns.LogicalColumnCount());
		for (idx_t c = 0; c < columns.LogicalColumnCount(); c++) {
			mappings.push_back({c, c});
		}
		return mappings;
	}

	mappings.reserve(insert.column_index_map.size());
	vector<PhysicalIndex> physical_to_source;
	physical_to_source.resize(columns.LogicalColumnCount(), PhysicalIndex(DConstants::INVALID_INDEX));
	for (idx_t source_idx = 0; source_idx < insert.column_index_map.size(); source_idx++) {
		auto mapped_idx = insert.column_index_map[PhysicalIndex(source_idx)];
		if (mapped_idx == DConstants::INVALID_INDEX) {
			continue;
		}
		physical_to_source[mapped_idx] = PhysicalIndex(source_idx);
	}
	for (idx_t physical_idx = 0; physical_idx < physical_to_source.size(); physical_idx++) {
		if (physical_to_source[physical_idx].index == DConstants::INVALID_INDEX) {
			continue;
		}
		mappings.push_back({physical_to_source[physical_idx].index, physical_idx});
	}
	return mappings;
}

string BuildInsertSQL(const D1Insert &insert, D1TableEntry &entry, const vector<InsertColumnMapping> &column_mappings) {
	string sql = "INSERT INTO ";
	sql += QuoteIdentifierPart(entry.name);
	if (!insert.column_index_map.empty()) {
		sql += " (";
		bool first = true;
		for (auto &mapping : column_mappings) {
			auto &logical_column = entry.GetColumns().GetColumn(LogicalIndex(mapping.physical_column_index));
			if (!first) {
				sql += ", ";
			}
			first = false;
			sql += QuoteIdentifierPart(logical_column.GetName());
		}
		sql += ")";
	}
	sql += " VALUES (";
	for (idx_t i = 0; i < column_mappings.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += "?";
	}
	sql += ")";
	return sql;
}

unique_ptr<GlobalSinkState> D1Insert::GetGlobalSinkState(ClientContext &context) const {
	optional_ptr<D1TableEntry> insert_table;
	if (!table) {
		auto &schema_ref = *schema.get_mutable();
		auto created = schema_ref.CreateTable(schema_ref.GetCatalogTransaction(context), *info);
		if (!created) {
			throw InternalException("Failed to create D1 table during CREATE TABLE AS");
		}
		insert_table = &created->Cast<D1TableEntry>();
	} else {
		insert_table = &table.get_mutable()->Cast<D1TableEntry>();
	}
	if (insert_table->IsView()) {
		throw BinderException("Cannot insert into D1 view \"%s\"", insert_table->name);
	}
	auto column_mappings = GetInsertColumnMappings(*this, *insert_table);
	auto insert_sql = BuildInsertSQL(*this, *insert_table, column_mappings);
	return make_uniq<D1InsertGlobalState>(*insert_table, std::move(insert_sql), std::move(column_mappings));
}

SinkResultType D1Insert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &state = input.global_state.Cast<D1InsertGlobalState>();
	for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
		vector<Value> params;
		params.reserve(state.column_mappings.size());
		for (auto &mapping : state.column_mappings) {
			params.push_back(chunk.GetValue(mapping.source_column_index, row_idx));
		}
		auto metas = state.client.Execute(state.insert_sql, params);
		ThrowOnStatementFailure(metas, state.insert_sql);
	}
	state.insert_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

SourceResultType D1Insert::GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
	(void)context;
	(void)input;
	auto &state = sink_state->Cast<D1InsertGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(state.insert_count));
	return SourceResultType::FINISHED;
}

unique_ptr<GlobalSinkState> D1Delete::GetGlobalSinkState(ClientContext &context) const {
	(void)context;
	auto &d1_table = table.Cast<D1TableEntry>();
	if (d1_table.IsView()) {
		throw BinderException("Cannot delete from D1 view \"%s\"", d1_table.name);
	}
	return make_uniq<D1DeleteGlobalState>(d1_table);
}

SinkResultType D1Delete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	(void)context;
	auto &state = input.global_state.Cast<D1DeleteGlobalState>();
	chunk.Flatten();
	auto &row_identifiers = chunk.data[row_id_index];
	auto row_data = FlatVector::GetData<row_t>(row_identifiers);
	for (idx_t i = 0; i < chunk.size(); i++) {
		auto metas = state.client.Execute(state.delete_sql, {Value::BIGINT(row_data[i])});
		ThrowOnStatementFailure(metas, state.delete_sql);
	}
	state.delete_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

SourceResultType D1Delete::GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
	(void)context;
	(void)input;
	auto &state = sink_state->Cast<D1DeleteGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(state.delete_count));
	return SourceResultType::FINISHED;
}

string BuildUpdateSQL(D1TableEntry &table, const vector<PhysicalIndex> &columns) {
	string sql = "UPDATE ";
	sql += QuoteIdentifierPart(table.name);
	sql += " SET ";
	for (idx_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		auto &column = table.GetColumn(LogicalIndex(columns[i].index));
		sql += QuoteIdentifierPart(column.GetName());
		sql += " = ?";
	}
	sql += " WHERE rowid = ?";
	return sql;
}

unique_ptr<GlobalSinkState> D1Update::GetGlobalSinkState(ClientContext &context) const {
	(void)context;
	auto &d1_table = table.Cast<D1TableEntry>();
	if (d1_table.IsView()) {
		throw BinderException("Cannot update D1 view \"%s\"", d1_table.name);
	}
	return make_uniq<D1UpdateGlobalState>(d1_table, BuildUpdateSQL(d1_table, columns));
}

SinkResultType D1Update::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	(void)context;
	auto &state = input.global_state.Cast<D1UpdateGlobalState>();
	chunk.Flatten();
	auto &row_identifiers = chunk.data[chunk.ColumnCount() - 1];
	auto row_data = FlatVector::GetData<row_t>(row_identifiers);
	for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
		vector<Value> params;
		params.reserve(chunk.ColumnCount());
		for (idx_t col_idx = 0; col_idx < chunk.ColumnCount() - 1; col_idx++) {
			params.push_back(chunk.GetValue(col_idx, row_idx));
		}
		params.push_back(Value::BIGINT(row_data[row_idx]));
		auto metas = state.client.Execute(state.update_sql, params);
		ThrowOnStatementFailure(metas, state.update_sql);
	}
	state.update_count += chunk.size();
	return SinkResultType::NEED_MORE_INPUT;
}

SourceResultType D1Update::GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const {
	(void)context;
	(void)input;
	auto &state = sink_state->Cast<D1UpdateGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(state.update_count));
	return SourceResultType::FINISHED;
}

class D1CreateIndex : public PhysicalOperator {
public:
	D1CreateIndex(PhysicalPlan &physical_plan, unique_ptr<CreateIndexInfo> info, TableCatalogEntry &table)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, {LogicalType::BIGINT}, 1),
	      info(std::move(info)), table(table) {
	}

	unique_ptr<CreateIndexInfo> info;
	TableCatalogEntry &table;

	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override {
		(void)chunk;
		(void)input;
		auto &schema = table.ParentSchema();
		auto transaction = schema.GetCatalogTransaction(context.client);
		schema.CreateIndex(transaction, *info, table);
		return SourceResultType::FINISHED;
	}

	bool IsSource() const override {
		return true;
	}
};

class LogicalD1CreateIndex : public LogicalExtensionOperator {
public:
	LogicalD1CreateIndex(unique_ptr<CreateIndexInfo> info, TableCatalogEntry &table)
	    : info(std::move(info)), table(table) {
	}

	unique_ptr<CreateIndexInfo> info;
	TableCatalogEntry &table;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		return planner.Make<D1CreateIndex>(std::move(info), table);
	}

	void Serialize(Serializer &writer) const override {
		throw InternalException("Cannot serialize D1 create index");
	}

	void ResolveTypes() override {
		types = {LogicalType::BIGINT};
	}
};

PhysicalOperator &AddCastToD1Types(ClientContext &context, PhysicalPlanGenerator &planner, PhysicalOperator &plan) {
	bool require_cast = false;
	auto &child_types = plan.GetTypes();
	for (auto &type : child_types) {
		auto d1_type = LogicalTypeToSQLiteType(type);
		if (d1_type != type) {
			require_cast = true;
			break;
		}
	}
	if (!require_cast) {
		return plan;
	}

	vector<LogicalType> cast_types;
	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &type = child_types[i];
		unique_ptr<Expression> expr = make_uniq<BoundReferenceExpression>(type, i);
		auto d1_type = LogicalTypeToSQLiteType(type);
		if (d1_type != type) {
			expr = BoundCastExpression::AddCastToType(context, std::move(expr), d1_type);
		}
		cast_types.push_back(d1_type);
		select_list.push_back(std::move(expr));
	}
	auto &projection =
	    planner.Make<PhysicalProjection>(std::move(cast_types), std::move(select_list), plan.estimated_cardinality);
	projection.children.push_back(plan);
	return projection;
}

} // namespace

D1Catalog::D1Catalog(AttachedDatabase &db, D1Config config, AccessMode access_mode, idx_t schema_cache_ttl_seconds)
    : Catalog(db), config(std::move(config)), access_mode(access_mode),
      schema_cache_ttl_seconds(schema_cache_ttl_seconds) {
	this->config.Validate();
}

D1Catalog::~D1Catalog() {
}

void D1Catalog::Initialize(bool load_builtin) {
	(void)load_builtin;
	CreateSchemaInfo info;
	main_schema = make_uniq<D1SchemaEntry>(*this, info, schema_cache_ttl_seconds);
}

optional_ptr<CatalogEntry> D1Catalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	(void)transaction;
	(void)info;
	throw BinderException("D1 catalogs only support a single schema: \"%s\"", DEFAULT_SCHEMA);
}

void D1Catalog::DropSchema(ClientContext &context, DropInfo &info) {
	(void)context;
	(void)info;
	throw BinderException("D1 catalogs do not support dropping schemas");
}

void D1Catalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> D1Catalog::LookupSchema(CatalogTransaction transaction,
                                                         const EntryLookupInfo &schema_lookup,
                                                         OnEntryNotFound if_not_found) {
	auto schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("D1 catalogs only expose schema \"%s\"", DEFAULT_SCHEMA);
}

PhysicalOperator &D1Catalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                        optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING is not supported for insertion into D1 tables");
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw BinderException("ON CONFLICT is not supported for insertion into D1 tables");
	}
	D_ASSERT(plan);
	auto &inner_plan = AddCastToD1Types(context, planner, *plan);
	auto &insert = planner.Make<D1Insert>(op, op.table, op.column_index_map);
	insert.children.push_back(inner_plan);
	return insert;
}

PhysicalOperator &D1Catalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                               LogicalCreateTable &op, PhysicalOperator &plan) {
	auto &inner_plan = AddCastToD1Types(context, planner, plan);
	auto &insert = planner.Make<D1Insert>(op, op.schema, std::move(op.info));
	insert.children.push_back(inner_plan);
	return insert;
}

PhysicalOperator &D1Catalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                        PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING is not supported for deletion from D1 tables");
	}
	auto &bound_ref = op.expressions[0]->Cast<BoundReferenceExpression>();
	auto &delete_op = planner.Make<D1Delete>(op, op.table, bound_ref.index);
	delete_op.children.push_back(plan);
	return delete_op;
}

PhysicalOperator &D1Catalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                        PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw BinderException("RETURNING is not supported for updates of D1 tables");
	}
	for (auto &expr : op.expressions) {
		if (expr->type == ExpressionType::VALUE_DEFAULT) {
			throw BinderException("SET DEFAULT is not supported for updates of D1 tables");
		}
	}
	auto &update_op = planner.Make<D1Update>(op, op.table, std::move(op.columns));
	update_op.children.push_back(plan);
	return update_op;
}

unique_ptr<LogicalOperator> D1Catalog::BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
                                                       unique_ptr<LogicalOperator> plan) {
	(void)binder;
	(void)plan;
	return make_uniq<LogicalD1CreateIndex>(unique_ptr_cast<CreateInfo, CreateIndexInfo>(std::move(stmt.info)), table);
}

DatabaseSize D1Catalog::GetDatabaseSize(ClientContext &context) {
	(void)context;
	DatabaseSize size;
	size.total_blocks = 0;
	size.free_blocks = 0;
	size.used_blocks = 0;
	size.block_size = 0;
	size.bytes = 0;
	size.wal_size = idx_t(-1);
	return size;
}

string D1Catalog::GetDBPath() {
	return StringUtil::Format("%s/accounts/%s/d1/database/%s", config.endpoint, config.account_id, config.database_id);
}

void D1Catalog::InvalidateSchemaCache() {
	if (main_schema) {
		main_schema->InvalidateCache();
	}
}

} // namespace d1
} // namespace duckdb
