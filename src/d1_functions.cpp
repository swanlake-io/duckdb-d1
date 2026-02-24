#include "d1/functions.hpp"

#include "d1/client.hpp"
#include "d1/secret.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"

#include <nlohmann/json.hpp>

#include <mutex>

namespace duckdb {
namespace d1 {

using json = nlohmann::json;

namespace {

struct D1ReadBindData : public FunctionData {
	D1ReadBindData(D1Config config_p, vector<string> names_p, vector<LogicalType> types_p, vector<vector<Value>> rows_p)
	    : config(std::move(config_p)), names(std::move(names_p)), types(std::move(types_p)), rows(std::move(rows_p)) {
	}

	D1Config config;
	vector<string> names;
	vector<LogicalType> types;
	vector<vector<Value>> rows;

	// GCOVR_EXCL_START
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<D1ReadBindData>(config, names, types, rows);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<D1ReadBindData>();
		return config.endpoint == other.config.endpoint && config.account_id == other.config.account_id &&
		       config.database_id == other.config.database_id && config.api_token == other.config.api_token &&
		       names == other.names && types == other.types;
	}
	// GCOVR_EXCL_STOP
};

struct D1ExecuteBindData : public FunctionData {
	D1ExecuteBindData(D1Config config_p, string sql_p, vector<Value> params_p)
	    : config(std::move(config_p)), sql(std::move(sql_p)), params(std::move(params_p)) {
	}

	D1Config config;
	string sql;
	vector<Value> params;

	// GCOVR_EXCL_START
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<D1ExecuteBindData>(config, sql, params);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<D1ExecuteBindData>();
		return config.endpoint == other.config.endpoint && config.account_id == other.config.account_id &&
		       config.database_id == other.config.database_id && config.api_token == other.config.api_token &&
		       sql == other.sql && params == other.params;
	}
	// GCOVR_EXCL_STOP
};

struct D1BatchExecuteBindData : public FunctionData {
	D1BatchExecuteBindData(D1Config config_p, string batch_json_p)
	    : config(std::move(config_p)), batch_json(std::move(batch_json_p)) {
	}

	D1Config config;
	string batch_json;

	// GCOVR_EXCL_START
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<D1BatchExecuteBindData>(config, batch_json);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<D1BatchExecuteBindData>();
		return config.endpoint == other.config.endpoint && config.account_id == other.config.account_id &&
		       config.database_id == other.config.database_id && config.api_token == other.config.api_token &&
		       batch_json == other.batch_json;
	}
	// GCOVR_EXCL_STOP
};

struct D1AttachBindData : public FunctionData {
	D1AttachBindData(D1Config config_p, string secret_name_p, bool overwrite_p, bool include_views_p, string schema_p)
	    : config(std::move(config_p)), secret_name(std::move(secret_name_p)), overwrite(overwrite_p),
	      include_views(include_views_p), schema(std::move(schema_p)) {
	}

	D1Config config;
	string secret_name;
	bool overwrite;
	bool include_views;
	string schema;

	// GCOVR_EXCL_START
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<D1AttachBindData>(config, secret_name, overwrite, include_views, schema);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<D1AttachBindData>();
		return config.endpoint == other.config.endpoint && config.account_id == other.config.account_id &&
		       config.database_id == other.config.database_id && config.api_token == other.config.api_token &&
		       secret_name == other.secret_name && overwrite == other.overwrite &&
		       include_views == other.include_views && schema == other.schema;
	}
	// GCOVR_EXCL_STOP
};

struct D1RowScanGlobalState : public GlobalTableFunctionState {
	D1RowScanGlobalState() {
	}
	idx_t offset = 0;
	mutex lock;

	idx_t MaxThreads() const override {
		// GCOVR_EXCL_LINE
		return 1;
	}
};

struct D1MetadataGlobalState : public GlobalTableFunctionState {
	explicit D1MetadataGlobalState(D1Config config_p, string sql_p, vector<Value> params_p, string batch_json_p)
	    : config(std::move(config_p)), sql(std::move(sql_p)), params(std::move(params_p)),
	      batch_json(std::move(batch_json_p)), loaded(false), offset(0) {
	}

