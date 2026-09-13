#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# scripts/build.sh - ZL build driver (POSIX counterpart of scripts/build.bat).
#
# The default build is the CORE developer build: the ZL executable, the package
# manager, the binding generator, and every compiler/runtime source they depend
# on. Tests, benchmarks and examples are NOT in that graph; they are reached
# through explicit modes.
#
#   build.sh          core toolchain     (target: zl-core)
#   build.sh test     core + all tests   (target: zl-tests)
#   build.sh full     everything         (target: zl-full)
#   build.sh clean    remove build dir
#   build.sh help     show this help
# ---------------------------------------------------------------------------
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SOURCE_DIR}/build"
CONFIG="Release"
GENERATOR=""
TARGET=""
JOBS="4"
DO_CONFIGURE=1
DO_BUILD=1
DO_CLEAN=0
DO_RUN=0
DO_CTEST=0
MODE="core"
RUN_FILE="${SOURCE_DIR}/examples/Hello.zl"
EXTRA_CMAKE_ARGS=()

usage() {
	cat <<'EOF'

Usage: scripts/build.sh [mode] [options]

Modes (the default is "core"):
  (none) / core    Build only the ZL toolchain: zl_language, zlpkg, zl-bind
                   and every compiler/runtime source they need.
                   CMake target: zl-core
  test             Build the core toolchain plus every C++ test executable.
                   CMake target: zl-tests   (run them with ctest)
  full             Build the complete repository on purpose: core, tests,
                   benchmark and example prerequisites.
                   CMake target: zl-full
  clean            Delete the build directory.
  help             Show this help.

The default build never pulls in tests, stress tests, benchmarks, examples or
auxiliary targets - they exist, but only behind "test" and "full".

Options:
  --debug              Configure and build Debug instead of Release
  --release            Configure and build Release
  --clean              Delete the build directory before building
  --ctest              Build the tests and then run ctest
  --reconfigure        Force the configure step
  --no-configure       Skip the configure step
  --build-only         Build without configuring
  --configure-only     Configure without building
  --build-dir PATH     Use a different build directory
  --source-dir PATH    Use a different source directory
  --generator NAME     Pass an explicit CMake generator
  --target NAME        Build one specific target (overrides the mode)
  --jobs N             Parallel build jobs (default: 4)
  --cmake-arg VALUE    Append an extra cmake configure argument
  --run [FILE]         Run build/zl_language after building
  -h, --help           Show this help

Examples:
  scripts/build.sh
  scripts/build.sh test
  scripts/build.sh test --ctest
  scripts/build.sh full --jobs 8
  scripts/build.sh clean
  scripts/build.sh --debug --target zl_language
  scripts/build.sh --run examples/Hello.zl
EOF
}

