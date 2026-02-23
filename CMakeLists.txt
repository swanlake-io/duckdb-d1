cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME d1)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

find_package(OpenSSL REQUIRED)

include_directories(src/include)
include_directories(duckdb/third_party/httplib)
include_directories(third_party)

set(EXTENSION_SOURCES
    src/d1_extension.cpp
    src/d1_secret.cpp
    src/d1_transport_httplib.cpp
    src/d1_client.cpp
    src/d1_functions.cpp
    src/d1_sql_utils.cpp
    src/d1_storage.cpp
    src/d1_catalog.cpp
    src/d1_schema_entry.cpp
    src/d1_table_entry.cpp
    src/d1_scan.cpp
    src/d1_transaction.cpp
    src/d1_transaction_manager.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_compile_definitions(${EXTENSION_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
