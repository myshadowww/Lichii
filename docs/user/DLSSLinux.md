# NVIDIA DLSS replacement on Linux

Linux x86-64 builds can use native NVIDIA DLSS Super Resolution and DLAA for the game
passes Lichii already supports. This uses the existing **Use NVIDIA DLSS in place of a
game's DLSS (experimental)** setting in Graphics. It requires an NVIDIA RTX GPU, the
NVIDIA driver with its NGX component, and Vulkan rendering.

## Setup

1. Enable the NVIDIA DLSS setting in Graphics, globally or in the game's settings.
2. Select **Download NVIDIA DLSS…**, review NVIDIA's license, and accept the download.
   Lichii downloads the supported Linux runtime from NVIDIA and verifies its SHA-256.
   Alternatively, **Import Linux DLSS…** accepts the same supported runtime file.
3. Start the game. Setting changes and runtime installation take effect on the next game start.

The panel distinguishes an installed runtime from a compatible device and successful
evaluations. “Ready; waiting for a supported game pass” means the backend initialized;
it does not mean that the current game has used DLSS. Missing components and runtime
failures appear in the panel and log. Existing game-specific fallback behavior applies.

The runtime is downloaded separately; Lichii packages do not contain NVIDIA's model,
DLSS5VKLayer, or OptiScaler. No layer installation or Wine process is required.

## Supported behavior

Linux shares Windows' recognized-pass selection, color/depth/motion inputs, jitter,
reset, output composition, and game-specific policies. This does not add support for
every game's DLSS network. Ocarina's existing motion and scene/HUD composition use the
same backend interface. Fire Emblem's spatial workaround remains the default; its
existing `EDEN_NX2_DLSS_REPLACEMENT=dlss` override is still required to select DLSS.

This implements SR/DLAA, not the separate DLSS 5 neural-rendering effect demonstrated
with DLSS5VKLayer. It does not add frame generation or ray reconstruction.

## Files and diagnostics

The setup panel's **Open folder** button opens `ngx` inside Lichii's user-data directory.
The supported runtime is `libnvidia-ngx-dlss.so.310.9.16` from NVIDIA DLSS SDK revision
`9981c6350dc58c0ce21f024fd65e31e851c31689`.

Runtime search order:

1. `LICHII_NGX_DIR`, when set.
2. The user-data directory's `ngx` folder, including portable-mode user data.
3. An `ngx` folder beside the executable.

Each folder is checked for the supported filename or `libnvidia-ngx-dlss.so`. The latter
supports manually managed installations; the in-app download/import flow accepts only
the pinned model. NGX cache files go in `ngx` under Lichii's cache directory. The driver
core, `libnvidia-ngx.so.1`, is loaded from the system library search path.

`EDEN_NX2_DLSS_LOG=1` adds NGX diagnostics. Existing preset/HDR/exposure and replacement
diagnostics retain their Windows meanings. Device readiness is logged as
`NVIDIA DLSS is available through native Linux NGX`; successful evaluation counts are
shown in the settings panel. Restart the game after resolving a reported failure.

## Building and checking the backend

`ENABLE_NVIDIA_DLSS` defaults on for Linux x86-64 and supported Windows builds.
`-DENABLE_NVIDIA_DLSS=OFF` excludes the backend and hides its settings controls.
Linux builds fetch four pinned NVIDIA interface headers with verified hashes. An offline
build can instead use `-DLICHII_NGX_SDK=/path/to/sdk`, containing `include/nvsdk_ngx_*.h`.
No NVIDIA static library is linked, and runtime model files are not copied by CMake.

With `YUZU_TESTS=ON`, build the optional `ngx_smoke` target and run:

```sh
cmake --build build --target ngx_smoke
build/bin/ngx_smoke /path/to/directory/containing/the/model 0
```

The last argument selects the Vulkan GPU index. This does not boot the emulator. It uses
Lichii's Vulkan device, memory allocator, scheduler, and upscaler; checks missing/disabled
runtime behavior; reads back SR/DLAA output; tests continuous frames, history reset,
resolution and HDR/depth-mode changes; injects an evaluation failure to check output clearing
and fallback; and repeats device startup/shutdown. It writes isolated logs/cache under a temporary directory.
It is deliberately excluded from `ctest`, since it requires NVIDIA hardware and the
separately obtained model. Disable external graphics layers when testing this backend.

A passing synthetic test does not establish temporal quality in a game. Release validation
also needs Ocarina gameplay, camera motion, scene/HUD composition, game switching, and
matched Windows output/performance checks. The Windows backend continues to use Streamline.

## Implementation references

- [NVIDIA Linux SDK/model](https://github.com/NVIDIA/DLSS/tree/9981c6350dc58c0ce21f024fd65e31e851c31689/lib/Linux_x86_64)
- [NVIDIA Vulkan interface](https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/include/nvsdk_ngx_vk.h)
- [NVIDIA on native Linux NGX](https://github.com/nvpro-samples/vk_streamline/issues/4)
- [OptiScaler native Linux support request](https://github.com/optiscaler/OptiScaler/issues/815)

The loader resolves the driver core dynamically. Its `Init_ProjectID_Ext` and `Shutdown1`
entry points use the core ABI, which differs from the static SDK wrapper declarations;
those differences are isolated in `ngx_api.h`. Extensions are requested before device
creation, and NGX evaluates on Lichii's existing command buffer. Normal frames introduce
no CPU image copies or extra queue-idle waits. Reconfiguration waits for prior work before
releasing features or output images; shutdown releases upscalers before the NGX device.
