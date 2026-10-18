#!/usr/bin/env bash
# Re-creates lib/HomeSpan from upstream HomeSpan (pinned tag) and applies the local patch
# that adds the HAP "POST /resource" route needed for HomeKit camera snapshots.
set -euo pipefail

TAG="2.1.8"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone --quiet --depth 1 --branch "$TAG" https://github.com/HomeSpan/HomeSpan.git "$TMP/HomeSpan"

rm -rf "$ROOT/lib/HomeSpan"
mkdir -p "$ROOT/lib/HomeSpan"
cp -R "$TMP/HomeSpan/src" "$TMP/HomeSpan/library.properties" "$TMP/HomeSpan/LICENSE" "$ROOT/lib/HomeSpan/"

patch -d "$ROOT/lib/HomeSpan" -p1 < "$ROOT/patches/homespan-$TAG-resource.patch"

grep -q "POST /resource" "$ROOT/lib/HomeSpan/src/HAP.cpp"
echo "lib/HomeSpan = HomeSpan $TAG + resource patch"