missing_value() { echo "Missing value for $1" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
	case "$1" in
		-h|--help|help) usage; exit 0 ;;
		core)    MODE="core"; shift ;;
		test|tests) MODE="test"; shift ;;
		full|all)   MODE="full"; shift ;;
		clean)   MODE="clean"; shift ;;
		--debug)   CONFIG="Debug"; shift ;;
		--release) CONFIG="Release"; shift ;;
		--clean)   DO_CLEAN=1; shift ;;
		--ctest)   DO_CTEST=1; MODE="test"; shift ;;
		--reconfigure)   DO_CONFIGURE=1; shift ;;
		--no-configure)  DO_CONFIGURE=0; shift ;;
		--build-only)    DO_CONFIGURE=0; DO_BUILD=1; shift ;;
		--configure-only) DO_BUILD=0; shift ;;
		--run)
			DO_RUN=1; shift
			if [[ $# -gt 0 && "$1" != --* ]]; then RUN_FILE="$1"; shift; fi
			;;
		--build-dir)  [[ $# -ge 2 ]] || missing_value "$1"; BUILD_DIR="$2"; shift 2 ;;
		--source-dir) [[ $# -ge 2 ]] || missing_value "$1"; SOURCE_DIR="$2"; shift 2 ;;
		--generator)  [[ $# -ge 2 ]] || missing_value "$1"; GENERATOR="$2"; shift 2 ;;
		--target)     [[ $# -ge 2 ]] || missing_value "$1"; TARGET="$2"; shift 2 ;;
		--jobs)       [[ $# -ge 2 ]] || missing_value "$1"; JOBS="$2"; shift 2 ;;
		--cmake-arg)  [[ $# -ge 2 ]] || missing_value "$1"; EXTRA_CMAKE_ARGS+=("$2"); shift 2 ;;
		*) echo "Unknown option: $1" >&2; usage; exit 1 ;;
	esac
done

BUILD_DIR="${BUILD_DIR%/}"
SOURCE_DIR="${SOURCE_DIR%/}"

if [[ "${BUILD_DIR}" == "${SOURCE_DIR}" ]]; then
	echo "Refusing to use the source directory as the build directory." >&2
	exit 1
fi

# --- clean mode: remove the build output and stop --------------------------
if [[ "${MODE}" == "clean" ]]; then
	if [[ -d "${BUILD_DIR}" ]]; then
		echo "[ZL] Cleaning \"${BUILD_DIR}\"..."
		rm -rf "${BUILD_DIR}"
		if [[ -d "${BUILD_DIR}" ]]; then
			echo "[ZL] Failed to remove \"${BUILD_DIR}\"." >&2
			exit 1
		fi
	else
		echo "[ZL] Nothing to clean: \"${BUILD_DIR}\" does not exist."
	fi
	echo "[ZL] Clean complete."
	exit 0
fi

if [[ ${DO_CLEAN} -eq 1 && -d "${BUILD_DIR}" ]]; then
	echo "[ZL] Cleaning \"${BUILD_DIR}\"..."
	rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

# --- pick the aggregate target for the selected mode ------------------------
# --target overrides the mode, so single-target builds still work.
if [[ -n "${TARGET}" ]]; then
	BUILD_TARGET="${TARGET}"
	MODE_LABEL="target ${TARGET}"
else
	case "${MODE}" in
		test) BUILD_TARGET="zl-tests"; MODE_LABEL="test" ;;
		full) BUILD_TARGET="zl-full";  MODE_LABEL="full" ;;
		*)    BUILD_TARGET="zl-core";  MODE_LABEL="core" ;;
	esac
fi

if [[ ${DO_CONFIGURE} -eq 1 ]]; then
	echo "[ZL] Configuring ${MODE_LABEL} build (${CONFIG}) in \"${BUILD_DIR}\"..."
	cmake_args=(-S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CONFIG}")
	[[ -n "${GENERATOR}" ]] && cmake_args+=(-G "${GENERATOR}")
	[[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]] && cmake_args+=("${EXTRA_CMAKE_ARGS[@]}")
	cmake "${cmake_args[@]}" || exit $?
fi

if [[ ${DO_BUILD} -eq 1 ]]; then
	case "${MODE_LABEL}" in
		core)
			echo "[ZL] Building the core toolchain (compiler, runtime, VM, required tools)..."
			echo "[ZL] Tests, benchmarks and examples are excluded - use \"build.sh test\" or \"build.sh full\"."
			;;
		test) echo "[ZL] Building the core toolchain and all test executables..." ;;
		full) echo "[ZL] Building the complete repository (core + tests + benchmarks + examples)..." ;;
		*)    echo "[ZL] Building ${MODE_LABEL}..." ;;
	esac
	echo "[ZL] cmake --build \"${BUILD_DIR}\" --config ${CONFIG} --parallel ${JOBS} --target ${BUILD_TARGET}"
	if ! cmake --build "${BUILD_DIR}" --config "${CONFIG}" --parallel "${JOBS}" --target "${BUILD_TARGET}"; then
		status=$?
		echo "[ZL] Build FAILED (see the compiler/CMake output above)." >&2
		exit ${status}
	fi
	case "${MODE_LABEL}" in
		core) echo "[ZL] Core build complete." ;;
		test) echo "[ZL] Test build complete. Run the suite with: ctest --test-dir \"${BUILD_DIR}\"" ;;
		full) echo "[ZL] Full build complete." ;;
	esac
fi

if [[ ${DO_CTEST} -eq 1 ]]; then
	echo "[ZL] Running ctest..."
	ctest --test-dir "${BUILD_DIR}" --output-on-failure -C "${CONFIG}" || exit $?
fi

if [[ ${DO_RUN} -eq 1 ]]; then
	EXE="${BUILD_DIR}/zl_language"
	if [[ ! -x "${EXE}" ]]; then
		echo "Could not find \"${EXE}\"." >&2
		exit 1
	fi
	echo "[ZL] Running \"${EXE}\" \"${RUN_FILE}\"..."
	exec "${EXE}" "${RUN_FILE}"
fi

echo "[ZL] Done."
exit 0
