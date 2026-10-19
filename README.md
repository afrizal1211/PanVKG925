# PanVKG925

Mesa fork carrying the experimental **PanVK kbase** Vulkan driver, patched so it
loads on **Mali-G925-Immortalis MC11** (MediaTek Dimensity 9400+ / MT6991, e.g.
POCO X8 Pro Max / Redmi Turbo 5 Max).

Based on the PanVK-kbase work in [`Alhkxsj/mesa`](https://github.com/Alhkxsj/mesa)
(`main`, Mesa `26.3.0-devel`).

## The change

Upstream's kbase probe (`src/panfrost/lib/kmod/kbase_kmod.c`,
`kbase_kmod_supports_panvk()`) requires exactly **12** shader cores for the
G925-Immortalis class. The 9400+/MT6991 bins expose **11** cores
(`shader_present = 0xEE0067`), so the device was rejected before the model
lookup, and `pan_get_model()` would report `Unknown gpu_id or variant`.

The patch relaxes that one check from `== 12` to `>= 10`, matching the existing
`min_core_count = 10` floor already declared for the G925-Immortalis model row in
`src/panfrost/model/pan_model.c`. The true core count is still derived from
`shader_present`, so the device is named **MC11** automatically and nothing else
in the backend assumes 12.

See `ci/panvk-g925-mc11.patch`.

## Verified device facts

| Property | Value |
|---|---|
| SoC | MT6991 (Dimensity 9400+) |
| GPU | Mali-G925-Immortalis, 11 cores, r0p1, `0x0D080300` |
| `core_mask` | `0xEE0067` (11 bits) |
| Driver UAPI | `1.30` (`mali_kbase_mt6991_r49`, DDK `r49p1-03bet0`) |
| MHU VA bits | 48 |
| KMD | `mali_kbase_mt6991_r49` |
| OPP top / bottom | 1612 MHz / 338 MHz |

The only remaining unverified gate is `GPU_FEAT_RAY_INTERSECTION`
(`gpu_features & (1 << 2)`).

## Build

Cross-build for Android aarch64 (NDK r27, API 34):

```sh
ANDROID_NDK_HOME=/path/to/android-ndk-r27 \
LIBDRM_SRC=/path/to/libdrm \
  ./ci/build-panvk-kbase.sh
```

`LIBDRM_SRC` must be a libdrm checkout already installed for the Android target
(its `pkgconfig` dir on `PKG_CONFIG_PATH`). The GitHub Actions workflow
`.github/workflows/build-panvk-g925.yml` does the whole thing — NDK install,
libdrm cross-build, Mesa configure + compile — and uploads
`libvulkan_panfrost.so` as the `panvk-g925-aarch64` artifact.

## Notes

- **Headless only.** The kbase backend has no dma-buf export, so real swapchains
  are gated behind `PANVK_KBASE_WSI_NATIVE` / `PANVK_KBASE_WSI_SW` and the
  device is otherwise built as a compute/headless Vulkan device.
- Experimental: every extension/feature is declared through the
  `PANVK_KBASE_EXPERIMENT` switch and only after passing per-item hardware
  criteria. Do not set it in production.
- This does not replace the vendor OpenGL ES stack; it is a Vulkan driver.
