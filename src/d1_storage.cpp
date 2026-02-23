#include "d1/storage.hpp"

#include "d1/catalog.hpp"
#include "d1/secret.hpp"
#include "d1/transaction_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"

namespace duckdb {
namespace d1 {

namespace {

static Value GetAttachOption(const AttachOptions &attach_options, const string &name) {
	auto entry = attach_options.options.find(name);
	if (entry == attach_options.options.end()) {
		return Value();
	}
	return entry->second;
}

static bool TryGetAttachString(const AttachOptions &attach_options, const string &name, string &out) {
	auto value = GetAttachOption(attach_options, name);
	if (value.IsNull()) {
		return false;
	}
	Value casted;
	if (!value.DefaultTryCastAs(LogicalType::VARCHAR, casted, nullptr)) {
		throw BinderException("ATTACH option \"%s\" must be VARCHAR", name);
	}
	out = casted.GetValue<string>();
	return true;
}

static bool TryGetAttachBoolean(const AttachOptions &attach_options, const string &name, bool &out) {
	auto value = GetAttachOption(attach_options, name);
	if (value.IsNull()) {
		return false;
	}
	Value casted;
	if (!value.DefaultTryCastAs(LogicalType::BOOLEAN, casted, nullptr)) {
		throw BinderException("ATTACH option \"%s\" must be BOOLEAN", name);
	}
	out = casted.GetValue<bool>();
	return true;
}

static bool TryGetAttachUBigInt(const AttachOptions &attach_options, const string &name, idx_t &out) {
	auto value = GetAttachOption(attach_options, name);
	if (value.IsNull()) {
		return false;
	}
	Value casted;
	if (!value.DefaultTryCastAs(LogicalType::UBIGINT, casted, nullptr)) {
		throw BinderException("ATTACH option \"%s\" must be UBIGINT", name);
	}
	out = static_cast<idx_t>(casted.GetValue<uint64_t>());
	return true;
}

static D1Config ResolveAttachConfig(ClientContext &context, const AttachInfo &info,
                                    const AttachOptions &attach_options) {
	D1Config config;
	string secret_name;
	TryGetAttachString(attach_options, "secret", secret_name);
	if (secret_name.empty() && !info.path.empty()) {
		// Allow ATTACH '<secret_name>' ... as a compact form.
		secret_name = info.path;
	}
	if (!secret_name.empty()) {
		config = ResolveD1ConfigFromSecret(context, secret_name);
	}

	string override_value;
	if (TryGetAttachString(attach_options, "endpoint", override_value)) {
		config.endpoint = override_value;
	}
	if (TryGetAttachString(attach_options, "account_id", override_value)) {
		config.account_id = override_value;
	}
	if (TryGetAttachString(attach_options, "database_id", override_value)) {
		config.database_id = override_value;
	}
	if (TryGetAttachString(attach_options, "api_token", override_value)) {
		config.api_token = override_value;
	}

	if (config.endpoint.empty()) {
		config.endpoint = "https://api.cloudflare.com/client/v4";
	}
	config.Validate();
	return config;
}

static unique_ptr<Catalog> D1Attach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                    AttachedDatabase &db, const string &name, AttachInfo &info,
                                    AttachOptions &attach_options) {
	(void)storage_info;
	auto &db_config = DBConfig::GetConfig(context);
	if (!db_config.options.enable_external_access) {
		throw PermissionException("Attaching D1 databases is disabled through configuration");
	}

	auto config = ResolveAttachConfig(context, info, attach_options);
	bool read_only_option = false;
	if (TryGetAttachBoolean(attach_options, "read_only", read_only_option) && read_only_option) {
		attach_options.access_mode = AccessMode::READ_ONLY;
	}

	idx_t schema_cache_ttl_seconds = 30;
	TryGetAttachUBigInt(attach_options, "schema_cache_ttl", schema_cache_ttl_seconds);

	return make_uniq<D1Catalog>(db, std::move(config), attach_options.access_mode, schema_cache_ttl_seconds);
}

static unique_ptr<TransactionManager> D1CreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                 AttachedDatabase &db, Catalog &catalog) {
	(void)storage_info;
	return make_uniq<D1TransactionManager>(db, catalog.Cast<D1Catalog>());
}

} // namespace

D1StorageExtension::D1StorageExtension() {
	attach = D1Attach;
	create_transaction_manager = D1CreateTransactionManager;
}

} // namespace d1
} // namespace duckdb
