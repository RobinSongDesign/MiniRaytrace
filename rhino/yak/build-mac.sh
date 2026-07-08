#!/usr/bin/env bash
# Stages the macOS yak package layout (design doc §6.3 P2 / §6.4 / §8 F2) and,
# if the yak CLI is available, builds the .yak file.
#
# Unlike Windows, the release build here loads libMoltenVK.dylib directly
# (dladdr-located next to libmrt.dylib, volk custom loader path) instead of
# going through a system Vulkan loader + ICD json - macOS has no system
# Vulkan loader at all, and the plugin directory isn't an app bundle, so the
# viewer's dev-time loader+ICD approach doesn't apply here (design doc §6.2).
#
# UNVERIFIED: written without access to macOS hardware in this environment
# (see docs/rhino-integration-design.md risk RH1/§11). Signing/notarization
# is a separate step - see sign-mac.sh - and must run before this package is
# distributed (unsigned dylibs are Gatekeeper-blocked once yak's install
# gives them a quarantine attribute).
#
# Usage: rhino/yak/build-mac.sh [-c Configuration] [-o OidnDir]
#   -c  dotnet build configuration (default: Release)
#   -o  directory containing libOpenImageDenoise*.dylib (issue F4); omit to
#       ship without OIDN (degrades to pure accumulation, PRD R5)

set -euo pipefail

CONFIGURATION="Release"
OIDN_DIR=""
while getopts "c:o:" opt; do
  case "$opt" in
    c) CONFIGURATION="$OPTARG" ;;
    o) OIDN_DIR="$OPTARG" ;;
    *) echo "usage: $0 [-c Configuration] [-o OidnDir]" >&2; exit 1 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RHINO_PROJ="$REPO_ROOT/rhino/MiniRaytrace.Rhino/MiniRaytrace.Rhino.csproj"
STAGING="$SCRIPT_DIR/staging/mac"

echo "== building MiniRaytrace.Rhino ($CONFIGURATION) =="
dotnet build "$RHINO_PROJ" -c "$CONFIGURATION"

BIN_DIR="$REPO_ROOT/rhino/MiniRaytrace.Rhino/bin/$CONFIGURATION"
NATIVE_DYLIB="${MRT_NATIVE_DYLIB:-$REPO_ROOT/build-mac/capi/libmrt.dylib}"
MOLTENVK_DYLIB="${MRT_MOLTENVK_DYLIB:-}"

if [[ ! -f "$NATIVE_DYLIB" ]]; then
  echo "libmrt.dylib not found at $NATIVE_DYLIB - build the native 'mrt' CMake target on macOS first" \
       "(see docs/rhino-integration-design.md §6.2/§6.4), or set MRT_NATIVE_DYLIB." >&2
  exit 1
fi
if [[ -z "$MOLTENVK_DYLIB" || ! -f "$MOLTENVK_DYLIB" ]]; then
  echo "MRT_MOLTENVK_DYLIB not set (or file missing) - release macOS packages must bundle" \
       "libMoltenVK.dylib directly (design doc §6.2/§6.3 P2). Point it at the Vulkan SDK's" \
       "macOS/lib/libMoltenVK.dylib." >&2
  exit 1
fi

echo "== staging $STAGING =="
rm -rf "$STAGING"
mkdir -p "$STAGING/runtimes/osx-arm64/native"

cp "$BIN_DIR/MiniRaytrace.Rhino.rhp" "$STAGING/"
cp "$NATIVE_DYLIB" "$STAGING/runtimes/osx-arm64/native/"
cp "$MOLTENVK_DYLIB" "$STAGING/runtimes/osx-arm64/native/"
cp "$SCRIPT_DIR/manifest.yml" "$STAGING/"

if [[ -n "$OIDN_DIR" ]]; then
  echo "== staging OIDN from $OIDN_DIR (issue F4) =="
  cp "$OIDN_DIR"/libOpenImageDenoise*.dylib "$STAGING/runtimes/osx-arm64/native/" 2>/dev/null || true
fi

echo "== staged files =="
find "$STAGING" -type f | sed "s|$STAGING/|  |"

echo ""
echo "Next: run sign-mac.sh against $STAGING/runtimes/osx-arm64/native/*.dylib BEFORE"
echo "'yak build --platform mac' (design doc §6.5 / issue F3) - yak packages install with a"
echo "quarantine attribute and Gatekeeper will block unsigned dylibs."
