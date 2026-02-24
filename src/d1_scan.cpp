#include "d1/scan.hpp"

#include "d1/client.hpp"
#include "d1/sql_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/filter/struct_filter.hpp"

namespace duckdb {
namespace d1 {

namespace {

struct D1AttachedScanGlobalState : public GlobalTableFunctionState {
	D1AttachedScanGlobalState() {
	}

	vector<vector<Value>> rows;
	vector<LogicalType> output_types;
	idx_t offset = 0;
	mutex lock;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static Value CastOutputValue(const Value &value, const LogicalType &target_type) {
	if (value.IsNull()) {
		return Value(target_type);
	}
	if (value.type() == target_type) {
		return value;
	}
	if (target_type.id() == LogicalTypeId::VARCHAR) {
		return Value(value.ToString());
	}
	Value casted;
	if (value.DefaultTryCastAs(target_type, casted, nullptr)) {
		return casted;
	}
	throw InvalidInputException("Failed to cast D1 value \"%s\" from %s to %s", value.ToString(),
	                            value.type().ToString(), target_type.ToString());
}

static string ComparisonToSQL(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "<>";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		throw InvalidInputException("Unsupported comparison expression for D1 pushdown");
	}
}

static string TransformFilter(const string &column_name, TableFilter &filter, column_t column_id);

static string CombineFilters(const string &column_name, vector<unique_ptr<TableFilter>> &filters, const string &op,
                             column_t column_id) {
	vector<string> parts;
	for (auto &child : filters) {
		auto text = TransformFilter(column_name, *child, column_id);
		if (!text.empty()) {
			parts.push_back(std::move(text));
		}
	}
	if (parts.empty()) {
		return string();
	}
	return "(" + StringUtil::Join(parts, " " + op + " ") + ")";
}

static string TransformFilter(const string &column_name, TableFilter &filter, column_t column_id) {
	(void)column_id;
	switch (filter.filter_type) {
	case TableFilterType::IS_NULL:
		return column_name + " IS NULL";
	case TableFilterType::IS_NOT_NULL:
		return column_name + " IS NOT NULL";
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		return CombineFilters(column_name, conjunction.child_filters, "AND", column_id);
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction = filter.Cast<ConjunctionOrFilter>();
		return CombineFilters(column_name, conjunction.child_filters, "OR", column_id);
	}
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &comparison = filter.Cast<ConstantFilter>();
		if (comparison.constant.IsNull()) {
			if (comparison.comparison_type == ExpressionType::COMPARE_EQUAL) {
				return column_name + " IS NULL";
			}
			if (comparison.comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
				return column_name + " IS NOT NULL";
			}
			return "FALSE";
		}
		auto op = ComparisonToSQL(comparison.comparison_type);
		return StringUtil::Format("%s %s %s", column_name, op, ValueToSQLLiteral(comparison.constant));
	}
	case TableFilterType::IN_FILTER: {
		auto &in_filter = filter.Cast<InFilter>();
		if (in_filter.values.empty()) {
			return "FALSE";
		}
		vector<string> values;
		values.reserve(in_filter.values.size());
		for (auto &entry : in_filter.values) {
			values.push_back(ValueToSQLLiteral(entry));
		}
		return column_name + " IN (" + StringUtil::Join(values, ", ") + ")";
	}
	case TableFilterType::STRUCT_EXTRACT: {
		auto &struct_filter = filter.Cast<StructFilter>();
		auto field = QuoteIdentifierPart(struct_filter.child_name);
		auto nested_name = "(" + column_name + ")." + field;
		return TransformFilter(nested_name, *struct_filter.child_filter, column_id);
	}
	case TableFilterType::OPTIONAL_FILTER: {
		auto &optional_filter = filter.Cast<OptionalFilter>();
		return TransformFilter(column_name, *optional_filter.child_filter, column_id);
	}
	case TableFilterType::DYNAMIC_FILTER:
		return string();
	default:
		throw InvalidInputException("Unsupported pushed filter for D1 table scan");
	}
}

static string BuildFilterSQL(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
                             const vector<string> &column_names) {
	if (!filters || filters->filters.empty()) {
		return string();
	}
	vector<string> clauses;
	for (auto &entry : filters->filters) {
		auto filter_idx = entry.first;
		if (filter_idx >= column_ids.size()) {
			throw InvalidInputException("Invalid filter column index in D1 scan");
		}
		auto column_id = column_ids[filter_idx];
		string column_name;
		if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
			column_name = "rowid";
		} else {
			if (column_id >= column_names.size()) {
				throw InvalidInputException("Invalid filter column id in D1 scan");
			}
			column_name = QuoteIdentifierPart(column_names[column_id]);
		}
		auto filter_sql = TransformFilter(column_name, *entry.second, column_id);
		if (!filter_sql.empty()) {
			clauses.push_back(std::move(filter_sql));
		}
	}
	if (clauses.empty()) {
		return string();
	}
	return StringUtil::Join(clauses, " AND ");
}

