#pragma once

#include "d1/types.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {
namespace d1 {

unique_ptr<BaseSecret> CreateD1SecretFunction(ClientContext &context, CreateSecretInput &input);
void SetD1SecretParameters(CreateSecretFunction &function);
D1Config ResolveD1ConfigFromSecret(ClientContext &context, const string &secret_name);

} // namespace d1
} // namespace duckdb
