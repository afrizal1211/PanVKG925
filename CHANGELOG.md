# Changelog

All notable changes to the PanVKG925 driver fork are documented here. The format
is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0-alpha.2] - 2026-10-19

Fix the Winlator/Ludashi force-close by shipping **two capability profiles**,
and start advertising the Vulkan extensions/features a Vulkan device needs to be
usable at all.

### Why alpha.1 force-closed

The kbase backend hid almost every extension and feature behind environment
variables that Winlator/Ludashi never set. With no `PANVK_KBASE_DEVICE`,
`PANVK_KBASE_EXPERIMENT` or `PANVK_KBASE_WSI_*`, the driver:

- did not advertise `VK_KHR_swapchain`, so `vkCreateSwapchainKHR` was NULL;
- did not advertise `VK_KHR_get_memory_requirements2`, `VK_KHR_bind_memory2`
  or `VK_KHR_dedicated_allocation`, which DXVK requires during
  `vkCreateDevice`;
- reported API 1.0 and a device missing `multiview`, `shaderDrawParameters`,
  `fragmentStoresAndAtomics`, etc.

The loader then failed the device-creation/present path and the app aborted.

### Added

- **Two build profiles** selected by the new Meson option
  `-Dpanvk-g925-experimental=<bool>` (baked in as `-DPANVK_G925_EXPERIMENTAL`):
  - `safe` (`false`): always advertises the structural set — the promoted-core
    extensions (memory requirements, bind, dedicated allocation, maintenance
    1/2, image format list, format feature flags 2, SPIR-V 1.4, private data),
    `VK_KHR_driver_properties`, and the software-WSI `VK_KHR_swapchain` path.
  - `experimental` (`true`): additionally enables the hardware-candidate
    features DXVK probes for (`multiview`, `variablePointers`,
    `samplerYcbcrConversion`, `shaderDrawParameters`,
    `fragmentStoresAndAtomics`, `descriptorIndexing`, `bufferDeviceAddress`,
    ...). Not hardware-validated on every G925 bin — opt-in only.
- **Software WSI on by default** in both profiles (`panvk_kbase_wsi_enabled()`),
  so a swapchain exists without env vars. Opt out with
  `PANVK_KBASE_WSI_DISABLE=1`; `PANVK_KBASE_WSI_NATIVE=1` still selects the
  native X11/DRI3 path.
- **`PANVK_KBASE_DEVICE` defaults to `/dev/mali0`**, so the kbase ICD enumerates
  out of the box. Set it to another node or to an empty value to override.
- CI now builds **both** profiles and packages
  `panvk-g925-aarch64-safe.zip` and `panvk-g925-aarch64-experimental.zip`.
- `ci/panvk-g925-winlator-caps.patch` captures the capability/WSI changes.

### Changed

- `ci/package-winlator-driver.sh` takes a `safe|experimental` argument and
  writes variant-aware `meta.json` (`variant`, `experimentalFeatures`).

## [0.1.0-alpha.1] - 2026-10-19

First packaged release: a reproducible, CI-built **PanVK kbase** Vulkan ICD for
the 11-core Mali-G925-Immortalis, shipped as an Android Driver Package (ADPKG)
zip that Winlator / Ludashi / DroidDeck can import.

### Added

- **MC11 core-count fix.** `kbase_kmod_supports_panvk()` in
  `src/panfrost/lib/kmod/kbase_kmod.c` now accepts `shader_present` bit counts
  `>= 10` instead of requiring exactly `12`. The MT6991 / Dimensity 9400+
  Mali-G925-Immortalis exposes **11** cores (`core_mask = 0xEE0067`), so the
  device was previously rejected before the panfrost model lookup. Mirrored as
  `ci/panvk-g925-mc11.patch`.
- **Two-stage cross build.** The driver's internal shader generators
  (`mesa_clc`, `vtn_bindgen2`, `panfrost_compile`) are now built for the
  x86_64 host with `-Dmesa-clc=enabled -Dprecomp-compiler=enabled`, then the
  Android aarch64 target is configured with `-Dmesa-clc=system
  -Dprecomp-compiler=system -Dllvm=disabled`. This resolves the hard build
  dependency of panfrost-vk (`with_driver_using_cl`) on Mesa's CLC without
  linking LLVM into the target.
- **Winlator / Ludashi packaging.** `ci/package-winlator-driver.sh` and a CI
  step produce `libvulkan_panfrost.so` + `meta.json` (ADPKG `schemaVersion=1`)
  + `icd.json` at the zip root, plus a `SHA256SUMS` file.
- **GitHub Actions workflow** `.github/workflows/build-panvk-g925.yml` that
  installs NDK r27, cross-builds libdrm 2.4.123 for Android aarch64, builds
  SPIRV-Tools v2024.4 + SPIRV-LLVM-Translator `llvm_release_180` from source,
  builds the host generators, compiles the driver, and uploads the package.
- **README** install instructions for the ADPKG import flow.

### Fixed

- NDK toolchain path: `toolchains/` (plural), not `toolchain/`.
- Removed non-existent Meson options (`gallium-vdpau`, `gallium-xa`).
- Scoped the Android libdrm `pkg-config` path to the cross configure step only,
  so the native host generators no longer link an aarch64 `libdrm.so` into an
  x86-64 binary.
- `driverVersion` in `meta.json` now matches the ADPKG regex
  `^[0-9]+.[0-9]+.[0-9]+(-.+)?$` (`1.0.0-mc11`); the human-readable build label
  moved to `driverVersionLabel`.

### Known limitations

- **No real swapchain / presentation.** The kbase backend has no dma-buf export;
  swapchains are gated behind `PANVK_KBASE_WSI_NATIVE` / `PANVK_KBASE_WSI_SW`.
  Winlator/DXVK presentation is experimental and may fall back to copies.
- Feature/extension exposure is gated behind `PANVK_KBASE_EXPERIMENT`; no Vulkan
  conformance claim is made.
- Build is an alpha; game/DXVK compatibility has not been broadly validated.
- `libvulkan_panfrost.so` links `libdrm.so` and `libc++_shared.so`; the loader
  environment must provide them (both are present on modern Android/bionic
  runtimes).