	D1Config config;
	string sql;
	vector<Value> params;
	string batch_json;
	bool loaded = false;
	idx_t offset = 0;
	vector<D1StatementMeta> rows;
	mutex lock;

	idx_t MaxThreads() const override {
		// GCOVR_EXCL_LINE
		return 1;
	}
};

struct D1AttachGlobalState : public GlobalTableFunctionState {
	bool finished = false;

	idx_t MaxThreads() const override {
		// GCOVR_EXCL_LINE
		return 1;
	}
};

static Value MaybeGetNamedParameter(const named_parameter_map_t &named_parameters, const string &name) {
	auto entry = named_parameters.find(name);
	if (entry == named_parameters.end()) {
		return Value();
	}
	return entry->second;
}

static bool GetOptionalBoolParameter(const named_parameter_map_t &named_parameters, const string &name,
                                     bool default_value) {
	auto value = MaybeGetNamedParameter(named_parameters, name);
	if (value.IsNull()) {
		return default_value;
	}
	if (value.type().id() == LogicalTypeId::BOOLEAN) {
		return value.GetValue<bool>();
	}
	Value casted;
	if (!value.DefaultTryCastAs(LogicalType::BOOLEAN, casted, nullptr)) {
		throw BinderException("Named parameter \"%s\" must be a boolean", name);
	}
	return casted.GetValue<bool>();
}

static string GetOptionalStringParameter(const named_parameter_map_t &named_parameters, const string &name,
                                         const string &default_value = string()) {
	auto value = MaybeGetNamedParameter(named_parameters, name);
	if (value.IsNull()) {
		return default_value;
	}
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		return value.GetValue<string>();
	}
	Value casted;
	if (!value.DefaultTryCastAs(LogicalType::VARCHAR, casted, nullptr)) {
		throw BinderException("Named parameter \"%s\" must be VARCHAR", name);
	}
	return casted.GetValue<string>();
}

static optional_idx GetOptionalLimitParameter(const named_parameter_map_t &named_parameters, const string &name) {
	auto value = MaybeGetNamedParameter(named_parameters, name);
	if (value.IsNull()) {
		return optional_idx();
	}
	Value casted;
	if (!value.DefaultTryCastAs(LogicalType::UBIGINT, casted, nullptr)) {
		throw BinderException("Named parameter \"%s\" must be UBIGINT", name);
	}
	return static_cast<idx_t>(casted.GetValue<uint64_t>());
}

static vector<Value> ParseParamList(const Value &value) {
	if (value.IsNull()) {
		return {};
	}
	const auto type_id = value.type().id();
	if (type_id == LogicalTypeId::STRUCT) {
		return StructValue::GetChildren(value);
	}
	if (type_id == LogicalTypeId::LIST) {
		return ListValue::GetChildren(value);
	}
	if (type_id == LogicalTypeId::ARRAY) {
		return ArrayValue::GetChildren(value);
	}
	return {value};
}

