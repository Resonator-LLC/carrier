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
# Tarballs are produced by .gitlab-ci.yml and uploaded to the carrier
# project's GitLab Generic Package Registry under package `libjami`,
# version `<sha>` (file `libjami-<sha>-<triple>.tar.zst`).
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

# Pre-built tarballs live in the carrier project's GitLab Generic Package
# Registry: package `libjami`, version `<sha>`, file `<asset>`. Override the
# API host or project path via env. Auth: inside CI use CI_JOB_TOKEN; on a
# dev machine set JAMI_GITLAB_TOKEN to a token with read_package_registry.
GITLAB_API="${JAMI_GITLAB_API:-https://source.resonator.network/api/v4}"
GITLAB_PROJECT="${JAMI_GITLAB_PROJECT:-resonator%2Fcarrier}"
ASSET="libjami-$SHA-$TRIPLE.tar.zst"
URL="$GITLAB_API/projects/$GITLAB_PROJECT/packages/generic/libjami/$SHA/$ASSET"
SHA256_URL="$URL.sha256"

if [ -n "${CI_JOB_TOKEN:-}" ]; then
  AUTH_HEADER="JOB-TOKEN: $CI_JOB_TOKEN"
elif [ -n "${JAMI_GITLAB_TOKEN:-}" ]; then
  AUTH_HEADER="PRIVATE-TOKEN: $JAMI_GITLAB_TOKEN"
else
  cat <<EOF >&2
ERROR: no GitLab credential for the libjami package registry.

Set JAMI_GITLAB_TOKEN to a personal/deploy token with read_package_registry
scope (CI provides CI_JOB_TOKEN automatically). Or build from source:

  tools/build-libjami.sh --platform=$PLATFORM
EOF
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Fetching $URL"
if ! curl -fL --retry 3 --header "$AUTH_HEADER" -o "$TMP/$ASSET" "$URL" 2>/dev/null; then
  cat <<EOF >&2
ERROR: pre-built artefact not found for $TRIPLE at sha $SHA.

Either the package hasn't been published for this triple yet, the SHA in
JAMI_VERSION has no published package, or the token lacks access. Build from
source:

  tools/build-libjami.sh --platform=$PLATFORM

(or run carrier's .gitlab-ci.yml publish pipeline to build + upload it)
EOF
  exit 1
fi

curl -fL --retry 3 --header "$AUTH_HEADER" -o "$TMP/$ASSET.sha256" "$SHA256_URL"

echo "==> Verifying sha256"
(
  cd "$TMP"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c "$ASSET.sha256"
  else
    shasum -a 256 -c "$ASSET.sha256"
  fi
)

echo "==> Extracting to $CACHE_ROOT/libjami/"
mkdir -p "$CACHE_ROOT/libjami"
# Tarball is packaged as <key>/lib/...; extract directly.
tar --use-compress-program='zstd -d' -xf "$TMP/$ASSET" -C "$CACHE_ROOT/libjami"

[ -f "$PREFIX/lib/libjami-core.a" ] || {
  echo "ERROR: extraction completed but $PREFIX/lib/libjami-core.a missing" >&2
  exit 1
}

echo "==> Done. $(ls "$PREFIX/lib/"*.a | wc -l | tr -d ' ') archives, $(du -sh "$PREFIX" | awk '{print $1}') total."
