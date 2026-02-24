#include "d1/secret.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace d1 {

static string GetSecretStringValue(const KeyValueSecret &secret, const string &key, bool required = true) {
	auto value = secret.TryGetValue(key, !required);
	if (value.IsNull()) {
		if (required) {
			throw BinderException("D1 secret is missing required key \"%s\"", key);
		}
		return string();
	}
	return value.ToString();
}

static string NormalizeEndpoint(const string &endpoint) {
	if (endpoint.empty()) {
		return "https://api.cloudflare.com/client/v4";
	}
	auto result = endpoint;
	while (!result.empty() && result.back() == '/') {
		result.pop_back();
	}
	return result;
}

void D1Config::Validate() const {
	if (endpoint.empty()) {
		throw InvalidInputException("D1 endpoint is empty");
	}
	if (account_id.empty()) {
		throw InvalidInputException("D1 account_id is required");
	}
	if (database_id.empty()) {
		throw InvalidInputException("D1 database_id is required");
	}
	if (api_token.empty()) {
		throw InvalidInputException("D1 api_token is required");
	}
}

unique_ptr<BaseSecret> CreateD1SecretFunction(ClientContext &context, CreateSecretInput &input) {
	vector<string> prefix_paths;
	auto secret = make_uniq<KeyValueSecret>(prefix_paths, "d1", "config", input.name);

	for (const auto &entry : input.options) {
		auto name = StringUtil::Lower(entry.first);
		if (name == "account_id") {
			secret->secret_map["account_id"] = entry.second.ToString();
		} else if (name == "database_id") {
			secret->secret_map["database_id"] = entry.second.ToString();
		} else if (name == "api_token") {
			secret->secret_map["api_token"] = entry.second.ToString();
		} else if (name == "endpoint") {
			secret->secret_map["endpoint"] = NormalizeEndpoint(entry.second.ToString());
		} else {
			throw InvalidInputException("Unknown D1 secret parameter: %s", entry.first);
		}
	}

	if (!secret->TryGetValue("endpoint", false).IsNull()) {
		secret->secret_map["endpoint"] = NormalizeEndpoint(secret->TryGetValue("endpoint", true).ToString());
	}
	secret->redact_keys = {"api_token"};
	return std::move(secret);
}

void SetD1SecretParameters(CreateSecretFunction &function) {
	function.named_parameters["account_id"] = LogicalType::VARCHAR;
	function.named_parameters["database_id"] = LogicalType::VARCHAR;
	function.named_parameters["api_token"] = LogicalType::VARCHAR;
	function.named_parameters["endpoint"] = LogicalType::VARCHAR;
}

static unique_ptr<SecretEntry> GetSecretByName(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "memory");
	if (secret_entry) {
		return secret_entry;
	}
	secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "local_file");
	if (secret_entry) {
		return secret_entry;
	}
	return nullptr;
}

D1Config ResolveD1ConfigFromSecret(ClientContext &context, const string &secret_name) {
	if (secret_name.empty()) {
		throw BinderException("Secret name is required");
	}
	auto secret_entry = GetSecretByName(context, secret_name);
	if (!secret_entry) {
		throw BinderException("Secret with name \"%s\" not found", secret_name);
	}

	const auto *kv_secret = dynamic_cast<const KeyValueSecret *>(secret_entry->secret.get());
	if (!kv_secret) {
		throw BinderException("Secret \"%s\" is not a key-value secret", secret_name);
	}
	if (StringUtil::Lower(kv_secret->GetType()) != "d1") {
		throw BinderException("Secret \"%s\" is of type \"%s\", expected \"d1\"", secret_name, kv_secret->GetType());
	}

	D1Config config;
	config.endpoint = NormalizeEndpoint(GetSecretStringValue(*kv_secret, "endpoint", false));
	config.account_id = GetSecretStringValue(*kv_secret, "account_id");
	config.database_id = GetSecretStringValue(*kv_secret, "database_id");
	config.api_token = GetSecretStringValue(*kv_secret, "api_token");

	config.Validate();
	return config;
}

} // namespace d1
} // namespace duckdb
