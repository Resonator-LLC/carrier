#!/usr/bin/env bash
#
# Build libjami at the SHA pinned in carrier/JAMI_VERSION and stage the
# artifacts into ${XDG_CACHE_HOME:-$HOME/.cache}/resonator/libjami/<key>/,
# where <key> selects host vs iOS slice:
#
#   host:                 <sha>/lib/
#   --platform=ios-device:    <sha>-ios-device-arm64/lib/
#   --platform=ios-simulator: <sha>-ios-sim-fat/lib/         (lipo arm64+x86_64)
#
# Out-of-tree by design: source clone and build tree live under
# .../resonator/libjami-src/<sha>/, never inside the carrier repo. This
# is what removes the in-tree daemon dependency, the dirty-submodule
# state, and the patch workflow (see arch/jami-migration.md D21).
#
# Usage:
#   tools/build-libjami.sh                                    # host build
#   tools/build-libjami.sh --platform=ios-device              # iPhone arm64
#   tools/build-libjami.sh --platform=ios-simulator           # Sim arm64+x86_64 (fat)
#   tools/build-libjami.sh --platform=ios-all                 # both iOS slices
#   tools/build-libjami.sh --force                            # rebuild even if cached
#   tools/build-libjami.sh --print-prefix [--platform=...]    # echo cache prefix, exit
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
# Tarball cache shared across SHA bumps. Upstream tarballs are content-addressed
# so a gnutls-3.8.9.tar.xz fetched at one SHA is reusable at the next.
TARBALL_CACHE="$CACHE_ROOT/libjami-tarballs"

# iOS deployment floor — matches Jami's own CI (jami-client-ios/compile-ios.sh).
# Bumping requires checking that libjami's contribs build clean against the
# chosen SDK.
MIN_IOS_VERSION=14.5

# --- arg parsing ------------------------------------------------------------

PLATFORM=host
FORCE=0
PRINT_PREFIX=0
for arg in "$@"; do
  case "$arg" in
    --platform=*) PLATFORM="${arg#--platform=}" ;;
    --force) FORCE=1 ;;
    --print-prefix) PRINT_PREFIX=1 ;;
    -h|--help)
      sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' | head -n 30
      exit 0
      ;;
    *) echo "unknown arg: $arg (try --help)" >&2; exit 1 ;;
  esac
done

case "$PLATFORM" in
  host|ios-device|ios-simulator|ios-all) ;;
  *)
    echo "unknown platform: $PLATFORM (host|ios-device|ios-simulator|ios-all)" >&2
    exit 1
    ;;
esac

# iOS slices cross-compile through xcrun's toolchain; that's macOS-only.
case "$PLATFORM" in
  ios-*)
    [ "$(uname -s)" = "Darwin" ] || {
      echo "iOS builds require a macOS host (got $(uname -s))" >&2
      exit 1
    }
    ;;
esac

# --- per-platform PREFIX / artifact-triple / NPROC --------------------------
#
# ARTIFACT_TRIPLE doubles as cache-key suffix and tarball-name suffix.
# BUILD_TRIPLE is contrib's native-build output dir (host-only); iOS uses
# per-(arch,platform) build dirs so a single BUILD_TRIPLE doesn't apply.

case "$PLATFORM" in
  host)
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
    PREFIX="$CACHE_ROOT/libjami/$SHA"
    ;;
  ios-device)
    BUILD_TRIPLE=""
    ARTIFACT_TRIPLE="ios-device-arm64"
    NPROC="$(sysctl -n hw.ncpu)"
    PREFIX="$CACHE_ROOT/libjami/$SHA-$ARTIFACT_TRIPLE"
    ;;
  ios-simulator)
    BUILD_TRIPLE=""
    ARTIFACT_TRIPLE="ios-sim-fat"
    NPROC="$(sysctl -n hw.ncpu)"
    PREFIX="$CACHE_ROOT/libjami/$SHA-$ARTIFACT_TRIPLE"
    ;;
  ios-all)
    # Composite: just call ourselves once per slice. ios-all has no single
    # prefix path, so --print-prefix is meaningless for it.
    if [ "$PRINT_PREFIX" -eq 1 ]; then
      echo "--print-prefix is per-slice; use --platform=ios-device or ios-simulator" >&2
      exit 2
    fi
    recurse_args=()
    [ "$FORCE" -eq 1 ] && recurse_args+=(--force)
    "$0" --platform=ios-device "${recurse_args[@]+"${recurse_args[@]}"}"
    "$0" --platform=ios-simulator "${recurse_args[@]+"${recurse_args[@]}"}"
    exit 0
    ;;
esac

