#include "d1/transaction_manager.hpp"

#include "d1/catalog.hpp"

namespace duckdb {
namespace d1 {

D1TransactionManager::D1TransactionManager(AttachedDatabase &db, D1Catalog &catalog)
    : TransactionManager(db), catalog(catalog) {
}

Transaction &D1TransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<D1Transaction>(catalog, *this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData D1TransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	(void)context;
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void D1TransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

void D1TransactionManager::Checkpoint(ClientContext &context, bool force) {
	(void)context;
	(void)force;
}

} // namespace d1
} // namespace duckdb
