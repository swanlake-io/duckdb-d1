PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=d1
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

FORMAT_SHIM_DIR=${PROJ_DIR}scripts/shims
export PATH := ${FORMAT_SHIM_DIR}:$(PATH)

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: lint-strict integration-test live-d1-test coverage

lint-strict:
	./scripts/lint_strict.sh

integration-test: release
	./scripts/run_integration_tests.sh

live-d1-test: release
	./scripts/run_live_d1_tests.sh

coverage:
	./scripts/coverage.sh
