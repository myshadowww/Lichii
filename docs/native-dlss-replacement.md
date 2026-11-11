# Game-DLSS replacement

The Vulkan renderer can replace a recognized game-DLSS evaluation with NVIDIA DLSS on
Linux or Windows, then AMD FSR 3.1.4 if NVIDIA DLSS is disabled or unavailable. FSR uses AMD's
eight temporal-upscaling passes, including depth, motion, history and optional RCAS.
This is separate from the presentation filters that scale the finished window image.

Fire Emblem uses temporal replacement when the NVIDIA DLSS setting is enabled, with FSR
fallback if NVIDIA DLSS is unavailable. With the setting disabled it uses the jitter-only
spatial workaround. Explicit `fsr` and `dlss` comparison modes also enable temporal replacement
for its validated input contract. The upstream
temporal blend and RCAS fallback remain available if a temporal backend is unavailable.
Invalid consumed inputs leave the native dispatch enabled. An already-rejected native
tensor shader remains an explicit dispatch failure. General tensor emulation remains.

## Supported input contract

Selection currently recognizes Fire Emblem: Fortune's Weave's captured prepass and output
shader, under its exact program ID. Temporal mode requires matching B10G11R11 input/output,
R16G16 motion, D32 depth and a fresh jitter sample. Spatial mode requires only the matching
color/output and fresh jitter. The upstream temporal contract uses reversed depth,
normalized motion toward the previous frame, HDR color and jitter in render pixels.
Resolution scaling scales the render extent and jitter together. History resets on size
changes, first use and evaluation gaps longer than 250 ms. An explicit game camera-cut
reset is not yet extracted.

The v131072 capture also uses prepass `5e8d8dc58bed8ba4`, alongside the original
`4cbf6eff32525aec`, with final pass `14b4222bbe23abee`. A sampled frame and bounded
contract trace confirmed 1280x720 color, motion and depth and 2560x1440 output;
prepass color at CB0+0x278 matches final-pass color at CB0+0x328. Its shader adds
motion times the output dimensions to the current pixel before sampling previous
history, confirming normalized previous-minus-current motion. CB0+0x1a0/+0x1a4
motion biases are zero and CB0+0x1b0/+0x1b4 supplies render-pixel jitter. These are
exact title and shader gates; unrecognized programs retain native execution.

Fast Fusion's shaders still execute natively. Its captured shader identities are known,
but motion scale/direction, jitter units, depth convention, reset and resource roles have
not been proven for this LLE path. The RGBA16F menu/DLAA path also requires alpha handling
that the Fire Emblem contract does not provide. Turning on NVIDIA DLSS does not override
these safety checks and is not evidence of a Fast Fusion visual fix.

For a diagnostic run, `EDEN_NX2_DLSS_CONTRACT_TRACE=1` logs at most 24 matching Fast Fusion
dispatches per compute engine. `EDEN_NX2_DLSS_CONTRACT_SKIP=N` skips the first N matching
dispatches, capped at one million. Logs include exact shader identity, QMD launch dimensions,
bounded constant-buffer words and raw texture descriptors. They contain guest addresses
and should remain private. No shaders are skipped or game files changed by this capture.

## Star Fox

Star Fox (`0400E88025B1A000`, tested with update v131072) uses its own scene motion,
depth and jitter for NVIDIA DLSS on Vulkan. Enable **Use NVIDIA DLSS for supported games**,
with resolution scale **1x** and the NVIDIA runtime installed.
**Native** runs DLAA at the captured 1920x1080 render size. Other quality settings request
3840x2160 output for that input size; **Automatic** selects the supported mode for the
actual 2:1 ratio. The quality setting does not change the game's internal render resolution.
Neural rendering uses the same existing controls as the other NVIDIA paths.