static string QuoteSQLString(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

static string QuoteIdentifierPart(const string &part_p) {
	auto part = part_p;
	StringUtil::Trim(part);
	if (part.empty()) {
		throw BinderException("Identifier contains an empty part");
	}
	return "\"" + StringUtil::Replace(part, "\"", "\"\"") + "\"";
}

static string QuoteMultipartIdentifier(const string &identifier) {
	vector<string> parts;
	string current;
	for (auto ch : identifier) {
		if (ch == '.') {
			parts.push_back(current);
			current.clear();
		} else {
			current.push_back(ch);
		}
	}
	parts.push_back(current);
	if (parts.empty()) {
		throw BinderException("Identifier cannot be empty");
	}
	for (auto &part : parts) {
		part = QuoteIdentifierPart(part);
	}
	return StringUtil::Join(parts, ".");
}

static vector<Value> ParseParamsNamedParameter(const named_parameter_map_t &named_parameters,
                                               const string &function_name) {
	(void)function_name;
	auto value = MaybeGetNamedParameter(named_parameters, "params");
	if (value.IsNull()) {
		return {};
	}
	return ParseParamList(value);
}

static json DuckValueToJSON(const Value &value) {
	if (value.IsNull()) {
		return nullptr;
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return value.GetValue<bool>();
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return value.GetValue<int64_t>();
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return value.GetValue<uint64_t>();
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return value.GetValue<double>();
	case LogicalTypeId::VARCHAR:
		return value.GetValue<string>();
	case LogicalTypeId::LIST: {
		json result = json::array();
		for (const auto &child : ListValue::GetChildren(value)) {
			result.push_back(DuckValueToJSON(child));
		}
		return result;
	}
	case LogicalTypeId::ARRAY: {
		json result = json::array();
		for (const auto &child : ArrayValue::GetChildren(value)) {
			result.push_back(DuckValueToJSON(child));
		}
		return result;
	}
	case LogicalTypeId::STRUCT: {
		json result = json::object();
		auto &child_types = StructType::GetChildTypes(value.type());
		auto &child_values = StructValue::GetChildren(value);
		for (idx_t i = 0; i < child_values.size(); i++) {
			result[child_types[i].first] = DuckValueToJSON(child_values[i]);
		}
		return result;
	}
	default:
		return value.ToString();
	}
}

static string ParseBatchJSONPayload(const Value &value) {
	if (value.IsNull()) {
		throw BinderException("Batch payload cannot be NULL");
	}
	try {
		json payload;
		if (value.type().id() == LogicalTypeId::VARCHAR) {
			payload = json::parse(value.GetValue<string>());
		} else if (value.type().id() == LogicalTypeId::LIST) {
			payload = json::array();
			for (const auto &child : ListValue::GetChildren(value)) {
				payload.push_back(DuckValueToJSON(child));
			}
		} else if (value.type().id() == LogicalTypeId::ARRAY) {
			payload = json::array();
			for (const auto &child : ArrayValue::GetChildren(value)) {
				payload.push_back(DuckValueToJSON(child));
			}
		} else {
			throw BinderException("Batch payload must be a JSON string, LIST or ARRAY");
		}
		if (!payload.is_array()) {
			throw BinderException("Batch payload must be a JSON array");
		}
		return payload.dump();
	} catch (const BinderException &) {
		throw;
	} catch (const std::exception &ex) {
		throw BinderException("Invalid batch payload: %s", ex.what());
	}
}

static string RequireNotNullString(const Value &value, const string &arg_name, const string &function_name) {
	if (value.IsNull()) {
		throw BinderException("%s argument \"%s\" cannot be NULL", function_name, arg_name);
	}
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		return value.GetValue<string>();
	}
	Value casted;
	if (!value.DefaultTryCastAs(LogicalType::VARCHAR, casted, nullptr)) {
		throw BinderException("%s argument \"%s\" must be VARCHAR", function_name, arg_name);
	}
	return casted.GetValue<string>();
}

static string SanitizeSQL(const string &sql) {
	auto trimmed = sql;
	StringUtil::Trim(trimmed);
	while (!trimmed.empty() && trimmed.back() == ';') {
		trimmed.pop_back();
		StringUtil::RTrim(trimmed);
	}
	return trimmed;
}

static bool IsLikelyWriteSQL(const string &sql) {
	auto lowered = sql;
	StringUtil::Trim(lowered);
	lowered = StringUtil::Lower(lowered);
	return StringUtil::StartsWith(lowered, "insert") || StringUtil::StartsWith(lowered, "update") ||
	       StringUtil::StartsWith(lowered, "delete") || StringUtil::StartsWith(lowered, "create") ||
	       StringUtil::StartsWith(lowered, "drop") || StringUtil::StartsWith(lowered, "alter") ||
	       StringUtil::StartsWith(lowered, "replace") || StringUtil::StartsWith(lowered, "pragma") ||
	       StringUtil::StartsWith(lowered, "begin") || StringUtil::StartsWith(lowered, "commit") ||
	       StringUtil::StartsWith(lowered, "rollback") || StringUtil::StartsWith(lowered, "vacuum");
}

