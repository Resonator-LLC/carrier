#!/usr/bin/env bash
#
# Build libjami at the SHA pinned in carrier/JAMI_VERSION and stage the
# artifacts into ${XDG_CACHE_HOME:-$HOME/.cache}/resonator/libjami/<sha>/.
#
# Out-of-tree by design: source clone and build tree live under
# .../resonator/libjami-src/<sha>/, never inside the carrier repo. This
# is what removes the in-tree daemon dependency, the dirty-submodule
# state, and the patch workflow (see arch/jami-migration.md D21).
#
# Usage:
#   tools/build-libjami.sh            # build if cache empty, else no-op
#   tools/build-libjami.sh --force    # always rebuild
#   tools/build-libjami.sh --print-prefix   # echo cache prefix and exit
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PIN_FILE="$REPO_ROOT/JAMI_VERSION"

[ -f "$PIN_FILE" ] || { echo "JAMI_VERSION not found at $PIN_FILE" >&2; exit 1; }
SHA="$(tr -d '[:space:]' < "$PIN_FILE")"
[ -n "$SHA" ] || { echo "JAMI_VERSION is empty" >&2; exit 1; }

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/resonator"
SRC_DIR="$CACHE_ROOT/libjami-src/$SHA"
PREFIX="$CACHE_ROOT/libjami/$SHA"

# Two distinct triples:
#   BUILD_TRIPLE  - matches contrib's output dir; includes uname -r on macOS
#   ARTIFACT_TRIPLE - portable name used for the tarball; drops the macOS
#                     kernel version so the same artifact serves macOS hosts
#                     across kernel versions (ABI is stable for our SDK target).
case "$(uname -s)" in
  Darwin)
    BUILD_TRIPLE="$(uname -m)-apple-darwin$(uname -r)"
    ARTIFACT_TRIPLE="$(uname -m)-apple-darwin"
    NPROC="$(sysctl -n hw.ncpu)"
    ;;
  Linux)
    BUILD_TRIPLE="$(uname -m)-linux-gnu"
    ARTIFACT_TRIPLE="$BUILD_TRIPLE"
    NPROC="$(nproc)"
    ;;
  *) echo "unsupported platform: $(uname -s)" >&2; exit 1 ;;
esac

if [ "${1:-}" = "--print-prefix" ]; then
  echo "$PREFIX"
  exit 0
fi

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ "$FORCE" -eq 0 ] && [ -f "$PREFIX/lib/libjami-core.a" ]; then
  echo "libjami already staged at $PREFIX (use --force to rebuild)"
  exit 0
fi

echo "==> libjami $SHA / $ARTIFACT_TRIPLE (build: $BUILD_TRIPLE)"
echo "    source: $SRC_DIR"
echo "    prefix: $PREFIX"
echo "    (cold build is 1-3 hours; subsequent calls reuse the cache)"

# 1. Clone/checkout source out of repo
if [ ! -d "$SRC_DIR/.git" ]; then
  mkdir -p "$(dirname "$SRC_DIR")"
  echo "==> Cloning jami-daemon"
  git clone https://github.com/savoirfairelinux/jami-daemon.git "$SRC_DIR"
fi
git -C "$SRC_DIR" fetch origin "$SHA" 2>/dev/null || true
git -C "$SRC_DIR" checkout --quiet "$SHA"
git -C "$SRC_DIR" submodule update --init --recursive

# 2. Build contrib deps. The contrib system can detect existing system
# libraries (e.g. brew-installed gnutls) and skip vendoring them. That
# leaves pjproject's --with-gnutls=yes pointing at default search paths
# that miss Apple Silicon's /opt/homebrew/include — which is the issue
# the now-deleted 0001-macos-pjproject-gnutls-prefix.patch worked around.
#
# We sidestep it by clearing PKG_CONFIG_LIBDIR for the contrib phase so
# every dep is source-built. Hermetic, slower first time, no patch.
echo "==> Building contrib (hermetic; may take 30-90 min)"
mkdir -p "$SRC_DIR/contrib/native"
(
  cd "$SRC_DIR/contrib/native"
  PKG_CONFIG_LIBDIR="" PKG_CONFIG_PATH="" ../bootstrap
  PKG_CONFIG_LIBDIR="" PKG_CONFIG_PATH="" make -j"$NPROC"
)

# 3. Build the daemon (cmake)
echo "==> Building libjami (cmake)"
rm -rf "$SRC_DIR/build"
(
  cd "$SRC_DIR"
  cmake -S . -B build \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF
  cmake --build build -j"$NPROC"
)

# 4. Stage artifacts into the prefix
echo "==> Staging to $PREFIX"
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib" "$PREFIX/include/jami"
cp "$SRC_DIR/build/libjami-core.a" "$PREFIX/lib/"
cp "$SRC_DIR/contrib/$BUILD_TRIPLE/lib/"*.a "$PREFIX/lib/"
cp "$SRC_DIR/src/jami/"*.h "$PREFIX/include/jami/"

cat > "$PREFIX/MANIFEST" <<EOF
# libjami prefix manifest
sha=$SHA
artifact_triple=$ARTIFACT_TRIPLE
build_triple=$BUILD_TRIPLE
upstream=https://github.com/savoirfairelinux/jami-daemon
built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

echo "==> Done."
echo "    $(ls "$PREFIX/lib/"*.a | wc -l | tr -d ' ') archives, $(du -sh "$PREFIX" | awk '{print $1}') total"
