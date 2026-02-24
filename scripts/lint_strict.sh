#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

log() {
    local ts
    ts="$(date '+%Y-%m-%d %H:%M:%S')"
    echo "[lint][${ts}] $*"
}

run_with_heartbeat() {
    local label="$1"
    shift
    local heartbeat_sec="${LINT_HEARTBEAT_SEC:-20}"

    log "START ${label}"
    "$@" &
    local cmd_pid=$!

    while kill -0 "${cmd_pid}" >/dev/null 2>&1; do
        sleep "${heartbeat_sec}"
        if kill -0 "${cmd_pid}" >/dev/null 2>&1; then
            log "${label} is still running..."
        fi
    done

    if wait "${cmd_pid}"; then
        log "DONE ${label}"
        return 0
    fi

    local status=$?
    log "FAILED ${label} (exit ${status})"
    return "${status}"
}

trap 'log "Command failed: ${BASH_COMMAND}"' ERR

CPU_COUNT="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
if command -v ninja >/dev/null 2>&1; then
    export GEN="${GEN:-ninja}"
fi
log "Build generator: ${GEN:-default (Makefiles)}"

log "Preparing Python lint environment"
LINT_VENV="${ROOT_DIR}/build/lint-venv"
if [[ ! -x "${LINT_VENV}/bin/python3" ]]; then
    run_with_heartbeat "create lint virtualenv" python3 -m venv "${LINT_VENV}"
fi
if ! "${LINT_VENV}/bin/python3" -c "import black, cmakelang" >/dev/null 2>&1; then
    run_with_heartbeat "install Python lint deps" "${LINT_VENV}/bin/python3" -m pip install "black>=24" "cmakelang"
fi
export PATH="${LINT_VENV}/bin:${PATH}"

if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy is required for strict lint checks" >&2
    exit 1
fi

if ! command -v clang-format-11 >/dev/null 2>&1 && ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format (preferably clang-format-11) is required for strict lint checks" >&2
    exit 1
fi

# Enforce deterministic formatting and strict static analysis on extension sources.
run_with_heartbeat "format-check" make format-check

TIDY_RULESET='-*,clang-analyzer-*,bugprone-*,performance-*'
TIDY_THREADS="${TIDY_THREADS:-${CPU_COUNT}}"
RUN_TIDY=1
if [[ "${LINT_TIDY_DIFF:-0}" == "1" ]]; then
    BASE_BRANCH="${GIT_BASE_BRANCH:-main}"
    if git rev-parse --verify "origin/${BASE_BRANCH}" >/dev/null 2>&1; then
        if git diff --name-only --diff-filter=ACMR "origin/${BASE_BRANCH}...HEAD" -- src | rg -q '\.(c|cc|cpp|cxx)$'; then
            log "Detected C/C++ changes under src/ vs origin/${BASE_BRANCH}; running clang-tidy"
        else
            RUN_TIDY=0
            log "No C/C++ changes under src/ vs origin/${BASE_BRANCH}; skipping clang-tidy"
        fi
    else
        echo "Base branch origin/${BASE_BRANCH} not found in diff mode." >&2
        echo "Run 'git fetch origin ${BASE_BRANCH}' or set LINT_TIDY_DIFF=0." >&2
        exit 1
    fi
fi

if [[ "${RUN_TIDY}" == "1" ]]; then
    log "clang-tidy scope: extension sources in src/ only"
    run_with_heartbeat "tidy-check (extension sources)" \
        env TIDY_THREADS="${TIDY_THREADS}" TIDY_CHECKS="${TIDY_RULESET}" make tidy-check
fi

if [[ "${LINT_SKIP_RELEASE_BUILD:-1}" != "1" ]]; then
    # Ensure no compiler warnings slip through for extension code.
    run_with_heartbeat "release build (warnings as errors)" \
        env CMAKE_BUILD_PARALLEL_LEVEL="${CPU_COUNT}" TREAT_WARNINGS_AS_ERRORS=1 make release
else
    log "Skipping full release build (strict mode is extension-only)"
fi

log "Strict lint checks passed"
