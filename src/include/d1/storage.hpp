#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {
namespace d1 {

class D1StorageExtension : public StorageExtension {
public:
	D1StorageExtension();
};

} // namespace d1
} // namespace duckdb
