#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace d1 {

string QuoteSQLString(const string &value);
string QuoteIdentifierPart(const string &identifier);
string QuoteMultipartIdentifier(const string &identifier);

LogicalType SQLiteTypeToLogicalType(const string &sqlite_type_decl);
LogicalType LogicalTypeToSQLiteType(const LogicalType &input);

string ValueToSQLLiteral(const Value &value);

} // namespace d1
} // namespace duckdb
