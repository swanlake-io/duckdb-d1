#pragma once

#include "duckdb/transaction/transaction.hpp"

namespace duckdb {
namespace d1 {

class D1Catalog;

class D1Transaction : public Transaction {
public:
	D1Transaction(D1Catalog &catalog, TransactionManager &manager, ClientContext &context);

	static D1Transaction &Get(ClientContext &context, Catalog &catalog);

	D1Catalog &GetD1Catalog() {
		return d1_catalog;
	}

private:
	D1Catalog &d1_catalog;
};

} // namespace d1
} // namespace duckdb
