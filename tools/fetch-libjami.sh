#!/usr/bin/env bash
#
# Download the pre-built libjami tarball for this host's triple at the SHA
# pinned in carrier/JAMI_VERSION, verify sha256, and extract into
# ${XDG_CACHE_HOME:-$HOME/.cache}/resonator/libjami/<sha>/.
#
# Idempotent: if the prefix already has libjami-core.a, exits cleanly.
# Falls back to `make libjami-build` instructions if the artefact for this
# triple isn't published yet.
#
# Tarballs are produced by .github/workflows/build-libjami-artifacts.yml
# and uploaded to a release tagged `libjami-<sha>` on this repo.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PIN_FILE="$REPO_ROOT/JAMI_VERSION"

[ -f "$PIN_FILE" ] || { echo "JAMI_VERSION not found at $PIN_FILE" >&2; exit 1; }
SHA="$(tr -d '[:space:]' < "$PIN_FILE")"
[ -n "$SHA" ] || { echo "JAMI_VERSION is empty" >&2; exit 1; }

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/resonator"
PREFIX="$CACHE_ROOT/libjami/$SHA"

case "$(uname -s)" in
  Darwin) TRIPLE="$(uname -m)-apple-darwin$(uname -r)" ;;
  Linux)  TRIPLE="$(uname -m)-linux-gnu" ;;
  *) echo "unsupported platform: $(uname -s)" >&2; exit 1 ;;
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

Either the release hasn't been published yet for this triple, or the SHA
in JAMI_VERSION doesn't have a corresponding release. Build from source:

  make libjami-build

(or trigger .github/workflows/build-libjami-artifacts.yml on $RELEASE_REPO)
EOF
  exit 1
fi

curl -fL --retry 3 -o "$TMP/$ASSET.sha256" "$SHA256_URL"

echo "==> Verifying sha256"
( cd "$TMP" && shasum -a 256 -c "$ASSET.sha256" )

echo "==> Extracting to $CACHE_ROOT/libjami/"
mkdir -p "$CACHE_ROOT/libjami"
# Tarball is packaged as <sha>/lib/...; extract directly.
tar --use-compress-program='zstd -d' -xf "$TMP/$ASSET" -C "$CACHE_ROOT/libjami"

[ -f "$PREFIX/lib/libjami-core.a" ] || {
  echo "ERROR: extraction completed but $PREFIX/lib/libjami-core.a missing" >&2
  exit 1
}

echo "==> Done. $(ls "$PREFIX/lib/"*.a | wc -l | tr -d ' ') archives, $(du -sh "$PREFIX" | awk '{print $1}') total."
