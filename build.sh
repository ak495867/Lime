#!/usr/bin/env bash
# LIME build script.
# Builds the vendored asmjit backend (cached object files) and links
#   - test_lime(.exe)  : full test suite (JIT, x86_64 decoder, async I/O, ...)
#   - lime(.exe)       : the CLI runtime
#
# Usage:  ./build.sh [--clean]
set -e
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++20 -O2 -Iinclude -Ithird_party/asmjit -DASMJIT_STATIC"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    BIN_SUFFIX=".exe"
    LIBS="-lws2_32 -lWinHvPlatform -luser32 -lgdi32"
    ;;
  *)
    BIN_SUFFIX=""
    LIBS=""
    ;;
esac

if [ "$1" = "--clean" ]; then
  rm -rf build
fi

ASMJIT_ROOT=third_party/asmjit/asmjit
OBJ_ROOT=build/asmjit
mkdir -p "$OBJ_ROOT"

# ---------------------------------------------------------------------------
# 1. Compile the vendored asmjit library (cached per translation unit)
# ---------------------------------------------------------------------------
ASMJIT_OBJS=""
for src in "$ASMJIT_ROOT"/core/*.cpp \
           "$ASMJIT_ROOT"/axl/*.cpp \
           "$ASMJIT_ROOT"/arm/*.cpp \
           "$ASMJIT_ROOT"/x86/x86_*.cpp; do
  case "$src" in *_test.cpp) continue ;; esac
  rel="${src#"$ASMJIT_ROOT"/}"
  obj="$OBJ_ROOT/${rel%.cpp}.o"
  mkdir -p "$(dirname "$obj")"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    echo "[CXX] $src"
    $CXX $CXXFLAGS -c "$src" -o "$obj"
  fi
  ASMJIT_OBJS="$ASMJIT_OBJS $obj"
done

# ---------------------------------------------------------------------------
# 2. LIME sources
# ---------------------------------------------------------------------------
LIME_TEST_SRCS=""
for src in src/*.cpp; do
  [ "$src" = "src/main.cpp" ] && continue
  LIME_TEST_SRCS="$LIME_TEST_SRCS $src"
done

echo "[LINK] test_lime$BIN_SUFFIX"
# shellcheck disable=SC2086
$CXX $CXXFLAGS $LIME_TEST_SRCS tests/test_lime.cpp $ASMJIT_OBJS \
     -o "test_lime$BIN_SUFFIX" $LIBS

echo "[LINK] lime$BIN_SUFFIX"
# shellcheck disable=SC2086
$CXX $CXXFLAGS src/*.cpp $ASMJIT_OBJS -o "lime$BIN_SUFFIX" $LIBS

echo "Build complete: test_lime$BIN_SUFFIX, lime$BIN_SUFFIX"
