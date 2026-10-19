#!/usr/bin/env bash
# Package a built libvulkan_panfrost.so into the import zip accepted by
# Winlator / Ludashi ("Components -> GPU driver -> Import .zip") and by
# DroidDeck-style apps ("Components -> GPU drivers -> Import .zip").
#
# Package contract (files at the zip ROOT):
#   libvulkan_panfrost.so
#   icd.json          { "ICD": { "library_path": "./libvulkan_panfrost.so", ... } }
#   meta.json         schemaVersion=1 superset (see below)
#
# Usage:
#   ci/package-winlator-driver.sh <path-to-libvulkan_panfrost.so> [outdir]
set -euo pipefail

so="${1:?usage: $0 <libvulkan_panfrost.so> [outdir]}"
outdir="${2:-out}"
test -f "$so" || { echo "error: $so not found" >&2; exit 1; }

mkdir -p "$outdir"
cp "$so" "$outdir/libvulkan_panfrost.so"

# Prefer the NDK llvm-strip (Android ELF); fall back to a host strip.
if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip --strip-unneeded "$outdir/libvulkan_panfrost.so" || true
elif [ -n "${ANDROID_NDK_TOOLCHAIN:-}" ]; then
    "$ANDROID_NDK_TOOLCHAIN/bin/llvm-strip" --strip-unneeded \
        "$outdir/libvulkan_panfrost.so" || true
else
    strip --strip-unneeded "$outdir/libvulkan_panfrost.so" 2>/dev/null || true
fi

sha="$(sha256sum "$outdir/libvulkan_panfrost.so" | cut -d' ' -f1)"
size="$(stat -c%s "$outdir/libvulkan_panfrost.so")"
build_id="$(readelf -n "$outdir/libvulkan_panfrost.so" 2>/dev/null \
    | awk '/Build ID:/{print $3}' || true)"

cat > "$outdir/icd.json" <<'EOF'
{"ICD":{"library_path":"./libvulkan_panfrost.so","api_version":"1.4.363"}}
EOF

G925_DRIVER_VERSION="${G925_DRIVER_VERSION:-PanVK G925 0.1.0-alpha.1}" \
SHA="$sha" SIZE="$size" BUILD_ID="$build_id" OUTDIR="$outdir" \
python3 - <<'PY'
import json, os
outdir = os.environ["OUTDIR"]
sha = os.environ["SHA"]
meta = {
    "schemaVersion": 1,
    "author": "afrizal1211",
    "vendor": "Mesa",
    "name": "PanVK G925 (Mali-G925-Immortalis MC11)",
    # ADPKG requires driverVersion to match ^[0-9]+.[0-9]+.[0-9]+(-.+)?$
    "driverVersion": "1.0.0-mc11",
    "driverVersionLabel": os.environ["G925_DRIVER_VERSION"] + " / Mesa panfrost (kbase CSF)",
    "packageVersion": "0.1.0-alpha.1",
    "libraryName": "libvulkan_panfrost.so",
    "minApi": 34,
    "sanitizedArtifactSha256": sha,
    "description": (
        "PanVK (Mesa) Vulkan ICD for the 11-core Mali-G925-Immortalis "
        "(MT6991/MT8799, Dimensity 9400+) over the vendor mali_kbase CSF "
        "kernel driver (no DRM render node needed). Includes the MC11 "
        "core-count fix so the driver accepts the 11-core shader_present "
        "mask. Alpha build; Winlator/Ludashi/DXVK compatibility is "
        "experimental and this is not a Vulkan conformance claim."
    ),
    "kind": "android-vulkan-icd",
    "abi": "android-aarch64-bionic",
    "libc": "bionic",
    "gpuFamily": "mali",
    "guestBackend": "mali-kbase",
    "maliPanArch": 13,
    "maliKbaseFrontend": "csf",
    "maliKbaseUapiMajor": 1,
    "maliKbaseMinUapiMinor": 30,
    "maliKbaseMaxUapiMinor": 30,
    "coreMask": "0xEE0067",
    "coreCount": 11,
    "gpuId": "0xd8300015",
    "sourceRepo": "afrizal1211/PanVKG925",
    "artifactSize": int(os.environ["SIZE"]),
    "artifactBuildId": os.environ.get("BUILD_ID", ""),
    "icd": "icd.json",
    "files": ["libvulkan_panfrost.so", "icd.json"],
    "filesSha256": {"libvulkan_panfrost.so": sha},
}
with open(os.path.join(outdir, "meta.json"), "w") as f:
    json.dump(meta, f, separators=(",", ":"), sort_keys=True)
print(json.dumps(meta, indent=2))
PY

( cd "$outdir" && rm -f panvk-g925-aarch64.zip && \
  zip -q panvk-g925-aarch64.zip libvulkan_panfrost.so icd.json meta.json )

echo
echo "== package =="
unzip -l "$outdir/panvk-g925-aarch64.zip"
( cd "$outdir" && sha256sum libvulkan_panfrost.so icd.json meta.json \
    panvk-g925-aarch64.zip )
