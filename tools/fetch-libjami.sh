#!/usr/bin/env bash
#
# Download the pre-built libjami tarball for this host's triple at the SHA
# pinned in carrier/JAMI_VERSION, verify sha256, and extract into
# ${XDG_CACHE_HOME:-$HOME/.cache}/resonator/libjami/<key>/, where <key>
# selects host vs iOS slice (matches build-libjami.sh's layout):
#
#   host:                     <sha>/
#   --platform=ios-device:    <sha>-ios-device-arm64/
#   --platform=ios-simulator: <sha>-ios-sim-fat/
#
# Idempotent: if the prefix already has libjami-core.a, exits cleanly.
# Falls back to `make libjami-build` instructions if the artefact for this
# triple isn't published yet.
#
# Tarballs are produced by .github/workflows/build-libjami-artifacts.yml
# and uploaded to a release tagged `libjami-<sha>` on this repo. The iOS
# slices are added in Cut 8 (impl-plan §3.2); until then `--platform=ios-*`
# routes to source build via the fallback message.
#
# Usage:
#   tools/fetch-libjami.sh                            # host
#   tools/fetch-libjami.sh --platform=ios-device      # iPhone arm64
#   tools/fetch-libjami.sh --platform=ios-simulator   # Sim arm64+x86_64 fat
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PIN_FILE="$REPO_ROOT/JAMI_VERSION"

[ -f "$PIN_FILE" ] || { echo "JAMI_VERSION not found at $PIN_FILE" >&2; exit 1; }
SHA="$(tr -d '[:space:]' < "$PIN_FILE")"
[ -n "$SHA" ] || { echo "JAMI_VERSION is empty" >&2; exit 1; }

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/resonator"

PLATFORM=host
for arg in "$@"; do
  case "$arg" in
    --platform=*) PLATFORM="${arg#--platform=}" ;;
    *) echo "unknown arg: $arg" >&2; exit 1 ;;
  esac
done

case "$PLATFORM" in
  host|ios-device|ios-simulator) ;;
  *)
    echo "unknown platform: $PLATFORM (host|ios-device|ios-simulator)" >&2
    exit 1
    ;;
esac

# Compute the artifact triple (cache-key suffix and tarball-name suffix).
# Use the portable artifact triple for host (no macOS kernel version) so the
# same tarball works across macOS hosts at different kernel versions.
case "$PLATFORM" in
  host)
    case "$(uname -s)" in
      Darwin) TRIPLE="$(uname -m)-apple-darwin" ;;
      Linux)  TRIPLE="$(uname -m)-linux-gnu" ;;
      *) echo "unsupported platform: $(uname -s)" >&2; exit 1 ;;
    esac
    PREFIX="$CACHE_ROOT/libjami/$SHA"
    ;;
  ios-device)
    TRIPLE="ios-device-arm64"
    PREFIX="$CACHE_ROOT/libjami/$SHA-$TRIPLE"
    ;;
  ios-simulator)
    TRIPLE="ios-sim-fat"
    PREFIX="$CACHE_ROOT/libjami/$SHA-$TRIPLE"
    ;;
esac

if [ -f "$PREFIX/lib/libjami-core.a" ]; then
  echo "libjami already staged at $PREFIX"
  exit 0
fi

# Repo URL: override via JAMI_RELEASE_REPO. Default matches antenna's
# carrier submodule URL (https://github.com/Resonator-LLC/carrier).
RELEASE_REPO="${JAMI_RELEASE_REPO:-Resonator-LLC/carrier}"
TAG="libjami-$SHA"
ASSET="libjami-$SHA-$TRIPLE.tar.zst"
URL="https://github.com/$RELEASE_REPO/releases/download/$TAG/$ASSET"
SHA256_URL="$URL.sha256"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Fetching $URL"
if ! curl -fL --retry 3 -o "$TMP/$ASSET" "$URL" 2>/dev/null; then
  cat <<EOF >&2
ERROR: pre-built artefact not found for $TRIPLE at $TAG.

Either the release hasn't been published yet for this triple (iOS tarballs
are added in Cut 8 / impl-plan §3.2 — until then iOS slices source-build
on every dev machine), or the SHA in JAMI_VERSION doesn't have a
corresponding release. Build from source:

  tools/build-libjami.sh --platform=$PLATFORM

(or trigger .github/workflows/build-libjami-artifacts.yml on $RELEASE_REPO)
EOF
  exit 1
fi

curl -fL --retry 3 -o "$TMP/$ASSET.sha256" "$SHA256_URL"

echo "==> Verifying sha256"
( cd "$TMP" && shasum -a 256 -c "$ASSET.sha256" )

echo "==> Extracting to $CACHE_ROOT/libjami/"
mkdir -p "$CACHE_ROOT/libjami"
# Tarball is packaged as <key>/lib/...; extract directly.
tar --use-compress-program='zstd -d' -xf "$TMP/$ASSET" -C "$CACHE_ROOT/libjami"

[ -f "$PREFIX/lib/libjami-core.a" ] || {
  echo "ERROR: extraction completed but $PREFIX/lib/libjami-core.a missing" >&2
  exit 1
}

echo "==> Done. $(ls "$PREFIX/lib/"*.a | wc -l | tr -d ' ') archives, $(du -sh "$PREFIX" | awk '{print $1}') total."