static void EnsureResultHasColumns(D1TabularResult &result) {
	idx_t max_cols = result.columns.size();
	for (const auto &row : result.rows) {
		max_cols = MaxValue<idx_t>(max_cols, row.size());
	}
	if (max_cols == 0) {
		result.columns.push_back("result");
		max_cols = 1;
	}
	if (result.columns.size() < max_cols) {
		for (idx_t i = result.columns.size(); i < max_cols; i++) {
			result.columns.push_back(StringUtil::Format("column_%llu", i + 1));
		}
	}
	for (auto &row : result.rows) {
		while (row.size() < max_cols) {
			row.push_back(Value());
		}
	}
}

static LogicalType InferColumnType(const D1TabularResult &result, idx_t col_idx) {
	bool seen_bool = false;
	bool seen_signed = false;
	bool seen_unsigned = false;
	bool seen_double = false;
	bool seen_string = false;
	bool signed_negative = false;
	bool unsigned_over_int64 = false;

	for (const auto &row : result.rows) {
		if (col_idx >= row.size()) {
			continue;
		}
		auto &value = row[col_idx];
		if (value.IsNull()) {
			continue;
		}
		switch (value.type().id()) {
		case LogicalTypeId::BOOLEAN:
			seen_bool = true;
			break;
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
			seen_signed = true;
			signed_negative = signed_negative || value.GetValue<int64_t>() < 0;
			break;
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
			seen_unsigned = true;
			unsigned_over_int64 = unsigned_over_int64 ||
			                      value.GetValue<uint64_t>() > static_cast<uint64_t>(NumericLimits<int64_t>::Maximum());
			break;
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
			seen_double = true;
			break;
		default:
			seen_string = true;
			break;
		}
	}

	if (seen_string) {
		return LogicalType::VARCHAR;
	}
	if (seen_bool && (seen_signed || seen_unsigned || seen_double)) {
		return LogicalType::VARCHAR;
	}
	if (seen_double) {
		if (seen_signed || seen_unsigned) {
			return LogicalType::DOUBLE;
		}
		return LogicalType::DOUBLE;
	}
	if (seen_signed || seen_unsigned) {
		if (seen_signed && seen_unsigned) {
			if (signed_negative && unsigned_over_int64) {
				return LogicalType::VARCHAR;
			}
			if (!signed_negative && unsigned_over_int64) {
				return LogicalType::UBIGINT;
			}
			return LogicalType::BIGINT;
		}
		if (seen_unsigned) {
			return unsigned_over_int64 ? LogicalType::UBIGINT : LogicalType::BIGINT;
		}
		return LogicalType::BIGINT;
	}
	if (seen_bool) {
		return LogicalType::BOOLEAN;
	}
	return LogicalType::VARCHAR;
}

static Value CastForOutput(const Value &value, const LogicalType &target_type) {
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

static void FillReadChunk(const D1ReadBindData &bind_data, D1RowScanGlobalState &state, DataChunk &output) {
	idx_t start = 0;
	idx_t count = 0;
	{
		lock_guard<mutex> guard(state.lock);
		if (state.offset >= bind_data.rows.size()) {
			output.SetCardinality(0);
			return;
		}
		start = state.offset;
		count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.rows.size() - state.offset);
		state.offset += count;
	}

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto &row = bind_data.rows[start + row_idx];
		for (idx_t col_idx = 0; col_idx < bind_data.names.size(); col_idx++) {
			auto value = col_idx < row.size() ? row[col_idx] : Value();
			output.SetValue(col_idx, row_idx, CastForOutput(value, bind_data.types[col_idx]));
		}
	}
	output.SetCardinality(count);
}

static vector<LogicalType> BuildMetadataTypes() {
	return {LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::DOUBLE,
	        LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::DOUBLE,
	        LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE,
	        LogicalType::DOUBLE,  LogicalType::VARCHAR};
}

