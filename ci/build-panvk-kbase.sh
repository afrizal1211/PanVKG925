#!/usr/bin/env bash
#
# Cross-build the PanVK kbase (mali_kbase) Vulkan driver for Android on
# aarch64 (MT6991 / Mali-G925-Immortalis).
#
# Requires:
#   - Android NDK r27 exposed on PATH (aarch64-linux-android34-clang)
#   - meson, ninja, pkg-config
#   - a libdrm source checkout for the Android target, passed as $LIBDRM_SRC
#     (or otherwise provided via pkg-config).
#
# Usage:
#   ANDROID_NDK_HOME=/path/to/ndk LIBDRM_SRC=/path/to/libdrm ./ci/build-panvk-kbase.sh
#
set -euo pipefail

buildtype="${BUILDTYPE:-release}"
builddir="${BUILDDIR:-build-android-aarch64}"

if [ -n "${ANDROID_NDK_TOOLCHAIN:-}" ]; then
    export PATH="$ANDROID_NDK_TOOLCHAIN/bin:$PATH"
elif [ -n "${ANDROID_NDK_HOME:-}" ]; then
    export PATH="$ANDROID_NDK_HOME/toolchain/llvm/prebuilt/linux-x86_64/bin:$PATH"
fi

if ! command -v aarch64-linux-android34-clang >/dev/null 2>&1; then
    echo "error: aarch64-linux-android34-clang not found; point ANDROID_NDK_HOME at NDK r27" >&2
    exit 1
fi

rm -rf "$builddir"
meson setup "$builddir" \
    --cross-file ci/android-aarch64.ini \
    --buildtype "$buildtype" \
    -Dplatforms=android \
    -Dandroid-stub=true \
    -Dgallium-drivers= \
    -Dvulkan-drivers=panfrost \
    -Dpanfrost-kmds=kbase \
    -Dpanfrost-rust=false \
    -Dglx=disabled \
    -Degl=disabled \
    -Dgallium-va=disabled \
    -Dgallium-vdpau=disabled \
    -Dgallium-xa=disabled \
    -Dvideo-codecs= \
    -Dbuild-tests=false \
    -Dllvm=disabled \
    -Dvalgrind=disabled \
    -Dlibunwind=disabled \
    -Dzstd=disabled

ninja -C "$builddir"

echo
echo "== built Vulkan driver =="
find "$builddir" -name 'libvulkan_panfrost.so' -o -name 'libvulkan_panvk*.so' | while read -r so; do
    echo "  $so"
    file "$so" || true
done
