#!/usr/bin/env bash
# Codesign + notarize the macOS native dylibs before packaging (design doc
# §6.5 / issue F3). Requires a Developer ID Application certificate in the
# local keychain and a `notarytool` credentials profile already stored via
# `xcrun notarytool store-credentials` - neither of which this environment
# has (no Apple Developer account here), so this script is UNVERIFIED and
# provided as the documented pipeline for whoever runs it on real macOS
# hardware with real credentials.
#
# ⚠️ Per design doc §6.5's own risk note: bare dylibs (not an .app bundle)
# can't be `stapled` - Gatekeeper validates them online against the
# notarization ticket instead, so a clean-machine/offline install check is
# part of F3's acceptance, not just "notarytool submit succeeded".
#
# Usage: rhino/yak/sign-mac.sh <staging-dir> <Developer ID Application: NAME (TEAMID)> <notarytool-profile-name>
#   e.g. rhino/yak/sign-mac.sh rhino/yak/staging/mac "Developer ID Application: Example LLC (ABCDE12345)" mini-raytrace-notary

set -euo pipefail

STAGING="${1:?usage: sign-mac.sh <staging-dir> <signing-identity> <notarytool-profile>}"
IDENTITY="${2:?usage: sign-mac.sh <staging-dir> <signing-identity> <notarytool-profile>}"
PROFILE="${3:?usage: sign-mac.sh <staging-dir> <signing-identity> <notarytool-profile>}"

NATIVE_DIR="$STAGING/runtimes/osx-arm64/native"
if [[ ! -d "$NATIVE_DIR" ]]; then
  echo "no $NATIVE_DIR - run build-mac.sh first" >&2
  exit 1
fi

shopt -s nullglob
DYLIBS=("$NATIVE_DIR"/*.dylib)
if [[ ${#DYLIBS[@]} -eq 0 ]]; then
  echo "no .dylib files found under $NATIVE_DIR" >&2
  exit 1
fi

echo "== codesign (Developer ID Application, timestamped) =="
for f in "${DYLIBS[@]}"; do
  echo "  $f"
  codesign --force --timestamp --options runtime --sign "$IDENTITY" "$f"
  codesign --verify --verbose "$f"
done

echo "== zip for notarization (bare dylibs can't be stapled - §6.5) =="
ZIP="$STAGING/../notarize-payload.zip"
rm -f "$ZIP"
(cd "$NATIVE_DIR" && zip -q "$ZIP" -- *.dylib)

echo "== submitting to notarytool (profile: $PROFILE) =="
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait

echo "== done. Verify Gatekeeper acceptance on a clean/offline machine before shipping (F3 acceptance) =="
