#!/usr/bin/env bash
# Build a self-contained MiniRaytrace.app (Release) and zip it for distribution.
# Prerequisite (build machine only): Vulkan SDK with VULKAN_SDK set.
# The produced .app runs on any Apple Silicon Mac without the SDK.
set -euo pipefail

cd "$(dirname "$0")/.."

# VULKAN_SDK may not be exported in this shell; probe the standard LunarG
# install location (newest version wins). CMake can also find a system-wide
# install (/usr/local/lib), so this is best-effort, not a hard requirement.
if [[ -z "${VULKAN_SDK:-}" ]]; then
    detected=$(ls -d "$HOME"/VulkanSDK/*/macOS 2>/dev/null | sort -V | tail -1 || true)
    if [[ -n "$detected" ]]; then
        export VULKAN_SDK="$detected"
        export PATH="$VULKAN_SDK/bin:$PATH"
        echo "VULKAN_SDK auto-detected: $VULKAN_SDK"
    else
        echo "warning: VULKAN_SDK not set and not found under ~/VulkanSDK;"
        echo "         relying on CMake to locate a system-wide Vulkan install."
    fi
fi

BUILD_DIR=build-release
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMRT_MACOS_BUNDLE=ON \
    -DMRT_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR" -j

APP="$BUILD_DIR/viewer/MiniRaytrace.app"
ZIP="MiniRaytrace-$(date +%Y%m%d).zip"

# Verify the bundle is complete before shipping.
for f in \
    "Contents/MacOS/MiniRaytrace" \
    "Contents/Frameworks/libvulkan.1.dylib" \
    "Contents/Frameworks/libMoltenVK.dylib" \
    "Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"; do
    if [[ ! -e "$APP/$f" ]]; then
        echo "error: bundle is missing $f" >&2
        exit 1
    fi
done

ditto -c -k --keepParent "$APP" "$ZIP"
echo
echo "done:"
echo "  app: $APP"
echo "  zip: $ZIP  (right-click > Open on first launch; ad-hoc signed)"
