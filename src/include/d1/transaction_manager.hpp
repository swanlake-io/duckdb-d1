#pragma once

#include "d1/transaction.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {
namespace d1 {

class D1Catalog;

class D1TransactionManager : public TransactionManager {
public:
	D1TransactionManager(AttachedDatabase &db, D1Catalog &catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	D1Catalog &catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<D1Transaction>> transactions;
};

} // namespace d1
} // namespace duckdb