The title and native prepass/output hashes gate extraction. The captured contract is sRGB
RGBA8 scene color, RG16F unjittered motion, reversed D32_FLOAT_S8_UINT depth, pixel jitter
at CB0+0x1b0, and a history-reset flag at CB0+0x1d0. Motion converts to previous-frame pixel
offsets with scale `(-width/2, +height/2)`. Sampling the input decodes sRGB into an RGBA16F
linear image. DLSS uses HDR auto exposure; the guest's exposure textures are not passed to
NGX. Non-finite jitter, unexpected formats, subresources, aliasing and rescaled color are
rejected. History resets on first use, resize, guest reset, enable changes, skipped frames
and evaluation gaps longer than 250 ms.

All native network dispatches still execute, including writes to secondary history images.
The spatial repair is recorded first. DLSS then replaces RGB in the native RGBA16F output
while preserving its alpha; asynchronous evaluation failure leaves this frame's spatial
result intact. In 4K mode this writeback is downsampled, while the full-resolution DLSS
image is retained until native scene upscale `4027797e061f5eb4`. After that dispatch, the
validated 4K scene target receives one sRGB encoding through a UNORM storage view. The
game draws its HUD and subsequent color processing normally. Missing or stale temporal
results retain the guest upscale. This path does not use Ocarina's HUD-difference composition.

Flight training bypasses that native DLSS chain. Its separate entry point tracks the
captured scene draw and final encoded RGBA16F copy, with matching same-frame GPU resources
and validated constant buffers. It reads the actual raster jitter (the captured training
sequence alternates quarter-pixel offsets), converts unjittered RG16_SNORM motion to RG16F,
and decodes the final scene before DLSS. It writes the 4K scene target before the HUD;
Native quality reconstructs 1080p and scales that result to the target. Switching between
the cinematic and gameplay input paths resets history. Unexpected shader variants, camera
layouts, resource changes, or unreadable constants retain the game's own scene upscale.

Disabling NVIDIA DLSS, missing runtime support, or an unsupported contract retains Star
Fox's existing spatial repair or native gameplay upscale. Star Fox does not opt into the
Fire Emblem FSR path.
`ngx_smoke` exercises both native AA and super resolution with sRGB input, D32S8 depth,
alpha preservation, resets/resizes, stale-frame rejection, output encoding, gameplay's
encoded float color and SNORM motion, mode transitions, and injected NGX evaluation failure. Captured hashes and dimensions
deliberately limit support to the verified game path; future updates must be checked again.

## Bananza and Pokopia

Donkey Kong Bananza (`040074A01BF12000`, v458752) and Pokémon Pokopia
(`0400A3A01EE4C000`, v655360) use the NVIDIA checkbox with resolution scale **1x**.
Their adapters reconstruct camera motion from the actual scene depth and successive camera
matrices, then replace the recognized scene result before the game draws its HUD.
They do not supply separate character or object motion vectors, and the observed paths have
no raster jitter. Moving objects and disocclusions therefore still need visual scrutiny.

Bananza uses linear B10G11R11 scene color and standard D32 depth. It follows the exact
camera, depth-reader, tonemap, SMAA and native scaler chain. At full resolution it runs
DLAA at 2880x1620; at lower internal resolutions it reconstructs the same output size.
The active viewport can be narrower than the backing image (for example, 2095 active
pixels in a 2096-wide allocation). Matching guest addresses alone do not establish that
two host images hold the same frame: registered aliases must also have a complete copy
covering the active crop and equal modification ticks, with no pending CPU modification
or remapping. Unexpected resources retain native rendering.

Pokopia uses its exact scene, lighting, tonemap and final FXAA chain. Its camera has
reversed D24 depth and linear B10G11R11 color. The verified path is 1920x1080 DLAA;
the quality setting does not change the game's output resolution.

Both adapters copy color and depth into owned images, reconstruct previous-minus-current
pixel motion, and retain native execution. Writeback happens only after a successful DLSS
evaluation. First use, toggles, frame gaps, output-size changes and detected camera cuts reset
history. These title and shader gates deliberately leave unfamiliar paths untouched.

Dynamic input sizes opt into the NGX runtime's advertised inclusive size ranges. The backend
reuses a feature within its range, retaining the output and Neural Rendering resources when
only the input crop changes. L/M presets reserve the supported maximum input size for these
dynamic adapters. Static adapters retain their actual input allocation policy. Neural
Rendering samples motion only inside the active crop, including when the allocation is larger.