if [ "$PRINT_PREFIX" -eq 1 ]; then
  echo "$PREFIX"
  exit 0
fi

# Idempotency sentinel: libjami-core.a is the last thing staged, so its presence
# means the slice is complete. --force wipes the prefix unconditionally.
if [ "$FORCE" -eq 0 ] && [ -f "$PREFIX/lib/libjami-core.a" ]; then
  echo "libjami already staged at $PREFIX (use --force to rebuild)"
  exit 0
fi

echo "==> libjami $SHA / $ARTIFACT_TRIPLE [platform=$PLATFORM]"
echo "    source: $SRC_DIR"
echo "    prefix: $PREFIX"
echo "    (cold build is 1-3 hours per slice; subsequent calls reuse the cache)"

# --- shared source clone (all platforms share the same daemon checkout) -----

if [ ! -d "$SRC_DIR/.git" ]; then
  mkdir -p "$(dirname "$SRC_DIR")"
  echo "==> Cloning jami-daemon"
  git clone https://github.com/savoirfairelinux/jami-daemon.git "$SRC_DIR"
fi
git -C "$SRC_DIR" fetch origin "$SHA" 2>/dev/null || true
git -C "$SRC_DIR" checkout --quiet "$SHA"
git -C "$SRC_DIR" submodule update --init --recursive

# --- shared tarball cache symlink (content-addressed; survives SHA bumps) ---