static vector<string> BuildMetadataNames() {
	return {"statement_index", "success",      "changed_db",        "changes",          "duration",       "last_row_id",
	        "rows_read",       "rows_written", "served_by_primary", "served_by_region", "served_by_colo", "size_after",
	        "sql_duration_ms", "error"};
}

static void FillMetadataChunk(const vector<D1StatementMeta> &rows, D1MetadataGlobalState &state, DataChunk &output) {
	idx_t start = 0;
	idx_t count = 0;
	{
		lock_guard<mutex> guard(state.lock);
		if (state.offset >= rows.size()) {
			output.SetCardinality(0);
			return;
		}
		start = state.offset;
		count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, rows.size() - state.offset);
		state.offset += count;
	}
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto &meta = rows[start + row_idx];
		output.SetValue(0, row_idx, Value::UBIGINT(meta.statement_index));
		output.SetValue(1, row_idx, Value::BOOLEAN(meta.success));
		output.SetValue(2, row_idx, Value::BOOLEAN(meta.changed_db));
		output.SetValue(3, row_idx, Value::DOUBLE(meta.changes));
		output.SetValue(4, row_idx, Value::DOUBLE(meta.duration));
		output.SetValue(5, row_idx, Value::DOUBLE(meta.last_row_id));
		output.SetValue(6, row_idx, Value::DOUBLE(meta.rows_read));
		output.SetValue(7, row_idx, Value::DOUBLE(meta.rows_written));
		output.SetValue(8, row_idx, Value::BOOLEAN(meta.served_by_primary));
		output.SetValue(9, row_idx, Value(meta.served_by_region));
		output.SetValue(10, row_idx, Value(meta.served_by_colo));
		output.SetValue(11, row_idx, Value::DOUBLE(meta.size_after));
		output.SetValue(12, row_idx, Value::DOUBLE(meta.sql_duration_ms));
		if (meta.error.empty()) {
			output.SetValue(13, row_idx, Value());
		} else {
			output.SetValue(13, row_idx, Value(meta.error));
		}
	}
	output.SetCardinality(count);
}

static unique_ptr<FunctionData> BindReadFromSQL(ClientContext &context, const string &secret_name, const string &sql,
                                                const vector<Value> &params, bool raw_endpoint, bool all_varchar,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto config = ResolveD1ConfigFromSecret(context, secret_name);
	D1Client client(config);
	auto result = client.Query(sql, params, raw_endpoint);
	EnsureResultHasColumns(result);

	names = result.columns;
	if (all_varchar) {
		for (idx_t i = 0; i < names.size(); i++) {
			return_types.push_back(LogicalType::VARCHAR);
		}
	} else {
		for (idx_t i = 0; i < names.size(); i++) {
			return_types.push_back(InferColumnType(result, i));
		}
	}

	return make_uniq<D1ReadBindData>(std::move(config), names, return_types, std::move(result.rows));
}

static string BuildTablesSQL(bool include_views) {
	// Filter Cloudflare-managed internal tables that are present in sqlite_master but not user-queryable.
	string sql = "SELECT name, type, sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' AND name NOT GLOB '_cf_*'";
	if (include_views) {
		sql += " AND type IN ('table', 'view')";
	} else {
		sql += " AND type = 'table'";
	}
	sql += " ORDER BY name";
	return sql;
}

static unique_ptr<FunctionData> BindReadFunction(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names,
                                                 bool raw_endpoint, const string &function_name) {
	if (input.inputs.size() != 2) {
		throw BinderException("%s expects 2 positional arguments: secret_name, sql", function_name);
	}

	auto secret_name = RequireNotNullString(input.inputs[0], "secret_name", function_name);
	auto sql = SanitizeSQL(RequireNotNullString(input.inputs[1], "sql", function_name));
	if (sql.empty()) {
		throw BinderException("%s argument \"sql\" cannot be empty", function_name);
	}
	if (IsLikelyWriteSQL(sql)) {
		throw BinderException("%s only supports read queries; use d1_execute or d1_batch_execute for writes",
		                      function_name);
	}
	auto params = ParseParamsNamedParameter(input.named_parameters, function_name);
	auto all_varchar = GetOptionalBoolParameter(input.named_parameters, "all_varchar", false);
	return BindReadFromSQL(context, secret_name, sql, params, raw_endpoint, all_varchar, return_types, names);
}

