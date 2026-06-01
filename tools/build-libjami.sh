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
#
# Exported so Jami's contrib/src/main.mak picks it up (its default is 9.3,
# which makes std::filesystem::path unavailable in C++17/20 contribs like
# dhtnet — they need iOS 13.0+ for filesystem).
export MIN_IOS_VERSION=14.5

# Android NDK target. API 26 (Android 8.0) is the floor: the daemon's AAudio
# audio layer (src/media/audio/aaudio) calls AAudioStream_* which the NDK marks
# unavailable before API 26. Jami's own android cross-file uses 29; 26 is the
# minimum that compiles. minSdk in the Station plugin + host app must match.
# arm64-v8a is the only ABI we ship today (Apple-silicon emulators run arm64
# system images, and it covers every modern physical device). Add x86_64 only
# if an Intel emulator / Chromebook ever becomes a target.
ANDROID_API_LEVEL=26
ANDROID_ABI_DEFAULT=arm64-v8a

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
  host|ios-device|ios-simulator|ios-all|android-arm64) ;;
  *)
    echo "unknown platform: $PLATFORM (host|ios-device|ios-simulator|ios-all|android-arm64)" >&2
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

# Android slices cross-compile through the NDK's clang toolchain (works on a
# macOS or Linux host). Resolve the NDK now so --print-prefix and the build
# share one path: ANDROID_NDK_HOME, else ANDROID_NDK, else the newest NDK under
# the SDK. Pin ONE NDK and reuse it for Cargokit (android.ndkVersion) too —
# mismatched libc++ ABIs fail at link/runtime.
case "$PLATFORM" in
  android-*)
    [ -n "${ANDROID_NDK_HOME:-}" ] || ANDROID_NDK_HOME="${ANDROID_NDK:-}"
    if [ -z "$ANDROID_NDK_HOME" ]; then
      _sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
      ANDROID_NDK_HOME="$(ls -d "$_sdk"/ndk/* 2>/dev/null | sort -V | tail -1)"
    fi
    [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ] || {
      echo "android build needs an NDK: set ANDROID_NDK_HOME (resolved '$ANDROID_NDK_HOME')" >&2
      exit 1
    }
    export ANDROID_NDK_HOME
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
  android-arm64)
    BUILD_TRIPLE=""
    ARTIFACT_TRIPLE="android-arm64"
    case "$(uname -s)" in
      Darwin) NPROC="$(sysctl -n hw.ncpu)" ;;
      *)      NPROC="$(nproc)" ;;
    esac
    PREFIX="$CACHE_ROOT/libjami/$SHA-$ARTIFACT_TRIPLE"
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
    # build host (used for codegen helpers during cross-compile). Two leaks
    # have to be plugged for the probe to succeed.
    #
    # 1. iOS CFLAGS in `$CFLAGS` are NOT consumed by the probe (it ignores
    #    autoconf CFLAGS and uses a bare `$CC_FOR_BUILD conftest.c -o
    #    conftest` command), so the bare `cc/gcc` candidates would normally
    #    compile cleanly — except for...
    # 2. `$SDKROOT` is honored by clang as a fallback for `-isysroot`. We
    #    exported it above pointing at iPhoneSimulator.sdk / iPhoneOS.sdk
    #    so `xcrun` finds the right SDK for the iOS CC. A bare `/usr/bin/cc`
    #    invocation then inherits SDKROOT, produces a Mach-O with
    #    LC_BUILD_VERSION platform=IOSSIMULATOR, and macOS dyld refuses to
    #    exec it. GMP's probe runs the compiled conftest, so the run fails
    #    and GMP reports either "Cannot find a build system compiler"
    #    (no explicit CC_FOR_BUILD) or "Specified CC_FOR_BUILD doesn't
    #    seem to work" (explicit /usr/bin/cc).
    #
    # Fix: set CC_FOR_BUILD with an explicit `-isysroot` to the macOSX SDK.
    # The command-line `-isysroot` overrides the SDKROOT env, so the build-
    # host compiler emits a macOS Mach-O that dyld will run.
    local macos_sdk
    macos_sdk="$(xcrun --sdk macosx --show-sdk-path)"
    export CC_FOR_BUILD="/usr/bin/cc -isysroot $macos_sdk"
    export CXX_FOR_BUILD="/usr/bin/c++ -isysroot $macos_sdk"
    export CPP_FOR_BUILD="/usr/bin/cc -isysroot $macos_sdk -E"
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

# --- Android per-arch build -------------------------------------------------
#
# build_one_arch_android <arch> <stage_dir>
#   arch:      arm64 (maps to ABI arm64-v8a; the only slice we ship today)
#   stage_dir: where to drop .a files (== PREFIX for the single-arch case)
#
# Cross-compiles through the NDK clang toolchain — no xcrun, no gas-preprocessor
# (clang's integrated assembler handles arm64). The contrib build honours
# env-set CC/CXX/AR/... (contrib/src/main.mak guards each with `origin`, so an
# environment value wins over the `$(CROSS_COMPILE)gcc` default the NDK lacks)
# and reads ANDROID_NDK/ANDROID_ABI/ANDROID_API at bootstrap time
# (contrib/bootstrap check_android_sdk → HAVE_ANDROID, which routes cmake
# packages through $(ANDROID_NDK)/build/cmake/android.toolchain.cmake).