mkdir -p "$TARBALL_CACHE" "$SRC_DIR/contrib"
if [ -d "$SRC_DIR/contrib/tarballs" ] && [ ! -L "$SRC_DIR/contrib/tarballs" ]; then
  for f in "$SRC_DIR/contrib/tarballs"/*; do
    [ -e "$f" ] || continue
    mv -n "$f" "$TARBALL_CACHE/" 2>/dev/null || true
  done
  rm -rf "$SRC_DIR/contrib/tarballs"
fi
ln -sfn "$TARBALL_CACHE" "$SRC_DIR/contrib/tarballs"

# --- iOS helper: gas-preprocessor.pl ---------------------------------------
#
# ffmpeg's arm asm and a couple of contrib packages need gas-preprocessor.pl
# on PATH. Install into $SRC_DIR/extras/tools/build/bin/ (mirroring upstream
# compile-ios.sh) and prepend to PATH. No-op if already on PATH.
ensure_gas_preprocessor() {
  command -v gas-preprocessor.pl >/dev/null && return 0
  local toolbin="$SRC_DIR/extras/tools/build/bin"
  mkdir -p "$toolbin"
  if [ ! -x "$toolbin/gas-preprocessor.pl" ]; then
    echo "==> Installing gas-preprocessor.pl into $toolbin"
    curl -fsSL https://github.com/libav/gas-preprocessor/raw/master/gas-preprocessor.pl \
      -o "$toolbin/gas-preprocessor.pl"
    chmod +x "$toolbin/gas-preprocessor.pl"
  fi
  export PATH="$toolbin:$PATH"
}

# --- iOS per-arch build -----------------------------------------------------
#
# build_one_arch_ios <arch> <sdk> <sdk_platform> <stage_dir> <bitcode_flag>
#   arch:         arm64 | x86_64
#   sdk:          iphoneos | iphonesimulator
#   sdk_platform: iPhoneOS | iPhoneSimulator  (Jami contrib's env-var convention)
#   stage_dir:    where to drop .a files (caller-owned; per-arch for sim, ==PREFIX for device)
#   bitcode_flag: "-fembed-bitcode" for device, "" for simulator
#
# Contrib build/install dirs are per-(arch,sdk) so device + sim builds don't
# clobber each other and incremental rebuilds within a slice are cached.

build_one_arch_ios() {
  local arch="$1"
  local sdk="$2"
  local sdk_platform="$3"
  local stage_dir="$4"
  local bitcode="$5"

  local host_triple
  case "$arch" in
    arm64)  host_triple=aarch64-apple-darwin_ios ;;
    x86_64) host_triple=x86_64-apple-darwin_ios ;;
    *) echo "unknown arch: $arch" >&2; return 1 ;;
  esac

  local min_flag
  case "$sdk" in
    iphoneos)         min_flag="-miphoneos-version-min=$MIN_IOS_VERSION" ;;
    iphonesimulator)  min_flag="-mios-simulator-version-min=$MIN_IOS_VERSION" ;;
    *) echo "unknown sdk: $sdk" >&2; return 1 ;;
  esac

  local sdkroot
  sdkroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  local devpath
  devpath="$(xcrun --sdk "$sdk" --show-sdk-platform-path)/Developer"

  local contrib_build="$SRC_DIR/contrib/native-$arch-$sdk_platform"
  local contrib_install="$SRC_DIR/contrib/$arch-$sdk_platform"
  local cmake_build="$SRC_DIR/build-ios-$arch-$sdk_platform"

  mkdir -p "$contrib_build" "$contrib_install"

  echo "==> [$arch-$sdk_platform] contrib bootstrap+build"
  (
    cd "$contrib_build"
    # Daemon's contrib rules.mak read these via env. BUILDFORIOS=1 toggles
    # the iOS code paths inside contrib/src/*/rules.mak.
    export BUILDFORIOS=1
    export IOS_TARGET_PLATFORM="$sdk_platform"
    export MIN_IOS="$min_flag"
    export SDKROOT="$sdkroot"
    export DEVPATH="$devpath"
    export CC="xcrun -sdk $sdk clang"
    export CXX="xcrun -sdk $sdk clang++"
    export CFLAGS="$min_flag $bitcode -arch $arch -isysroot $sdkroot"
    export CXXFLAGS="$min_flag $bitcode -arch $arch -isysroot $sdkroot"
    export LDFLAGS="$min_flag $bitcode -arch $arch -isysroot $sdkroot"

    # Cross-compile hygiene: GNU autoconf packages (gmp, gnutls, …) probe for
    # a "build system compiler" that can produce binaries runnable on the
    # build host (used for codegen helpers during cross-compile). With the
    # iOS CC/CFLAGS above leaking through, the probe loop tries every
    # candidate compiler with iOS flags appended and fails with
    #   configure: error: Cannot find a build system compiler
    # Setting CC_FOR_BUILD / *_FOR_BUILD to neutral host-only values gives
    # configure the right compiler for the build-host helpers.
    export CC_FOR_BUILD="/usr/bin/cc"
    export CXX_FOR_BUILD="/usr/bin/c++"
    export CPP_FOR_BUILD="/usr/bin/cc -E"
    export CFLAGS_FOR_BUILD=""
    export CXXFLAGS_FOR_BUILD=""
    export CPPFLAGS_FOR_BUILD=""
    export LDFLAGS_FOR_BUILD=""

    # Idempotent: bootstrap rewrites config.mak; make's stamp files cache
    # built packages.
    ../bootstrap \
      --host="$host_triple" \
      --prefix="$contrib_install" \
      --disable-libav \
      --enable-ffmpeg \
      --disable-plugin \
      --disable-libarchive \
      --disable-dbus

    make fetch
    make -j"$NPROC"
  )

  echo "==> [$arch-$sdk_platform] daemon cmake"
  if [ "$FORCE" -eq 1 ] || [ ! -f "$cmake_build/libjami-core.a" ]; then
    rm -rf "$cmake_build"
    (
      cd "$SRC_DIR"
      export PKG_CONFIG_PATH="$contrib_install/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
      cmake -S . -B "$cmake_build" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdkroot" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS_VERSION" \
        -DCMAKE_C_FLAGS="$min_flag $bitcode" \
        -DCMAKE_CXX_FLAGS="$min_flag $bitcode" \
        -DCMAKE_FIND_ROOT_PATH="$contrib_install" \
        -DCMAKE_PREFIX_PATH="$contrib_install" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_CONTRIB=OFF \
        -DJAMI_DBUS=OFF \
        -DJAMI_PLUGIN=OFF \
        -DCMAKE_BUILD_TYPE=Release
      cmake --build "$cmake_build" -j"$NPROC"
    )
  fi

  echo "==> [$arch-$sdk_platform] stage to $stage_dir"
  mkdir -p "$stage_dir/lib"
  cp "$cmake_build/libjami-core.a" "$stage_dir/lib/"
  # Strip iOS host triple suffix at staging time (libpj-aarch64-apple-darwin_ios.a
  # -> libpj.a) so the archive names are toolchain-target agnostic in the prefix.
  local src base dst
  for src in "$contrib_install/lib/"*.a; do
    [ -f "$src" ] || continue
    base="$(basename "$src")"
    dst="$(echo "$base" | sed -E "s/-(aarch64|x86_64|arm64)-apple-darwin_ios\.a$/.a/")"
    cp "$src" "$stage_dir/lib/$dst"
  done
}

# --- main per-platform dispatch ---------------------------------------------

