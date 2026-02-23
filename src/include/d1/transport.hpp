#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace d1 {

struct D1HTTPResponse {
	int status_code = 0;
	string body;
	string error;

	bool HasError() const {
		return !error.empty();
	}
};

struct D1HTTPOptions {
	idx_t timeout_seconds = 30;
	idx_t retries = 3;
	idx_t retry_wait_ms = 100;
	double retry_backoff = 2.0;
	bool follow_redirects = true;
};

class D1Transport {
public:
	virtual ~D1Transport() = default;
	virtual D1HTTPResponse PostJSON(const string &url, const unordered_map<string, string> &headers, const string &body,
	                                const D1HTTPOptions &options) = 0;
};

class D1HttpLibTransport : public D1Transport {
public:
	D1HTTPResponse PostJSON(const string &url, const unordered_map<string, string> &headers, const string &body,
	                        const D1HTTPOptions &options) override;
};

} // namespace d1
} // namespace duckdb
