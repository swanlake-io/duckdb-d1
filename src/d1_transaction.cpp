#include "d1/transaction.hpp"

#include "d1/catalog.hpp"

namespace duckdb {
namespace d1 {

D1Transaction::D1Transaction(D1Catalog &catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), d1_catalog(catalog) {
}

D1Transaction &D1Transaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<D1Transaction>();
}

} // namespace d1
} // namespace duckdb