case "$PLATFORM" in
  host)
    # The official daemon path is `cmake .. && make`, which lets contrib's
    # pkg-config / dpkg auto-detection substitute system libraries — correct
    # for an end-user daemon install, wrong for a static prefix that
    # downstream consumers link against by file path. --ignore-system-libs
    # forces every contrib package source-built (wipes PKGS_FOUND, including
    # iconv which is otherwise auto-added on POSIX). Carrier's Makefile
    # wildcards libiconv.a / libcharset.a out of the prefix to keep the
    # link clean.
    echo "==> Building contrib (may take 30-90 min)"
    mkdir -p "$SRC_DIR/contrib/native"
    (
      cd "$SRC_DIR/contrib/native"
      ../bootstrap --ignore-system-libs
      make -j"$NPROC"
    )

    # BUILD_CONTRIB=OFF because step above already built contrib via
    # contrib/native; CMake's auto-build (the README's "easy way") would
    # otherwise re-run bootstrap+make in contrib/build-${TARGET}.
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

    echo "==> Staging to $PREFIX"
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX/lib" "$PREFIX/include/jami"
    cp "$SRC_DIR/build/libjami-core.a" "$PREFIX/lib/"
    # pjproject/srtp/yuv name archives with the full BUILD_TRIPLE — including
    # the macOS kernel version. Strip the kernel version so a tarball built
    # on one kernel serves consumers running another. Linux's BUILD_TRIPLE
    # has no kernel version, so this rewrite is a no-op there.
    for src in "$SRC_DIR/contrib/$BUILD_TRIPLE/lib/"*.a; do
      base="$(basename "$src")"
      dst="$(echo "$base" | sed -E "s/-($(uname -m)|aarch64|x86_64)-apple-darwin[0-9.]+\.a$/-\1-apple-darwin.a/")"
      cp "$src" "$PREFIX/lib/$dst"
    done
    cp "$SRC_DIR/src/jami/"*.h "$PREFIX/include/jami/"
    ;;

  ios-device)
    ensure_gas_preprocessor
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX/lib" "$PREFIX/include/jami"
    build_one_arch_ios arm64 iphoneos iPhoneOS "$PREFIX" "-fembed-bitcode"
    cp "$SRC_DIR/src/jami/"*.h "$PREFIX/include/jami/"
    ;;

  ios-simulator)
    ensure_gas_preprocessor
    # Build each simulator arch into a per-arch stage dir, then lipo the
    # archives together into $PREFIX/lib. Stage dirs live under SRC_DIR so
    # they're naturally per-SHA and don't pollute the cache prefix.
    SIM_STAGE_ARM64="$SRC_DIR/stage-ios-arm64-iPhoneSimulator"
    SIM_STAGE_X86="$SRC_DIR/stage-ios-x86_64-iPhoneSimulator"
    rm -rf "$SIM_STAGE_ARM64" "$SIM_STAGE_X86" "$PREFIX"
    mkdir -p "$PREFIX/lib" "$PREFIX/include/jami"

    build_one_arch_ios arm64  iphonesimulator iPhoneSimulator "$SIM_STAGE_ARM64" ""
    build_one_arch_ios x86_64 iphonesimulator iPhoneSimulator "$SIM_STAGE_X86"   ""

    echo "==> Lipo-merging simulator archives into $PREFIX/lib"
    # Union of archive names across both stages (vpx disabled on arm64 sim
    # in upstream means the set isn't symmetric — see compile-ios.sh's
    # special-case at the bottom).
    bases=$( {
      ls "$SIM_STAGE_ARM64/lib/"*.a 2>/dev/null
      ls "$SIM_STAGE_X86/lib/"*.a   2>/dev/null
    } | xargs -n1 basename | sort -u )
    for base in $bases; do
      slices=""
      [ -f "$SIM_STAGE_ARM64/lib/$base" ] && slices="$slices $SIM_STAGE_ARM64/lib/$base"
      [ -f "$SIM_STAGE_X86/lib/$base"   ] && slices="$slices $SIM_STAGE_X86/lib/$base"
      # shellcheck disable=SC2086  # $slices is intentionally word-split
      set -- $slices
      if [ "$#" -eq 1 ]; then
        cp "$1" "$PREFIX/lib/$base"
      else
        lipo -create "$@" -output "$PREFIX/lib/$base"
      fi
    done

    cp "$SRC_DIR/src/jami/"*.h "$PREFIX/include/jami/"
    ;;
esac

# --- manifest + summary -----------------------------------------------------

cat > "$PREFIX/MANIFEST" <<EOF
# libjami prefix manifest
sha=$SHA
platform=$PLATFORM
artifact_triple=$ARTIFACT_TRIPLE
build_triple=${BUILD_TRIPLE:-n/a}
min_ios_version=${MIN_IOS_VERSION}
upstream=https://github.com/savoirfairelinux/jami-daemon
built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

echo "==> Done."
echo "    $(ls "$PREFIX/lib/"*.a | wc -l | tr -d ' ') archives, $(du -sh "$PREFIX" | awk '{print $1}') total"
