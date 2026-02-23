#include "d1/transport.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <cmath>
#include <chrono>
#include <thread>

#include "httplib.hpp"

namespace duckdb {
namespace d1 {

namespace {

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
namespace httplib_ns = duckdb_httplib_openssl;
#else
namespace httplib_ns = duckdb_httplib;
#endif

struct ParsedURL {
	string scheme;
	string host;
	int port = 0;
	string base;
	string path;
};

ParsedURL ParseURL(const string &url) {
	auto scheme_sep = url.find("://");
	if (scheme_sep == string::npos) {
		throw InvalidInputException("Invalid URL, missing scheme: %s", url);
	}
	ParsedURL parsed;
	parsed.scheme = StringUtil::Lower(url.substr(0, scheme_sep));
	auto rest = url.substr(scheme_sep + 3);
	auto path_pos = rest.find('/');
	auto host_port = path_pos == string::npos ? rest : rest.substr(0, path_pos);
	parsed.path = path_pos == string::npos ? "/" : rest.substr(path_pos);
	if (host_port.empty()) {
		throw InvalidInputException("Invalid URL, missing host: %s", url);
	}
	auto colon_pos = host_port.rfind(':');
	if (colon_pos != string::npos && host_port.find(']') == string::npos) {
		parsed.host = host_port.substr(0, colon_pos);
		auto port_str = host_port.substr(colon_pos + 1);
		if (port_str.empty()) {
			throw InvalidInputException("Invalid URL, missing port after ':' in %s", url);
		}
		try {
			auto parsed_port = std::stoi(port_str);
			if (parsed_port <= 0 || parsed_port > NumericLimits<uint16_t>::Maximum()) {
				throw InvalidInputException("Invalid URL, out-of-range port in %s", url);
			}
			parsed.port = parsed_port;
		} catch (const std::exception &) {
			throw InvalidInputException("Invalid URL, invalid port in %s", url);
		}
	} else {
		parsed.host = host_port;
		parsed.port = parsed.scheme == "https" ? 443 : 80;
	}
	parsed.base = parsed.scheme + "://" + host_port;
	if (parsed.path.empty()) {
		parsed.path = "/";
	}
	return parsed;
}

bool ShouldRetry(const D1HTTPResponse &response) {
	if (response.HasError()) {
		return true;
	}
	if (response.status_code == 408 || response.status_code == 429) {
		return true;
	}
	return response.status_code >= 500;
}

} // namespace

D1HTTPResponse D1HttpLibTransport::PostJSON(const string &url, const unordered_map<string, string> &headers,
                                            const string &body, const D1HTTPOptions &options) {
	auto parsed = ParseURL(url);
	idx_t attempts = 0;
	D1HTTPResponse last_response;
	while (attempts <= options.retries) {
		attempts++;
		httplib_ns::Client client(parsed.base);
		client.set_follow_location(options.follow_redirects);
		client.set_connection_timeout(static_cast<time_t>(options.timeout_seconds), 0);
		client.set_read_timeout(static_cast<time_t>(options.timeout_seconds), 0);
		client.set_write_timeout(static_cast<time_t>(options.timeout_seconds), 0);

		httplib_ns::Headers req_headers;
		for (const auto &entry : headers) {
			req_headers.emplace(entry.first, entry.second);
		}

		auto res = client.Post(parsed.path, req_headers, body, "application/json");
		if (!res) {
			last_response = D1HTTPResponse();
			last_response.error = "HTTP request failed";
		} else {
			last_response = D1HTTPResponse();
			last_response.status_code = res->status;
			last_response.body = res->body;
		}

		if (!ShouldRetry(last_response) || attempts > options.retries) {
			return last_response;
		}
		auto sleep_ms = static_cast<idx_t>(static_cast<double>(options.retry_wait_ms) *
		                                   pow(options.retry_backoff, static_cast<double>(attempts - 1)));
		std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
	}
	return last_response;
}

} // namespace d1
} // namespace duckdb