static unique_ptr<GlobalTableFunctionState> D1AttachedInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<D1AttachedScanBindData>();
	auto result = make_uniq<D1AttachedScanGlobalState>();

	vector<string> projected_sql;
	result->output_types.reserve(input.column_ids.size());
	projected_sql.reserve(input.column_ids.size());
	for (auto column_id : input.column_ids) {
		if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
			projected_sql.push_back("rowid");
			result->output_types.push_back(LogicalType::ROW_TYPE);
			continue;
		}
		if (column_id >= bind_data.column_names.size()) {
			throw InvalidInputException("Invalid projected column id %llu for D1 table scan", column_id);
		}
		projected_sql.push_back(QuoteIdentifierPart(bind_data.column_names[column_id]));
		result->output_types.push_back(bind_data.column_types[column_id]);
	}

	auto query = StringUtil::Format("SELECT %s FROM %s", StringUtil::Join(projected_sql, ", "),
	                                QuoteMultipartIdentifier(bind_data.table_name));
	auto filter_sql = BuildFilterSQL(input.column_ids, input.filters, bind_data.column_names);
	if (!filter_sql.empty()) {
		query += " WHERE " + filter_sql;
	}

	D1Client client(bind_data.config);
	auto rows = client.Query(query, {}, true);
	for (auto &row : rows.rows) {
		if (row.size() < result->output_types.size()) {
			row.resize(result->output_types.size());
		}
		result->rows.push_back(std::move(row));
	}
	return std::move(result);
}

static void D1AttachedScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	(void)context;
	auto &state = data.global_state->Cast<D1AttachedScanGlobalState>();

	idx_t start = 0;
	idx_t count = 0;
	{
		lock_guard<mutex> guard(state.lock);
		if (state.offset >= state.rows.size()) {
			output.SetCardinality(0);
			return;
		}
		start = state.offset;
		count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
		state.offset += count;
	}

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto &row = state.rows[start + row_idx];
		for (idx_t col_idx = 0; col_idx < output.ColumnCount(); col_idx++) {
			auto value = col_idx < row.size() ? row[col_idx] : Value();
			output.SetValue(col_idx, row_idx, CastOutputValue(value, state.output_types[col_idx]));
		}
	}
	output.SetCardinality(count);
}

static BindInfo D1AttachedBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<D1AttachedScanBindData>();
	if (bind_data.table) {
		auto &table_ref = const_cast<TableCatalogEntry &>(*bind_data.table);
		return BindInfo(table_ref);
	}
	return BindInfo(ScanType::EXTERNAL);
}

static InsertionOrderPreservingMap<string> D1AttachedToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<D1AttachedScanBindData>();
	result["Table"] = bind_data.table_name;
	result["Type"] = bind_data.is_view ? "VIEW" : "TABLE";
	return result;
}

} // namespace

unique_ptr<FunctionData> D1AttachedScanBindData::Copy() const {
	return make_uniq<D1AttachedScanBindData>(config, table_name, column_names, column_types, is_view, table);
}

bool D1AttachedScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<D1AttachedScanBindData>();
	return config.endpoint == other.config.endpoint && config.account_id == other.config.account_id &&
	       config.database_id == other.config.database_id && config.api_token == other.config.api_token &&
	       table_name == other.table_name && column_names == other.column_names && column_types == other.column_types &&
	       is_view == other.is_view;
}

D1AttachedScanFunction::D1AttachedScanFunction() : TableFunction("d1_attached_scan", {}, D1AttachedScan) {
	init_global = D1AttachedInitGlobal;
	projection_pushdown = true;
	filter_pushdown = true;
	get_bind_info = D1AttachedBindInfo;
	to_string = D1AttachedToString;
}

} // namespace d1
} // namespace duckdb
