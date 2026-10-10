#!/usr/bin/env bash
# Download Windows MSVC dependency_cache + ccache artifacts from a fork CI run and unpack
# them for a local build tree. Requires: gh, 7z optional (deps are already archives).
#
#   .ci/extract-windows-caches.sh <run-id> [dest-dir]
#
# Defaults dest-dir to C:\rpcs3-ci-cache when running under WSL, else ./ci-cache.
# After extract:
#   - dependency_cache/  → point DEPS_CACHE_DIR here (or copy into a checkout) before setup-windows.sh
#   - ccache/            → set CCACHE_DIR to this path for the next msbuild
set -euo pipefail
RUN="${1:?usage: $0 <run-id> [dest-dir]}"
REPO="${REPO:-jrb00013/rpcs3}"
if [ -n "${2:-}" ]; then
  DEST="$2"
elif [ -d /mnt/c/Users/josep ]; then
  DEST="/mnt/c/rpcs3-ci-cache"
else
  DEST="$(pwd)/ci-cache"
fi
mkdir -p "$DEST/download"
echo "run $RUN → $DEST"
gh run download "$RUN" -R "$REPO" -n "Windows MSVC dependency cache" -D "$DEST/download/deps" || {
  echo "no 'Windows MSVC dependency cache' artifact on run $RUN (need a build after the workflow change)" >&2
  exit 1
}
gh run download "$RUN" -R "$REPO" -n "Windows MSVC ccache" -D "$DEST/download/ccache" || {
  echo "warn: no ccache artifact (deps still usable)" >&2
}
rm -rf "$DEST/dependency_cache" "$DEST/ccache"
mkdir -p "$DEST/dependency_cache"
# upload-artifact may nest or flatten; normalize
if [ -d "$DEST/download/deps/dependency_cache" ]; then
  cp -a "$DEST/download/deps/dependency_cache/." "$DEST/dependency_cache/"
else
  cp -a "$DEST/download/deps/." "$DEST/dependency_cache/"
fi
if [ -d "$DEST/download/ccache" ]; then
  mkdir -p "$DEST/ccache"
  if [ -d "$DEST/download/ccache/ccache" ]; then
    cp -a "$DEST/download/ccache/ccache/." "$DEST/ccache/"
  else
    cp -a "$DEST/download/ccache/." "$DEST/ccache/"
  fi
fi
echo "DEPS_CACHE_DIR=$DEST/dependency_cache"
echo "CCACHE_DIR=$DEST/ccache"
du -sh "$DEST/dependency_cache" "$DEST/ccache" 2>/dev/null || true