build_one_arch_android() {
  local arch="$1"
  local stage_dir="$2"

  local abi host_triple
  case "$arch" in
    arm64) abi="$ANDROID_ABI_DEFAULT"; host_triple=aarch64-linux-android ;;
    *) echo "unknown android arch: $arch" >&2; return 1 ;;
  esac

  local ndk_host_tag
  case "$(uname -s)" in
    Darwin) ndk_host_tag=darwin-x86_64 ;;   # NDK ships only x86_64 prebuilts (run under Rosetta)
    Linux)  ndk_host_tag=linux-x86_64 ;;
    *) echo "unsupported android build host: $(uname -s)" >&2; return 1 ;;
  esac
  local ndk_bin="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$ndk_host_tag/bin"
  local clang="$ndk_bin/${host_triple}${ANDROID_API_LEVEL}-clang"
  [ -x "$clang" ] || {
    echo "NDK clang not found: $clang (ANDROID_NDK_HOME=$ANDROID_NDK_HOME)" >&2
    return 1
  }

  local contrib_build="$SRC_DIR/contrib/native-android-$arch"
  local contrib_install="$SRC_DIR/contrib/$host_triple"
  local cmake_build="$SRC_DIR/build-android-$arch"

  mkdir -p "$contrib_build" "$contrib_install"

  echo "==> [android-$arch] contrib bootstrap+build (NDK $ndk_host_tag, API $ANDROID_API_LEVEL, ABI $abi)"
  (
    cd "$contrib_build"
    export PATH="$ndk_bin:$PATH"
    # Consumed by contrib/bootstrap check_android_sdk + main.mak's HAVE_ANDROID
    # cmake recipe.
    export ANDROID_NDK="$ANDROID_NDK_HOME"
    export ANDROID_ABI="$abi"
    export ANDROID_API="android-$ANDROID_API_LEVEL"
    # NDK toolchain. clang wrapper bakes in --target + API; llvm-* handle the
    # LTO bitcode the android contrib emits (cmake recipe forces IPO=ON).
    export CC="$clang"
    export CXX="${clang}++"
    export AR="$ndk_bin/llvm-ar"
    export RANLIB="$ndk_bin/llvm-ranlib"
    export STRIP="$ndk_bin/llvm-strip"
    export NM="$ndk_bin/llvm-nm"
    export LD="$ndk_bin/ld.lld"
    export CCAS="$clang"

    # Autoconf cross probes (gmp/gnutls/nettle) compile AND run a conftest on
    # the build host. Unlike iOS there's no SDKROOT leak to break a bare cc, so
    # the host compiler emits a host-runnable binary directly.
    export CC_FOR_BUILD=/usr/bin/cc
    export CXX_FOR_BUILD=/usr/bin/c++
    export CPP_FOR_BUILD="/usr/bin/cc -E"
    export CFLAGS_FOR_BUILD=""
    export CXXFLAGS_FOR_BUILD=""
    export CPPFLAGS_FOR_BUILD=""
    export LDFLAGS_FOR_BUILD=""

    # Cross-compile pkg-config hygiene. Restrict pkg-config to the contrib
    # prefix so packages don't auto-detect the BUILD host's libraries. Upstream
    # Jami's android contrib doesn't pass --without-brotli/--without-zstd to
    # gnutls (unlike its iOS arm), so on a macOS host gnutls's configure finds
    # Homebrew's libbrotli*/libzstd .pc files and enables RFC 8879 cert
    # compression — leaving Brotli*/ZSTD_* undefined in the final .so (no
    # target archive provides them → dlopen fails). LIBDIR replaces the default
    # system search path; contrib deps (nettle, …) still resolve from here.
    export PKG_CONFIG_LIBDIR="$contrib_install/lib/pkgconfig"

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

  echo "==> [android-$arch] daemon cmake"
  if [ "$FORCE" -eq 1 ] || [ ! -f "$cmake_build/libjami-core.a" ]; then
    rm -rf "$cmake_build"
    (
      cd "$SRC_DIR"
      export PKG_CONFIG_PATH="$contrib_install/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
      cmake -S . -B "$cmake_build" \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="android-$ANDROID_API_LEVEL" \
        -DANDROID_STL=c++_shared \
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

  echo "==> [android-$arch] stage to $stage_dir"
  mkdir -p "$stage_dir/lib"
  cp "$cmake_build/libjami-core.a" "$stage_dir/lib/"
  # Strip the android host-triple suffix (-aarch64-unknown-linux-android24.a)
  # off pj*/srtp/yuv archives so the prefix uses flat names (libpj.a). build.rs
  # then uses an empty pj suffix on android — same shape as the iOS staging.
  local src base dst
  for src in "$contrib_install/lib/"*.a; do
    [ -f "$src" ] || continue
    base="$(basename "$src")"
    dst="$(echo "$base" | sed -E "s/-(aarch64|arm|armv7a|x86_64|i686)(-unknown)?-linux-android(eabi)?[0-9]*\.a$/.a/")"
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

  android-arm64)
    rm -rf "$PREFIX"
    mkdir -p "$PREFIX/lib" "$PREFIX/include/jami"
    build_one_arch_android arm64 "$PREFIX"
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
