#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace d1 {

void RegisterD1TableFunctions(ExtensionLoader &loader);

} // namespace d1
} // namespace duckdb
