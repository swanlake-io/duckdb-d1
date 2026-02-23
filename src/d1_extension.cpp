#define DUCKDB_EXTENSION_MAIN

#include "d1_extension.hpp"

#include "d1/functions.hpp"
#include "d1/secret.hpp"
#include "d1/storage.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Register D1 table functions.
	d1::RegisterD1TableFunctions(loader);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.storage_extensions["d1"] = make_uniq<d1::D1StorageExtension>();

	// Register D1 secret type + provider.
	SecretType secret_type;
	secret_type.name = "d1";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction d1_secret_function = {"d1", "config", d1::CreateD1SecretFunction};
	d1::SetD1SecretParameters(d1_secret_function);
	loader.RegisterFunction(d1_secret_function);
}

void D1Extension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string D1Extension::Name() {
	return "d1";
}

std::string D1Extension::Version() const {
#ifdef EXT_VERSION_D1
	return EXT_VERSION_D1;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(d1, loader) {
	duckdb::LoadInternal(loader);
}
}
