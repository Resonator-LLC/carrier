#!/usr/bin/env bash
# Build the M1 libjami probe.
#
# Prerequisite: third_party/jami-daemon/build/ must contain a completed
# libjami build producing libjami (static or shared) and any generated
# pkg-config files. See arch/jami-migration.md D18/D19 for context.
#
# Usage:
#   ./tools/build_probe.sh             # build carrier/build/libjami_probe
#   ./tools/build_probe.sh run         # build and run

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
jami="$root/third_party/jami-daemon"
jami_build="$jami/build"
out="$root/build/libjami_probe"

mkdir -p "$root/build"

if [[ ! -d "$jami_build" ]]; then
  echo "error: $jami_build not found — run the cmake build in third_party/jami-daemon first" >&2
  exit 1
fi

# Prefer pkg-config if jami.pc is present; fall back to direct flags.
pc_path="$jami_build:$jami_build/contrib/$(cc -dumpmachine)/lib/pkgconfig"
export PKG_CONFIG_PATH="$pc_path:${PKG_CONFIG_PATH:-}"

if pkg-config --exists jami 2>/dev/null; then
  cflags="$(pkg-config --cflags jami)"
  libs="$(pkg-config --libs --static jami)"
else
  # Fallback: include headers directly, link the built archive.
  cflags="-I$jami/src"
  # The exact library filename varies — try common candidates.
  if   [[ -f "$jami_build/libjami.a" ]]; then libs="$jami_build/libjami.a"
  elif [[ -f "$jami_build/libjami.dylib" ]]; then libs="-L$jami_build -ljami"
  else
    echo "error: no libjami.a or libjami.dylib found in $jami_build" >&2
    exit 1
  fi
  # Supplementary platform libs for macOS — adjust if linking complains.
  libs="$libs -framework CoreFoundation -framework Security -framework SystemConfiguration"
fi

echo "+ cflags: $cflags"
echo "+ libs:   $libs"

set -x
c++ -std=c++20 -O0 -g -Wall -Wextra \
  $cflags \
  "$here/libjami_probe.cc" \
  $libs \
  -o "$out"
set +x

echo "built: $out"

if [[ "${1:-}" == "run" ]]; then
  echo "+ running"
  "$out"
fi