static unique_ptr<FunctionData> D1QueryBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return BindReadFunction(context, input, return_types, names, false, "d1_query");
}

static unique_ptr<FunctionData> D1RawBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	return BindReadFunction(context, input, return_types, names, true, "d1_raw");
}

static unique_ptr<FunctionData> D1TablesBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1) {
		throw BinderException("d1_tables expects 1 positional argument: secret_name");
	}
	auto secret_name = RequireNotNullString(input.inputs[0], "secret_name", "d1_tables");
	auto include_views = GetOptionalBoolParameter(input.named_parameters, "include_views", true);
	auto all_varchar = GetOptionalBoolParameter(input.named_parameters, "all_varchar", true);
	auto sql = BuildTablesSQL(include_views);
	return BindReadFromSQL(context, secret_name, sql, vector<Value>(), true, all_varchar, return_types, names);
}

static unique_ptr<FunctionData> D1ScanBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2) {
		throw BinderException("d1_scan expects 2 positional arguments: secret_name, table_name");
	}
	auto secret_name = RequireNotNullString(input.inputs[0], "secret_name", "d1_scan");
	auto table_name = RequireNotNullString(input.inputs[1], "table_name", "d1_scan");
	if (table_name.empty()) {
		throw BinderException("d1_scan argument \"table_name\" cannot be empty");
	}

	auto where_clause = GetOptionalStringParameter(input.named_parameters, "filter");
	auto limit = GetOptionalLimitParameter(input.named_parameters, "max_rows");
	auto all_varchar = GetOptionalBoolParameter(input.named_parameters, "all_varchar", false);

	auto sql = StringUtil::Format("SELECT * FROM %s", QuoteMultipartIdentifier(table_name));
	if (!where_clause.empty()) {
		sql += " WHERE " + where_clause;
	}
	if (limit.IsValid()) {
		sql += StringUtil::Format(" LIMIT %llu", limit.GetIndex());
	}

	return BindReadFromSQL(context, secret_name, sql, vector<Value>(), true, all_varchar, return_types, names);
}

static unique_ptr<GlobalTableFunctionState> D1ReadInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	(void)context;
	(void)input;
	return make_uniq<D1RowScanGlobalState>();
}

static void D1ReadFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	(void)context;
	auto &bind_data = input.bind_data->Cast<D1ReadBindData>();
	auto &state = input.global_state->Cast<D1RowScanGlobalState>();
	FillReadChunk(bind_data, state, output);
}

static unique_ptr<FunctionData> D1AttachBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 1) {
		throw BinderException("d1_attach expects 1 positional argument: secret_name");
	}
	auto secret_name = RequireNotNullString(input.inputs[0], "secret_name", "d1_attach");
	auto overwrite = GetOptionalBoolParameter(input.named_parameters, "overwrite", false);
	auto include_views = GetOptionalBoolParameter(input.named_parameters, "include_views", true);
	auto schema = GetOptionalStringParameter(input.named_parameters, "schema", DEFAULT_SCHEMA);
	if (schema.empty()) {
		schema = DEFAULT_SCHEMA;
	}

	auto config = ResolveD1ConfigFromSecret(context, secret_name);
	return_types = {LogicalType::BOOLEAN};
	names = {"success"};
	return make_uniq<D1AttachBindData>(std::move(config), std::move(secret_name), overwrite, include_views,
	                                   std::move(schema));
}

static unique_ptr<GlobalTableFunctionState> D1AttachInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	(void)context;
	(void)input;
	return make_uniq<D1AttachGlobalState>();
}

