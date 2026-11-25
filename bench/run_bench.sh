#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DEPS_DIR="${ROOT_DIR}/deps"
RESULTS_DIR="${ROOT_DIR}/bench/results"
mkdir -p "${BUILD_DIR}" "${RESULTS_DIR}"

CXX="${CXX:-clang++}"
CC="${CC:-clang}"
STD_FLAG="${STD_FLAG:--std=c++26}"

if [[ "$(uname -s)" == "Darwin" ]]; then
  ARCH_FLAG="-mcpu=native"
  CPU_FEATURES_SRC="arm/cpu_features.c"
else
  ARCH_FLAG="-march=native"
  if [[ "$(uname -m)" == "aarch64" ]]; then
    CPU_FEATURES_SRC="arm/cpu_features.c"
  else
    CPU_FEATURES_SRC="x86/cpu_features.c"
  fi
fi

# Fetch libdeflate v1.22 into deps/libdeflate if not already present
if [[ ! -d "${DEPS_DIR}/libdeflate" ]]; then
  mkdir -p "${DEPS_DIR}"
  git clone --depth 1 --branch v1.22 https://github.com/ebiggers/libdeflate.git "${DEPS_DIR}/libdeflate"
fi

echo "[1/3] Running ASan + UBSan RFC 1951 & 25,000 differential fuzz tests..."
"${CXX}" ${STD_FLAG} -O2 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -I"${ROOT_DIR}/include" \
  "${ROOT_DIR}/tests/picoflate_test.cc" \
  -lz -o "${BUILD_DIR}/picoflate_tests"
"${BUILD_DIR}/picoflate_tests"

echo "[2/3] Building libdeflate v1.22 + picoflate benchmark (${STD_FLAG} -O3 ${ARCH_FLAG})..."
"${CC}" -O2 -DNDEBUG ${ARCH_FLAG} -I"${DEPS_DIR}/libdeflate" \
  -c "${DEPS_DIR}/libdeflate/lib/deflate_decompress.c" -o "${BUILD_DIR}/deflate_decompress.o"
"${CC}" -O2 -DNDEBUG ${ARCH_FLAG} -I"${DEPS_DIR}/libdeflate" \
  -c "${DEPS_DIR}/libdeflate/lib/utils.c" -o "${BUILD_DIR}/utils.o"
"${CC}" -O2 -DNDEBUG ${ARCH_FLAG} -I"${DEPS_DIR}/libdeflate" \
  -c "${DEPS_DIR}/libdeflate/lib/${CPU_FEATURES_SRC}" -o "${BUILD_DIR}/cpu_features.o"

"${CXX}" ${STD_FLAG} -O3 ${ARCH_FLAG} -Wall -Wextra -Wpedantic -Werror \
  -DPICOFLATE_HAS_LIBDEFLATE=1 \
  -I"${ROOT_DIR}/include" -I"${DEPS_DIR}/libdeflate" \
  "${ROOT_DIR}/bench/picoflate_bench.cc" \
  "${BUILD_DIR}/deflate_decompress.o" "${BUILD_DIR}/utils.o" "${BUILD_DIR}/cpu_features.o" \
  -lz -o "${BUILD_DIR}/picoflate_bench"

echo "[3/3] Running 100,000 iters x 7 rounds benchmark..."
"${BUILD_DIR}/picoflate_bench" \
  --iters 100000 --rounds 7 \
  --csv "${RESULTS_DIR}/summary.csv" \
  --rounds-csv "${RESULTS_DIR}/rounds.csv"

echo "Done! Results written to ${RESULTS_DIR}/summary.csv and ${RESULTS_DIR}/rounds.csv"