The final Bananza run used Automatic quality, preset M, Natural Neural Rendering and the
normal device memory budget. It completed 3,527 DLSS evaluations and 3,527 Neural Rendering
submissions without runtime errors. Two startup features reserved 2880x1620; the second
served active crops from 1440x810 through 2095x1179 without repeated recreation. A clean
eight-second interval after walk/jump/camera movement averaged 43.63 FPS with 33.97 ms p99.
Selected in-motion frames showed no obvious long silhouette trails, but this bounded check
does not establish correct independent object motion or a performance gain.

Natural Neural Rendering is supported but can substantially reduce frame rate; compare it
with DLSS alone in the same scene. Bananza's existing lower-right black rectangle was also
observed with DLSS disabled and is not repaired by this integration.

## Splatoon Raiders

Splatoon Raiders (`0400380020056000`, v65536) uses the same checkbox at resolution
scale **1x**. The verified island path reconstructs 2560x1440 scene color to the
game's 3840x2160 output. Its boat path uses 2560x1440 DLAA. Other input sizes and
unrecognized passes retain native rendering; the quality setting does not change
the game's internal resolution.

The adapter matches the camera, tonemap, SMAA and native scaler resources within
one frame and GPU address space. It uses linear RGB10A2 color, standard D32 depth
and reconstructed camera motion, with no observed projection jitter or separate
object motion. The island path retains the linear tonemap result rather than
feeding the gamma-encoded SMAA result to DLSS. Native passes execute first, and
successful DLSS writeback preserves the target's two-bit alpha and subsequent HUD.
Toggles, skipped frames and detected camera cuts reset history.

Automatic quality with preset M, Natural Neural Rendering and live disable/re-enable
were exercised in island gameplay. These bounded checks do not establish whole-game
visual correctness or fix the separately recorded native lighting and loading reports.

## Ocarina of Time

The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) has no DLSS of its own. Its
end-of-frame upscale pass can be replaced with FSR 3.1 on Linux or Windows, on any supported
Vulkan GPU, or NVIDIA DLSS on supported NVIDIA GPUs on either platform. The **Use AMD FSR 3.1 upscaling in Ocarina of Time**
setting (`ocarina_fsr`) is off by default. NVIDIA DLSS takes precedence when its setting is
enabled and the runtime is available; FSR runs only when its own setting is on, so without it
the game keeps its own pass whenever DLSS is off, unavailable or fails to start.
`EDEN_NX2_DLSS_REPLACEMENT=fsr` explicitly enables FSR; `native` disables both replacements;
`dlss` asks for DLSS as its setting does.

- Max Resolution add-on off: the 3/4-scale pass (`e40f5efb5480e692`; `ef542df87cacfe9e` on the
  Linux RADV build, same contract) is replaced and FSR/DLSS
  writes the game's output size (1080p to 1440p docked, 810p to 1080p handheld).
- Max Resolution add-on on, docked: the full-size pass (`6bd913e1f34e83eb`) at 2560x1440 is
  replaced by a 3840x2160 temporal image. The game still finishes its 1440p frame. At presentation,
  the HUD is taken from the difference between that frame and a 1440p copy of the temporal scene,
  and composited over the 4K scene. The 4K image skips the presentation's post-processing and
  upscale filters.
- Max Resolution add-on on, handheld: the scene renders at 1920x1080 and the same output pass
  runs at 1:1 (`c033c3ddc9f0897b`, 1920x1080 to 1920x1080); the game's final pass then scales to
  its 2560x1440 output. FSR replaces it as native anti-aliasing (no upscale). DLSS leaves the
  game's pass there (DLAA through it is untested).
- The game has no motion vectors or jitter. Motion comes from the scene depth reprojected
  through this and the previous frame's camera (vertex constant buffer 3). Link's recognized
  skinned draw is replayed with both poses where his depth matches. Without his pose the frame
  keeps the camera motion. The input is tonemapped: both backends run in LDR without auto exposure.
