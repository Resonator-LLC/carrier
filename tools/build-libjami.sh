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
# Tarball cache shared across SHA bumps. Upstream tarballs are content-addressed
# so a gnutls-3.8.9.tar.xz fetched at one SHA is reusable at the next.
TARBALL_CACHE="$CACHE_ROOT/libjami-tarballs"

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
# libraries (e.g. brew-installed gnutls, apt-installed opus) and skip
# vendoring them — but its pkg-config plumbing then mis-fires when the
# downstream configure scripts try to actually link them. Notably,
# pjproject's --with-gnutls=yes misses Apple Silicon's /opt/homebrew/include
# (the issue the now-deleted 0001-macos-pjproject-gnutls-prefix.patch
# worked around), and ffmpeg's configure on Ubuntu fails with
# "opus not found" because dpkg-query auto-detection adds opus to
# PKGS_FOUND but its .pc files aren't on the search path used at
# configure time.
#
# --ignore-system-libs forces every dep to be source-built; PKGS_FOUND
# is ignored regardless of pkg-config / dpkg state. Hermetic, slower
# first time, no patch.
#
# Tarballs are pooled across SHA bumps via a symlink: the contrib
# default location (contrib/tarballs) points at $TARBALL_CACHE so
# upstream archives downloaded for one SHA serve every later SHA.
# Using a symlink instead of bootstrap's --cache-dir flag avoids
# contrib's "custom TARBALLS location requires flock" guard, which
# would otherwise need flock(1) — not shipped by default on macOS.
echo "==> Building contrib (hermetic; may take 30-90 min)"
mkdir -p "$SRC_DIR/contrib" "$SRC_DIR/contrib/native" "$TARBALL_CACHE"
# Migrate any pre-existing per-SHA tarballs into the shared cache before
# replacing the directory with a symlink. Idempotent on re-runs.
if [ -d "$SRC_DIR/contrib/tarballs" ] && [ ! -L "$SRC_DIR/contrib/tarballs" ]; then
  for f in "$SRC_DIR/contrib/tarballs"/*; do
    [ -e "$f" ] || continue
    mv -n "$f" "$TARBALL_CACHE/" 2>/dev/null || true
  done
  rm -rf "$SRC_DIR/contrib/tarballs"
fi
ln -sfn "$TARBALL_CACHE" "$SRC_DIR/contrib/tarballs"
(
  cd "$SRC_DIR/contrib/native"
  ../bootstrap --ignore-system-libs
  make -j"$NPROC"
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