static void D1AttachFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<D1AttachBindData>();
	auto &state = input.global_state->Cast<D1AttachGlobalState>();
	if (state.finished) {
		output.SetCardinality(0);
		return;
	}

	if (!StringUtil::CIEquals(bind_data.schema, DEFAULT_SCHEMA)) {
		Connection conn(context.db->GetDatabase(context));
		auto create_schema_sql =
		    StringUtil::Format("CREATE SCHEMA IF NOT EXISTS %s", QuoteMultipartIdentifier(bind_data.schema));
		auto schema_result = conn.Query(create_schema_sql);
		if (schema_result->HasError()) {
			throw IOException("Failed to create schema \"%s\": %s", bind_data.schema, schema_result->GetError());
		}
	}

	D1Client client(bind_data.config);
	auto objects = client.Query(BuildTablesSQL(bind_data.include_views), vector<Value>(), true);
	EnsureResultHasColumns(objects);
	if (objects.columns.size() < 1) {
		throw IOException("Invalid sqlite_master response from D1: expected at least one column");
	}

	idx_t name_col_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < objects.columns.size(); i++) {
		if (StringUtil::CIEquals(objects.columns[i], "name")) {
			name_col_idx = i;
			break;
		}
	}
	if (name_col_idx == DConstants::INVALID_INDEX) {
		throw IOException("Invalid sqlite_master response from D1: missing \"name\" column");
	}

	Connection conn(context.db->GetDatabase(context));
	for (const auto &row : objects.rows) {
		if (name_col_idx >= row.size() || row[name_col_idx].IsNull()) {
			continue;
		}
		auto object_name = row[name_col_idx].ToString();
		if (object_name.empty()) {
			continue;
		}
		auto qualified_name =
		    StringUtil::Format("%s.%s", QuoteMultipartIdentifier(bind_data.schema), QuoteIdentifierPart(object_name));
		auto create_view_sql = StringUtil::Format("%s VIEW %s AS SELECT * FROM d1_scan(%s, %s)",
		                                          bind_data.overwrite ? "CREATE OR REPLACE" : "CREATE", qualified_name,
		                                          QuoteSQLString(bind_data.secret_name), QuoteSQLString(object_name));
		if (!bind_data.overwrite) {
			create_view_sql =
			    StringUtil::Format("CREATE VIEW IF NOT EXISTS %s AS SELECT * FROM d1_scan(%s, %s)", qualified_name,
			                       QuoteSQLString(bind_data.secret_name), QuoteSQLString(object_name));
		}
		auto create_result = conn.Query(create_view_sql);
		if (create_result->HasError()) {
			throw IOException("Failed to create D1 view for \"%s\": %s", object_name, create_result->GetError());
		}
	}

	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(true));
	state.finished = true;
}

static unique_ptr<FunctionData> D1ExecuteBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2) {
		throw BinderException("d1_execute expects 2 positional arguments: secret_name, sql");
	}
	auto secret_name = RequireNotNullString(input.inputs[0], "secret_name", "d1_execute");
	auto sql = SanitizeSQL(RequireNotNullString(input.inputs[1], "sql", "d1_execute"));
	auto params = ParseParamsNamedParameter(input.named_parameters, "d1_execute");
	auto config = ResolveD1ConfigFromSecret(context, secret_name);

	return_types = BuildMetadataTypes();
	names = BuildMetadataNames();
	return make_uniq<D1ExecuteBindData>(std::move(config), std::move(sql), std::move(params));
}

static unique_ptr<GlobalTableFunctionState> D1ExecuteInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	(void)context;
	auto &bind_data = input.bind_data->Cast<D1ExecuteBindData>();
	return make_uniq<D1MetadataGlobalState>(bind_data.config, bind_data.sql, bind_data.params, string());
}

static void D1ExecuteFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	(void)context;
	auto &state = input.global_state->Cast<D1MetadataGlobalState>();
	{
		lock_guard<mutex> guard(state.lock);
		if (!state.loaded) {
			D1Client client(state.config);
			state.rows = client.Execute(state.sql, state.params);
			state.loaded = true;
		}
	}
	FillMetadataChunk(state.rows, state, output);
}