- FSR's LDR profile uses unit exposure/pre-exposure (no auto exposure), finite standard
  device depth (near 0.1, far 25000), and vertical FOV recovered from the view-projection.
  Its shaders use `HDR_COLOR_INPUT=0` and `INVERTED_DEPTH=0`. Camera reconstruction alone
  converts the stored [0,1] depth to GL [-1,1] clip coordinates. Motion is previous minus
  current in render pixels, with screen-down Y, so FSR's scale is `(1,1)` before normalization.
- Jitter (FSR only, on by default): the game has no subpixel sample sequence, so the host adds
  one. While FSR replaced the previous frame's upscale, every draw that depth-tests against the
  scene depth at the scene size has its viewport offset by a Halton (2,3) sub-pixel amount
  (AMD's phase count, 8 x (output / render width)^2: 18 for 1440p -> 4K, 8 for native AA), and
  FSR gets the same offset as the frame's jitter. Shadow maps, half-size passes, full-screen
  passes and frames the game upscales itself are not offset. A full-screen pass with depth
  writes on but the test off writes no depth (Vulkan disables writes with the test), so the
  depth-write flag alone doesn't count. DLSS gets no jitter.
- Motion with jitter: FSR is told the motion vectors carry no jitter (no jitter-cancellation
  flag). Camera motion comes from the unjittered matrices, reconstructed at each depth sample's
  unjittered position (the pixel centre minus the jitter). The replay of Link's skinned draw is
  offset by the same jitter, so its scene-depth check compares the same samples the game drew,
  and its motion is taken from the unjittered position. Without that offset the jitter made the
  check fail on most of the fragments it otherwise accepts.
- Texture LOD bias (FSR only): when the session's upscaler is FSR, mipmapped samplers get
  AMD's recommended bias log2(render / output) - 1 = -1.58 in docked mode (none in handheld).
  Samplers pick the biased or unbiased variant at each bind, so a console mode switch takes
  effect in-session. Images the GPU wrote (render targets: bloom, blur, reflections, fog) keep
  the game's own bias.
- FSR 4 (Windows, AMD's driver, off by default): with "Use AMD FSR 4 in place of FSR 3.1" on,
  the same inputs, jitter and RCAS setting go to AMD's FSR 4 through AMD's FidelityFX runtime
  (FidelityFX SDK 2.4.0, FSR Upscaling 4.1.2). Nothing of AMD's ships with Lichii: the setting's
  panel downloads `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` from
  the SDK's GitHub repository at the release commit into the user folder's `fidelityfx` folder,
  after showing AMD's licence. Each file must match its pinned size and SHA-256
  (`vulkan_common/ffx_files.h`) and carry AMD's Authenticode signature, checked after the
  download and again before loading. AMD's runtime picks the model (FP8 on RDNA 4, INT8 on RDNA 3
  and RDNA 2); the setting is only shown for those GPUs. `VK_EXT_shader_float8` and the
  `shaderFloat16` device feature are enabled for it only while the setting is on. FSR 3.1
  stays created and takes over if the files are missing or don't check out, AMD's runtime offers
  no FSR 4 for the GPU, the context can't be created or a dispatch fails. The log says which, and
  shows `Ocarina FSR 4 evaluated` while it runs.
- #644 (`82494c4820`, `74a2d20c90`, `4352c06bbf`) supplies the effects capture, used by FSR only
  (DLSS gets no bias mask and runs as before). FSR uses its R16F current/history bias as both
  reactive and transparency/composition masks; missing capture uses zero masks, not stale
  history. The DLSS preset changes were not imported.
- RCAS uses `fsr_sharpening_slider` with the existing inverted 0–200 mapping (stops × 100;
  stored 200 disables sharpening). HUD handling
  and the Max Resolution 1440p-to-4K presentation composition are shared with #606.
- FXAA and SMAA are skipped on frames either temporal backend drew.
- Any unexpected input, a busy frame slot, a rescaled image (resolution scale other than 1x) or
  a failed evaluation leaves the game's pass to run. The log names the reason as
  `Ocarina temporal: the game's upscale draws frame N (reason)`.
  `EDEN_NX2_DLSS_CONTRACT_TRACE=1` logs every FSR evaluation and scheduler writeback;
  otherwise periodic evaluation/writeback messages avoid a per-frame logging cost.

Shader hashes, buffer offsets and Link's mesh shape are taken from the game's current build.
A game update that changes them disables the replacement and leaves the game's pass in place.
The "Ocarina of Time anti-aliasing" option (No sharpening, docs/nx2/oot-network-bypass.md)
bypasses the same network on other GPUs. It does nothing while this replacement is enabled:
temporal upscaling takes precedence because it replaces the same output pass.

## Yoshi and the Mysterious Book

The retail v131072 (display version 2.0.0) adapter targets the captured 960×540 gameplay scene
reconstructed to 1920×1080. It recognizes native temporal program `85237f4449a5ebe7` (translated
`7a6e67d9226d0b53`) and its motion prepass `01fae3e0cd6781c3` (translated `505d41b4f1088151`).
The title gate is `0400245021D2C000`; the demo and other rendering variants are not enabled by
these hashes alone.

The game's prepass already combines encoded object motion with its native camera reprojection,
including the engine's world-origin handling and reverse-depth edge selection. Its RG16F result
stores previous-minus-current motion in output pixels. The adapter reproduces the temporal
consumer's nearest sample at `(2x+1, 2y+1)` and halves that displacement to obtain input-pixel
motion. It supplies the captured projection jitter in right/down input-pixel coordinates,
cross-checking the projection against the prepass and temporal constants. The jitter-free motion
field is not reconstructed from an assumed camera matrix.

The complete native temporal dispatch still runs, including the packed scalar-history shadow
copy-back and all four native outputs. After a successful DLSS evaluation, only the final
1920×1080 RGBA16F HDR color is replaced; its native alpha is retained. The game's history
buffers, neural state, exposure, bloom, LUT tonemapping, and later HUD remain on their native
paths. This is reconstruction from the original low-resolution HDR scene, before native
tonemapping.

The adapter requires exact native/translated identities, the captured constants and image
shapes, same-frame/channel/mapping motion and depth links, compatible mapped backing images and
host formats, and supported storage views. Other crops, book/menu chains, invalidated resources,
disabled DLSS, or a failed evaluation retain native rendering. Frame gaps, toggles, camera
discontinuities and changed projection parameters reset the DLSS history.

The moving-gameplay native and RenderDoc captures establish the contract, including recorded
coherent constant-buffer writes that match the replayed camera block. Hardware tests cover HDR
alpha/fallback and the production motion resampling pass. An uncaptured Linux run with the
normal allocator policy validated opening Wildwoods gameplay, actor and camera movement,
Automatic/M SR, live Off/On and Natural neural rendering. DLSS evaluations stopped over 724
presented frames while disabled, resumed when enabled, and reached 13,163 successful evaluations
with 2,986 submitted Natural frames. Short stationary samples measured about 60 FPS; the
movement recording also contains shader-build slowdown. These checks cover the observed 960×540
→ 1920×1080 gameplay variant, not all resolutions or full-game coverage.

## Kirby Air Riders — candidate validation incomplete

The library build leaves this candidate disabled and retains native rendering for Kirby.
The investigation below describes separately tested candidate builds; it does not establish
working Kirby DLSS support in the installed library build.

Kirby Air Riders (`040077201BE50000`, v458752 / 1.1.1) has a title-gated candidate
for its verified camera, tonemap, SMAA and native scaler chain. It uses reverse D24
depth, linear scene color and camera-only motion, preserving the observed zero jitter.
Independent rider and vehicle motion is not supplied. Native passes execute first;
failed input checks or DLSS evaluation retain the native result.

For the captured 854x480 scene, NVIDIA advertises an exact 853x480 UltraPerformance
input for the game's 2560x1440 output. The candidate resamples the complete native
image to 853x480 using pixel-center mapping, matching depth and destination-pixel
motion. It does not discard a column or relax NVIDIA's size limits. The native scaler
decodes the gamma-encoded SMAA result before storing linear RGB10A2; the candidate
therefore writes linear color and preserves native alpha and the later HUD compositor.

Hardware tests validate odd-width color/depth/motion mapping and unsupported-size
recovery. A copied-profile Automatic/M run with a 4 GiB cache cap reached 251 actual
DLSS evaluations by its last saved status, including race frames, but it had frequent
history resets and later reported Vulkan device loss. Temporal quality, stable moving
history, live toggles, Neural Rendering and normal-allocation behavior are not yet
validated; this is not a completed integration.

A fresh uncaptured run of the identical executable with only DLSS disabled reproduced
missing menu/character/race overlays and native split-front MME instruction-limit errors.
It reached a moving race before stalling without a device-loss report. These establish
native UI and stability problems, but do not establish the cause of the separate
DLSS-enabled device loss. Disabling split execution alone still reproduced MME failures;
the interpreter/memo path restored the overlays initially but also hit an instruction limit
and later produced a black scene. Neither diagnostic policy is an accepted workaround.

Bounded reset diagnostics identified consecutive-frame focal animation as a separate cause
of unnecessary resets. Only Kirby opts into retaining history when finite positive focal
scales change together at constant aspect, with every other projection coefficient unchanged
within the existing tolerance. Reprojection uses the full previous/current matrices; frame
and time gaps, position/rotation cuts, clip changes, explicit resets and policy transitions
remain active. Sixteen actual NGX hardware cases validate analytic per-pixel zoom motion and
these reset safeguards, including unchanged default behavior. The supported High GPU-accuracy comparison also reproduced native MME failures and a missing
race HUD. A bounded live DLSS interval reached 268 evaluations and retained focal history
(the projection-reset count stayed at its single initialization reset through 240 queued
frames), but its scene became cyan/overbright. Live Off stopped evaluations; the scene changed
before the comparison screenshot, so the color failure is not causally isolated. The native
failure captures contain extremely large instance-loop counts and remain under investigation.
Do not tune exposure or gamma against that corrupted baseline. Kirby remains blocked on native
correctness and actual moving-image validation; High is not an accepted workaround. A subsequent
same-binary High native-only run with `EDEN_NX2_FRAMES_AHEAD=0` also reproduced the
MME failures and lost the race HUD, despite no logged four-second fence timeout and
zero frames-ahead counts in all completed reporting intervals. No scheduling workaround
has been adopted.

## Mario Kart World (0400C3F00006E000, v917504 / 1.8.0)

The adapter reconstructs scene camera motion from the captured standard D24 depth, asymmetric
projection and rigid camera matrices. It reverses the current world-curvature deformation,
applies the previous deformation, and projects into the previous frame. There are no independent
object motion vectors for karts, characters or skinned actors; trails remain a limitation to
assess across more gameplay.

Exact camera, ColorGrading and HDRCompose shader identities bind the adapter to the captured
pre-HUD scene chain. Native recordings establish full-image UV mapping at 1664×932, 1680×940 and
1704×956, and the normal-policy live run also establishes 2560×1440. Actual NGX evaluations
covered 2048×1152 and 2560×1440 to fixed 2560×1440. Automatic/M created a Native feature and
then a dynamic Quality feature at startup; the latter was reused without further feature
creation throughout driving and live Off/On. Full-size input later reused Quality and should not
be described as a newly created DLAA feature.

The native pass runs first. Successful NGX evaluation gates the replacement writeback, which
applies the original captured exposure and component-wise HDR shoulder before subsequent HUD
rendering. Whole-image formats, consumed component swizzles, extents, full viewport/scissor,
exact fullscreen vertex/index bytes, camera constants, same frame/channel/map serial and
image/address identity must all match. Changes outside the contract retain native rendering.
History resets on enable, frame/time gaps, camera/projection cuts and allocation growth;
ordinary shrink within capacity retains history. Captured projection offsets are preserved, with
zero raster jitter for the supported contract.

Live DLSS-only driving succeeded, including a recorded motion sequence and native Off/On
fallback. The additional captured Stage/ColorGrading variant sustains NGX and Natural through
sunset/night driving. Corrected daytime Natural was also validated with captured exposure 0.25,
reference white 4 and normal scene colors. Sunset exposure 1 uses reference white 1. These
values normalize Natural's display proxy before the original native output transfer; they do not
change DLSS exposure or the scene supplied to NGX.

Bowser's captured luminance-dependent exposure uses the same known shader chain. An optional
native-only NR path now applies `T(Y)=base*Y+extra*min(Y,cap)` before inference and the exact
piecewise inverse after neural luminance changes. It preserves the vendor 128-byte parameter ABI
and default shader binaries. Parameter changes conservatively reset NR proxy history, counted by
`exposure_history_resets`; continually adapting exposure may therefore reduce temporal reuse.
Variable-exposure hardware cases and a bounded actual Bowser run pass. After a successful
native-Off map warp, live DLSS/Natural enablement produced 392 additional successful NGX/NR
frames during the recorded interval. Accepted base/extra tuples included 0.410960823/0.699545741
and 0.393825829/0.583484769 with cap 0.150000006. All 392 NR submissions also reset exposure
history, substantially limiting NR temporal reuse during adaptation. The run used an explicit
4 GiB cache cap and is not a normal-policy performance benchmark.

An initial Bowser interval retained native rendering while counters held; movement resumed
replacement. Its rejected tuple was not captured, so the coverage gap remains unclassified.
Earlier capped map-warp and Free Roam-entry stalls also remain unclassified. The successful
native-Off warp does not by itself establish their cause.

During a daytime run with the normal allocator policy, re-enabling Natural after Off failed to
allocate GPU memory. GPU 0 had 77 MiB free. NR reported the allocation error and DLSS continued;
this is an explicit optional-NR re-enable limitation, not a successful restore.

Hardware regressions cover prepared motion, original transfer including nonzero-extra
arithmetic, invalid inputs and failed-evaluation fallback. RenderDoc recorded state/initial
buffers were inspected; GPU replay pixels were unavailable because replay allocation failed with
out-of-memory.

The user's frame-pacing report remains open in KNOWN-ISSUES.md. These DLSS results are not a
pacing fix or a broad performance benchmark.

## Orbitals Friends Pass

This adapter is enabled only with Linux native NGX. Windows retains native rendering because
its current Streamline backend uses a shared viewport identity; independent per-player
Streamline resources and frame tokens require separate Windows implementation and validation.

Orbitals Global Friends Pass (`040004F027560000`, v327680) supports DLAA in
its captured local two-player trial. Each camera renders a 960x1080 view inside a
shared 1920x1080 attachment. Separate DLAA instances reconstruct each view, with
independent camera history and bounded input/output regions. The full-game program
ID and other layouts are not enabled by this contract.

The adapter recognizes the exact tonemap and FXAA passes and validates their camera
constants, shared depth resource, viewport, fullscreen geometry and sampled-image
chain. It retains the native tonemap and reverses only its final scaled sRGB transfer
around DLSS. Reverse D32 depth supplies camera reprojection; no independent object
motion or raster jitter has been observed. The native FXAA pass executes first, and
only a successful evaluation replaces RGB in the corresponding half of the scene.
Alpha and the other player's region are preserved. Toggles, frame gaps, camera cuts
and region changes reset the affected history.

The region shaders passed hardware tests covering offsets, transfer round trips,
bounds, alpha, independent histories and failure fallback. Actual trial gameplay
completed 14,872 DLSS evaluations and 5,738 Natural submissions, with separate
movement checks for both players. Live disable held the DLSS counter while 167
frames presented; re-enabling resumed both views. A Natural movement interval
submitted 900 evaluations and 900 Neural Rendering passes over 450 presented frames.

Short stationary samples measured 30.00 FPS with DLSS alone and 26.50 FPS with
Natural under the normal allocation policy while other GPU services remained active.
Natural also changes tone and reduces saturation in bright clothing. These bounded
checks do not establish whole-trial performance or correct independent object motion.

## Settings and comparison runs

"Use NVIDIA DLSS for supported games" defaults to off. On Linux, it can be changed
during gameplay with Apply, including when the session started with DLSS disabled. Install
the NVIDIA runtime files before launching: NGX and its Vulkan device features are prepared
at startup, while DLSS evaluation follows the live setting. Ocarina changes backend between
frames, finishes outstanding work before releasing its resources, and resets temporal history.
On Windows, the checkbox remains a startup setting and disabling it skips runtime loading.
An explicitly selected temporal replacement uses FSR 3.1 when NVIDIA DLSS is disabled.
Missing runtime files and unsupported GPUs fall back to FSR. Recording is
asynchronous. A failed DLSS recording clears that frame's output, restores input layouts,
and makes later evaluations fall back; it does not wait for the scheduler worker.

`EDEN_NX2_DLSS_REPLACEMENT=fsr` selects temporal replacement and forces FSR for comparison.
`dlss` selects temporal replacement with NVIDIA preferred when enabled and available.
`native` preserves the guest evaluation. With no override, Fire Emblem follows the NVIDIA
DLSS setting, using temporal replacement when enabled and spatial de-jitter when disabled.
Switching to spatial repair invalidates temporal history before the next temporal evaluation.
The old `EDEN_NX2_DLSS_STANDIN` variable is not read. The internal `standin` backend override
does not select Fire Emblem's intercepted dispatch. The RCAS sharpness setting controls FSR
and fallback sharpening. NVIDIA model-selection overrides
from the upstream implementation remain available for diagnostic comparisons.

## Build and distribution

FSR needs no external runtime. Keep `externals/FidelityFX-SDK/LICENSE.txt` and its attribution
with distributions. The vendored files are unmodified AMD MIT sources from FidelityFX SDK
v1.1.4, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

For the optional Windows NVIDIA path, obtain only the MIT Streamline headers and license
from NVIDIA-RTX/Streamline tag `v2.14.16`, commit
`625db07d4bca5abc67d781c42af7e7fb0761f1b6`. The SDK directory must contain `include/sl.h`,
`include/sl_dlss.h`, `include/sl_helpers_vk.h`, `include/sl_security.h` and their companion
headers. MSVC x86_64 builds enable `ENABLE_NVIDIA_DLSS` by default and fetch the headers;
`-DEGLANTINE_STREAMLINE_SDK=<header-root>` selects a local copy. This adds Windows
`version`, `wintrust` and `crypt32` libraries, not NVIDIA import libraries. Retain Streamline's
MIT `license.txt` in the packaged notices.

Do not bundle NVIDIA's proprietary DLLs. The Graphics panel obtains missing files from
NVIDIA's Streamline release only after the user reviews and accepts the license texts.
The runtime searches `EDEN_STREAMLINE_DIR`, the user `streamline` directory, then the
executable's `streamline` directory. It verifies the interposer signature and supports the
NVIDIA app's signed DLSS model. The UI and Windows loading path require Windows validation.

## Provenance

The Linux NGX path includes thanosipsis's synchronization workaround from
[`500c6cde069c`](https://github.com/thanosipsis/Lichii-linux-dlss/commit/500c6cde069c7daa18ab762eb7986a14adfc3cad).
It extends matching NGX image barriers to cover transfer writes, addressing internal image-clear
hazards observed with DLSS 310.9.16. Only barriers obtained through NGX's device-procedure callback
are wrapped. This compatibility workaround has been validated on NVIDIA driver 620.70.06;
other driver/runtime combinations need validation. The separate early-allocation patch is not included.

This backport is AI-assisted and requires human review and testing before distribution.
It carries upstream work by Duck912 from Eglantine commits `08dbcc7e3b3d`, `11cebf56014a`
and PR #434 through `f9d0c802e8e4`, plus Jonas's warning fix `537c1f4e173a`. The empty-extension requirement
fix `1889602c7a0d` is a separately included follow-up. Existing SPDX notices and AMD/NVIDIA
license text are retained. The upstream nx2 merge also carries newer asynchronous recording
and fallback changes while retaining the fork's signature checks and input validation.
