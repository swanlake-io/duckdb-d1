#include "d1/client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

namespace duckdb {
namespace d1 {

using json = nlohmann::json;

namespace {

string JoinErrors(const json &errors) {
	if (!errors.is_array()) {
		return "unknown error";
	}
	vector<string> parts;
	for (auto &entry : errors) {
		if (entry.is_object() && entry.contains("message")) {
			parts.push_back(entry["message"].get<string>());
		} else if (entry.is_string()) {
			parts.push_back(entry.get<string>());
		}
	}
	if (parts.empty()) {
		return "unknown error";
	}
	return StringUtil::Join(parts, "; ");
}

json ValueToJSON(const Value &value) {
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
	default:
		return value.ToString();
	}
}

Value JSONToValue(const json &value) {
	if (value.is_null()) {
		return Value();
	}
	if (value.is_boolean()) {
		return Value::BOOLEAN(value.get<bool>());
	}
	if (value.is_number_integer()) {
		return Value::BIGINT(value.get<int64_t>());
	}
	if (value.is_number_unsigned()) {
		auto v = value.get<uint64_t>();
		if (v <= static_cast<uint64_t>(NumericLimits<int64_t>::Maximum())) {
			return Value::BIGINT(static_cast<int64_t>(v));
		}
		return Value::UBIGINT(v);
	}
	if (value.is_number_float()) {
		return Value::DOUBLE(value.get<double>());
	}
	if (value.is_string()) {
		return Value(value.get<string>());
	}
	return Value(value.dump());
}

vector<json> NormalizeResultStatements(const json &root) {
	if (!root.contains("result")) {
		throw IOException("D1 response does not contain 'result'");
	}
	const auto &result = root["result"];
	if (result.is_array()) {
		vector<json> out;
		for (const auto &entry : result) {
			out.push_back(entry);
		}
		return out;
	}
	if (result.is_object()) {
		return {result};
	}
	throw IOException("Invalid D1 response: 'result' must be object or array");
}

D1StatementMeta ParseMetaFromStatement(const json &statement, idx_t statement_index) {
	D1StatementMeta meta;
	meta.statement_index = statement_index;
	meta.success = statement.value("success", true);

	if (!statement.contains("meta") || !statement["meta"].is_object()) {
		if (!meta.success) {
			meta.error = "statement failed without meta";
		}
		return meta;
	}
	const auto &meta_json = statement["meta"];
	meta.changed_db = meta_json.value("changed_db", false);
	meta.changes = meta_json.value("changes", 0.0);
	meta.duration = meta_json.value("duration", 0.0);
	meta.last_row_id = meta_json.value("last_row_id", 0.0);
	meta.rows_read = meta_json.value("rows_read", 0.0);
	meta.rows_written = meta_json.value("rows_written", 0.0);
	meta.served_by_primary = meta_json.value("served_by_primary", false);
	meta.served_by_region = meta_json.value("served_by_region", string());
	meta.served_by_colo = meta_json.value("served_by_colo", string());
	meta.size_after = meta_json.value("size_after", 0.0);
	if (meta_json.contains("timings") && meta_json["timings"].is_object()) {
		meta.sql_duration_ms = meta_json["timings"].value("sql_duration_ms", 0.0);
	}
	return meta;
}

bool IsRawAuthFailure(const D1HTTPResponse &response) {
	if (response.HasError()) {
		return false;
	}
	// Some D1 deployments reject /raw with SQLITE_AUTH while /query still works for the same SQL.
	if (response.status_code != 400) {
		return false;
	}
	return StringUtil::Contains(response.body, "SQLITE_AUTH") || StringUtil::Contains(response.body, "\"code\":7500");
}

} // namespace

D1Client::D1Client(D1Config config_p, unique_ptr<D1Transport> transport_p)
    : config(std::move(config_p)), transport(std::move(transport_p)) {
	StringUtil::Trim(config.endpoint);
	while (!config.endpoint.empty() && config.endpoint.back() == '/') {
		config.endpoint.pop_back();
	}
	config.Validate();
	if (!transport) {
		transport = make_uniq<D1HttpLibTransport>();
	}
}

string D1Client::BuildEndpointPath(const string &suffix) const {
	return StringUtil::Format("%s/accounts/%s/d1/database/%s/%s", config.endpoint, config.account_id,
	                          config.database_id, suffix);
}

string D1Client::BuildAuthHeader() const {
	return "Bearer " + config.api_token;
}

string D1Client::BuildSingleRequestBody(const string &sql, const vector<Value> &params) const {
	json body;
	body["sql"] = sql;
	if (!params.empty()) {
		body["params"] = json::array();
		for (const auto &param : params) {
			body["params"].push_back(ValueToJSON(param));
		}
	}
	return body.dump();
}

void D1Client::ThrowOnHTTPFailure(const D1HTTPResponse &response, const string &url) const {
	if (!response.HasError() && response.status_code < 400) {
		return;
	}
	if (response.HasError()) {
		throw IOException("D1 HTTP request failed for %s: %s", url, response.error);
	}
	throw IOException("D1 HTTP request failed for %s with status %d: %s", url, response.status_code, response.body);
}

D1TabularResult D1Client::ParseRawRows(const string &response_body) const {
	auto root = json::parse(response_body);
	if (root.contains("success") && !root["success"].get<bool>()) {
		throw IOException("D1 API error: %s", JoinErrors(root.value("errors", json::array())));
	}

	auto statements = NormalizeResultStatements(root);
	if (statements.size() != 1) {
		throw IOException("d1_raw expects exactly one statement result, got %llu", statements.size());
	}
	const auto &statement = statements[0];
	if (!statement.value("success", true)) {
		throw IOException("D1 statement failed");
	}
	if (!statement.contains("results") || !statement["results"].is_object()) {
		throw IOException("Invalid D1 raw response: missing object 'results'");
	}
	const auto &results = statement["results"];
	if (!results.contains("columns") || !results.contains("rows")) {
		throw IOException("Invalid D1 raw response: missing columns/rows");
	}

	D1TabularResult parsed;
	for (const auto &col : results["columns"]) {
		parsed.columns.push_back(col.get<string>());
	}
	for (const auto &row : results["rows"]) {
		if (!row.is_array()) {
			throw IOException("Invalid D1 raw row: expected array");
		}
		vector<Value> out_row;
		out_row.reserve(parsed.columns.size());
		for (const auto &entry : row) {
			out_row.push_back(JSONToValue(entry));
		}
		parsed.rows.push_back(std::move(out_row));
	}
	return parsed;
}

D1TabularResult D1Client::ParseQueryRows(const string &response_body) const {
	auto root = json::parse(response_body);
	if (root.contains("success") && !root["success"].get<bool>()) {
		throw IOException("D1 API error: %s", JoinErrors(root.value("errors", json::array())));
	}

	auto statements = NormalizeResultStatements(root);
	if (statements.size() != 1) {
		throw IOException("d1_query expects exactly one statement result, got %llu", statements.size());
	}
	const auto &statement = statements[0];
	if (!statement.value("success", true)) {
		throw IOException("D1 statement failed");
	}
	if (!statement.contains("results") || !statement["results"].is_array()) {
		throw IOException("Invalid D1 query response: missing array 'results'");
	}

	D1TabularResult parsed;
	const auto &rows = statement["results"];
	if (rows.empty()) {
		return parsed;
	}
	if (!rows[0].is_object()) {
		parsed.columns.push_back("value");
		for (const auto &row : rows) {
			parsed.rows.push_back({JSONToValue(row)});
		}
		return parsed;
	}

	for (auto it = rows[0].begin(); it != rows[0].end(); ++it) {
		parsed.columns.push_back(it.key());
	}
	for (const auto &row : rows) {
		vector<Value> out_row;
		out_row.reserve(parsed.columns.size());
		for (const auto &col : parsed.columns) {
			if (row.contains(col)) {
				out_row.push_back(JSONToValue(row[col]));
			} else {
				out_row.push_back(Value());
			}
		}
		parsed.rows.push_back(std::move(out_row));
	}
	return parsed;
}

vector<D1StatementMeta> D1Client::ParseStatementMetadata(const string &response_body) const {
	auto root = json::parse(response_body);
	if (root.contains("success") && !root["success"].get<bool>()) {
		throw IOException("D1 API error: %s", JoinErrors(root.value("errors", json::array())));
	}
	auto statements = NormalizeResultStatements(root);
	vector<D1StatementMeta> metas;
	metas.reserve(statements.size());
	for (idx_t i = 0; i < statements.size(); i++) {
		auto meta = ParseMetaFromStatement(statements[i], i);
		if (!meta.success) {
			meta.error = "statement execution failed";
		}
		metas.push_back(std::move(meta));
	}
	return metas;
}

D1TabularResult D1Client::Query(const string &sql, const vector<Value> &params, bool raw_endpoint) {
	auto body = BuildSingleRequestBody(sql, params);

	unordered_map<string, string> headers;
	headers["Authorization"] = BuildAuthHeader();
	headers["Accept"] = "application/json";
	headers["Content-Type"] = "application/json";
	headers["User-Agent"] = "duckdb-d1-extension";

	if (raw_endpoint) {
		auto raw_url = BuildEndpointPath("raw");
		auto raw_response = transport->PostJSON(raw_url, headers, body, http_options);
		if (!IsRawAuthFailure(raw_response)) {
			ThrowOnHTTPFailure(raw_response, raw_url);
			return ParseRawRows(raw_response.body);
		}
		// Fall back to /query when /raw is blocked by D1 auth policy.
		auto query_url = BuildEndpointPath("query");
		auto query_response = transport->PostJSON(query_url, headers, body, http_options);
		ThrowOnHTTPFailure(query_response, query_url);
		return ParseQueryRows(query_response.body);
	}

	auto query_url = BuildEndpointPath("query");
	auto query_response = transport->PostJSON(query_url, headers, body, http_options);
	ThrowOnHTTPFailure(query_response, query_url);
	return ParseQueryRows(query_response.body);
}

vector<D1StatementMeta> D1Client::Execute(const string &sql, const vector<Value> &params) {
	auto url = BuildEndpointPath("query");
	auto body = BuildSingleRequestBody(sql, params);

	unordered_map<string, string> headers;
	headers["Authorization"] = BuildAuthHeader();
	headers["Accept"] = "application/json";
	headers["Content-Type"] = "application/json";
	headers["User-Agent"] = "duckdb-d1-extension";
	auto response = transport->PostJSON(url, headers, body, http_options);
	ThrowOnHTTPFailure(response, url);
	return ParseStatementMetadata(response.body);
}

vector<D1StatementMeta> D1Client::BatchExecuteJSON(const string &batch_json) {
	auto url = BuildEndpointPath("query");
	json input = json::parse(batch_json);
	if (!input.is_array()) {
		throw InvalidInputException("Batch payload must be a JSON array");
	}
	json body;
	body["batch"] = json::array();
	for (const auto &entry : input) {
		if (entry.is_string()) {
			body["batch"].push_back({{"sql", entry.get<string>()}});
			continue;
		}
		if (!entry.is_object() || !entry.contains("sql")) {
			throw InvalidInputException("Batch entries must be strings or objects with a sql field");
		}
		json statement;
		statement["sql"] = entry["sql"];
		if (entry.contains("params")) {
			statement["params"] = entry["params"];
		}
		body["batch"].push_back(statement);
	}

	unordered_map<string, string> headers;
	headers["Authorization"] = BuildAuthHeader();
	headers["Accept"] = "application/json";
	headers["Content-Type"] = "application/json";
	headers["User-Agent"] = "duckdb-d1-extension";
	auto response = transport->PostJSON(url, headers, body.dump(), http_options);
	ThrowOnHTTPFailure(response, url);
	return ParseStatementMetadata(response.body);
}

} // namespace d1
} // namespace duckdb
