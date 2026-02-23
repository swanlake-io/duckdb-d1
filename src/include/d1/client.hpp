#pragma once

#include "d1/transport.hpp"
#include "d1/types.hpp"

namespace duckdb {
namespace d1 {

class D1Client {
public:
	explicit D1Client(D1Config config, unique_ptr<D1Transport> transport = nullptr);

	D1TabularResult Query(const string &sql, const vector<Value> &params, bool raw_endpoint);
	vector<D1StatementMeta> Execute(const string &sql, const vector<Value> &params);
	vector<D1StatementMeta> BatchExecuteJSON(const string &batch_json);

private:
	D1Config config;
	unique_ptr<D1Transport> transport;
	D1HTTPOptions http_options;

	string BuildEndpointPath(const string &suffix) const;
	string BuildAuthHeader() const;

	vector<D1StatementMeta> ParseStatementMetadata(const string &response_body) const;
	D1TabularResult ParseRawRows(const string &response_body) const;
	D1TabularResult ParseQueryRows(const string &response_body) const;

	string BuildSingleRequestBody(const string &sql, const vector<Value> &params) const;
	void ThrowOnHTTPFailure(const D1HTTPResponse &response, const string &url) const;
};

} // namespace d1
} // namespace duckdb
