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

# 2. Build contrib deps with --ignore-system-libs.
#
# We diverge from the daemon's official `cmake .. && make` path here.
# The official path lets contrib's pkg-config / dpkg auto-detection
# substitute system libraries for parts of its dep tree, which is
# correct for an end-user daemon install — the daemon links against
# whatever libssl/libgnutls/etc. the OS package manager provides.
#
# We're producing a *static prefix* that downstream consumers (carrier)
# link against by file path. Anything that auto-detection skips is
# absent from $PREFIX/lib and breaks the consumer's link list. On
# the macos-14 runner image, brew pre-installs openssl/gnutls/zlib;
# on Ubuntu, apt-installed libgnutls28-dev/libssl-dev are visible
# to dpkg-query. Both routes leave libcrypto.a/libssl.a/libgnutls.a
# out of our staged prefix.
#
# --ignore-system-libs forces every contrib package source-built
# regardless of system detection. PKGS_FOUND is wiped at config
# time (contrib/src/main.mak), so iconv (unconditionally added to
# PKGS_FOUND on POSIX) ends up source-built too — and the resulting
# libavformat references GNU's `_libiconv_*` symbols. Carrier's
# Makefile already wildcards libiconv.a / libcharset.a out of the
# prefix to keep the link clean.
#
# Tarballs are pooled across SHA bumps via a symlink: the contrib
# default location (contrib/tarballs) points at $TARBALL_CACHE so
# upstream archives downloaded for one SHA serve every later SHA.
# Using a symlink instead of bootstrap's --cache-dir flag avoids
# contrib's "custom TARBALLS location requires flock" guard, which
# would otherwise need flock(1) — not shipped by default on macOS.
echo "==> Building contrib (may take 30-90 min)"
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

# 3. Build the daemon (cmake). BUILD_CONTRIB=OFF because step 2 already
# built contrib via contrib/native; CMake's auto-build (the README's
# "easy way") would otherwise re-run bootstrap+make in
# contrib/build-${TARGET}, duplicating ~30-90 min of work.
echo "==> Building libjami (cmake)"
rm -rf "$SRC_DIR/build"
(
  cd "$SRC_DIR"
  cmake -S . -B build \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_CONTRIB=OFF
  cmake --build build -j"$NPROC"
)

# 4. Stage artifacts into the prefix.
#
# pjproject and a couple of its siblings (srtp, yuv) name their static
# archives with the full BUILD_TRIPLE — including the macOS kernel
# version (e.g. libpj-aarch64-apple-darwin23.6.0.a). That makes a tarball
# built on one kernel useless to consumers running another kernel, even
# though the ABI is stable for our SDK target. Strip the kernel version
# at staging time so the artefact name matches ARTIFACT_TRIPLE and the
# same prefix serves every macOS host. Linux's BUILD_TRIPLE has no
# kernel version, so this rewrite is a no-op there.
echo "==> Staging to $PREFIX"
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib" "$PREFIX/include/jami"
cp "$SRC_DIR/build/libjami-core.a" "$PREFIX/lib/"
for src in "$SRC_DIR/contrib/$BUILD_TRIPLE/lib/"*.a; do
  base="$(basename "$src")"
  # Replace -<arch>-apple-darwinN.N.N (or -<arch>-linux-gnu) with the
  # kernel-stripped triple. sed -E is portable across BSD and GNU.
  dst="$(echo "$base" | sed -E "s/-($(uname -m)|aarch64|x86_64)-apple-darwin[0-9.]+\.a$/-\1-apple-darwin.a/")"
  cp "$src" "$PREFIX/lib/$dst"
done
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