static unique_ptr<FunctionData> D1BatchExecuteBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2) {
		throw BinderException("d1_batch_execute expects 2 positional arguments: secret_name, batch");
	}
	auto secret_name = RequireNotNullString(input.inputs[0], "secret_name", "d1_batch_execute");
	auto config = ResolveD1ConfigFromSecret(context, secret_name);
	auto batch_json = ParseBatchJSONPayload(input.inputs[1]);

	return_types = BuildMetadataTypes();
	names = BuildMetadataNames();
	return make_uniq<D1BatchExecuteBindData>(std::move(config), std::move(batch_json));
}

static unique_ptr<GlobalTableFunctionState> D1BatchExecuteInitGlobal(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	(void)context;
	auto &bind_data = input.bind_data->Cast<D1BatchExecuteBindData>();
	return make_uniq<D1MetadataGlobalState>(bind_data.config, string(), vector<Value>(), bind_data.batch_json);
}

static void D1BatchExecuteFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	(void)context;
	auto &state = input.global_state->Cast<D1MetadataGlobalState>();
	{
		lock_guard<mutex> guard(state.lock);
		if (!state.loaded) {
			D1Client client(state.config);
			state.rows = client.BatchExecuteJSON(state.batch_json);
			state.loaded = true;
		}
	}
	FillMetadataChunk(state.rows, state, output);
}

} // namespace

void RegisterD1TableFunctions(ExtensionLoader &loader) {
	TableFunction d1_query("d1_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, D1ReadFunction, D1QueryBind,
	                       D1ReadInitGlobal);
	d1_query.named_parameters["params"] = LogicalType::ANY;
	d1_query.named_parameters["all_varchar"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(d1_query);

	TableFunction d1_raw("d1_raw", {LogicalType::VARCHAR, LogicalType::VARCHAR}, D1ReadFunction, D1RawBind,
	                     D1ReadInitGlobal);
	d1_raw.named_parameters["params"] = LogicalType::ANY;
	d1_raw.named_parameters["all_varchar"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(d1_raw);

	TableFunction d1_tables("d1_tables", {LogicalType::VARCHAR}, D1ReadFunction, D1TablesBind, D1ReadInitGlobal);
	d1_tables.named_parameters["include_views"] = LogicalType::BOOLEAN;
	d1_tables.named_parameters["all_varchar"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(d1_tables);

	TableFunction d1_scan("d1_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, D1ReadFunction, D1ScanBind,
	                      D1ReadInitGlobal);
	d1_scan.named_parameters["filter"] = LogicalType::VARCHAR;
	d1_scan.named_parameters["max_rows"] = LogicalType::UBIGINT;
	d1_scan.named_parameters["all_varchar"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(d1_scan);

	TableFunction d1_execute("d1_execute", {LogicalType::VARCHAR, LogicalType::VARCHAR}, D1ExecuteFunction,
	                         D1ExecuteBind, D1ExecuteInitGlobal);
	d1_execute.named_parameters["params"] = LogicalType::ANY;
	loader.RegisterFunction(d1_execute);

	TableFunction d1_batch_execute("d1_batch_execute", {LogicalType::VARCHAR, LogicalType::ANY}, D1BatchExecuteFunction,
	                               D1BatchExecuteBind, D1BatchExecuteInitGlobal);
	loader.RegisterFunction(d1_batch_execute);

	TableFunction d1_attach("d1_attach", {LogicalType::VARCHAR}, D1AttachFunction, D1AttachBind, D1AttachInitGlobal);
	d1_attach.named_parameters["overwrite"] = LogicalType::BOOLEAN;
	d1_attach.named_parameters["include_views"] = LogicalType::BOOLEAN;
	d1_attach.named_parameters["schema"] = LogicalType::VARCHAR;
	loader.RegisterFunction(d1_attach);
}

} // namespace d1
} // namespace duckdb
