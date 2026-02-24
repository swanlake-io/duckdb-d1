#include "d1/sql_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace d1 {

string QuoteSQLString(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

string QuoteIdentifierPart(const string &identifier) {
	auto part = identifier;
	StringUtil::Trim(part);
	if (part.empty()) {
		throw InvalidInputException("Identifier contains an empty part");
	}
	return "\"" + StringUtil::Replace(part, "\"", "\"\"") + "\"";
}

string QuoteMultipartIdentifier(const string &identifier) {
	vector<string> parts;
	string current;
	for (auto ch : identifier) {
		if (ch == '.') {
			parts.push_back(current);
			current.clear();
		} else {
			current.push_back(ch);
		}
	}
	parts.push_back(current);
	if (parts.empty()) {
		throw InvalidInputException("Identifier cannot be empty");
	}
	for (auto &part : parts) {
		part = QuoteIdentifierPart(part);
	}
	return StringUtil::Join(parts, ".");
}

LogicalType SQLiteTypeToLogicalType(const string &sqlite_type_decl) {
	auto sqlite_type = StringUtil::Lower(sqlite_type_decl);
	if (StringUtil::Contains(sqlite_type, "int")) {
		return LogicalType::BIGINT;
	}
	if (StringUtil::Contains(sqlite_type, "bool")) {
		return LogicalType::BIGINT;
	}
	if (StringUtil::Contains(sqlite_type, "char") || StringUtil::Contains(sqlite_type, "clob") ||
	    StringUtil::Contains(sqlite_type, "text")) {
		return LogicalType::VARCHAR;
	}
	if (StringUtil::Contains(sqlite_type, "blob") || sqlite_type.empty()) {
		return LogicalType::BLOB;
	}
	if (StringUtil::Contains(sqlite_type, "real") || StringUtil::Contains(sqlite_type, "floa") ||
	    StringUtil::Contains(sqlite_type, "doub")) {
		return LogicalType::DOUBLE;
	}
	if (sqlite_type == "date") {
		return LogicalType::DATE;
	}
	if (StringUtil::Contains(sqlite_type, "time")) {
		return LogicalType::TIMESTAMP;
	}
	if (StringUtil::Contains(sqlite_type, "dec") || StringUtil::Contains(sqlite_type, "num")) {
		return LogicalType::DOUBLE;
	}
	return LogicalType::VARCHAR;
}

LogicalType LogicalTypeToSQLiteType(const LogicalType &input) {
	switch (input.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return LogicalType::BIGINT;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return LogicalType::DOUBLE;
	case LogicalTypeId::BLOB:
		return LogicalType::BLOB;
	default:
		return LogicalType::VARCHAR;
	}
}

static string BlobToHexLiteral(const string_t &blob) {
	static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
	string result = "x'";
	for (idx_t i = 0; i < blob.GetSize(); i++) {
		auto byte_val = static_cast<uint8_t>(blob.GetData()[i]);
		result.push_back(HEX_DIGITS[(byte_val >> 4) & 0x0F]);
		result.push_back(HEX_DIGITS[byte_val & 0x0F]);
	}
	result.push_back('\'');
	return result;
}

string ValueToSQLLiteral(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return value.GetValue<bool>() ? "TRUE" : "FALSE";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return to_string(value.GetValue<int64_t>());
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return to_string(value.GetValue<uint64_t>());
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return value.ToString();
	case LogicalTypeId::BLOB:
		return BlobToHexLiteral(value.GetValueUnsafe<string_t>());
	default:
		return QuoteSQLString(value.ToString());
	}
}

} // namespace d1
} // namespace duckdb
