# Qualcomm system-driver startup failure

On TNA-AN00 / SM7325 / Adreno 642L / Android 14, the build-91738 startup
aborted before the first frame when creating the compute pipeline for
`098a126f92bf533a`. The actual driver was Qualcomm proprietary (`driverID=8`),
build `923a446bf8 / I23d7d7df15`, compiler `EV031.35.01.12`. It reports compute
subgroup size 64 and subgroup arithmetic support.

## Reproduction and cause boundary

`fluid_pipeline_probe.c` reproduces the failure without running the game or
opening application data. It loads the actual fluid SPIR-V and uses its four
descriptor bindings. Original translation, warp32 translation, storage-index
compensation on/off, and ordinary spirv-opt optimization all returned `-13`
from `vkCreateComputePipelines`. The driver logged `Failed to link shaders`.
Both original and transformed inputs pass `spirv-val --target-env vulkan1.1`.

Controlled reduction isolated the failure to `OpGroupNonUniformBallot` in the
divergent compute shader. Removing shuffle, barriers, image writes or
NoContraction decorations did not fix pipeline creation. Replacing ballot
results with constants did, but that changes the calculation and is only a
diagnostic negative control. This evidence identifies the failing compiler
input, not the closed-source compiler's internal failure. In particular, no
UGPR-spill diagnosis is claimed for this device.

## Runtime correction

`nvn_vk_ballot.h` replaces each compute ballot with a four-component unsigned
bitwise-OR subgroup reduction. Each active invocation contributes its own lane
bit when its predicate is true, otherwise zero. Reduction at the original
instruction therefore returns the same active predicate mask, including under
divergent branches. The four components preserve up to 128 lanes. This does not
assume a full active mask, insert workgroup barriers, alter floating-point
operations, or remove a game dispatch.

The backend enables the pass for the Qualcomm proprietary driver only when
compute subgroup arithmetic is advertised. Other drivers retain their original
modules. The pass runs in memory after verified cache loading, before storage
compensation and Vulkan shader-module creation. The original resource image,
compressed shader files, metadata and offline-pack checksums remain unchanged.

## Verification, 2026-10-16

- Actual failing shader: original pipeline `-13`; the production C lowering
  creates the same system-driver pipeline successfully, including storage
  compensation. Its output also passes SPIR-V validation.
- `verify_system_ballot.py`: three GPU readback cases, 12 divergent ballot sites
  each, two workgroups, local sizes 32 and 128, partial/full subgroups, false and
  true predicates, output sentinels, and an injected lane input. All CPU/GPU
  comparisons have zero mismatches on driver 8, subgroup size 64.
- The same C pass was applied to all 26 real compute-cache modules; every result
  passes SPIR-V validation. All 22 ballot-free modules remain byte-identical.
- A main-built native library was packaged with the previously verified
  installer shell. Installed APK SHA-256 matched the build report. UID, resource
  metadata and the save archive were unchanged by the in-place update.
- Two cold launches with the saved choice `system` and no private-driver marker
  reached real frames (376 and 379 at the 30-second samples). Logs identify driver
  8 and the eight fluid ballot replacements. The first run also entered the
  actual gameplay scene and continued presenting frames.

The system-driver path still displays a deformed character model. Its cause is
not established here, and this change does not claim to fix that rendering
problem, all GPUs, all levels or long-session stability. Startup success and
remaining image correctness are separate observations.

Local evidence is retained outside Git under
`analysis/d2r-mobile-port/TNA_AN00/system-driver-root-cause/`: baseline and
reduction probe results, GPU readbacks, transformed SPIR-V, build manifests,
deployment data-preservation checks, two cold-start logs/screenshots, and the
in-game screenshot. Game resources and binary artifacts are not committed.

## Run the regression

Use the pinned Android NDK 27.2.12479018 and an explicitly selected device:

```powershell
python tools/android/verify_system_ballot.py --serial DEVICE_SERIAL --output D:/AndroidSwitch/analysis/ballot-check --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-cache
```

The probe uses a dedicated `/data/local/tmp/d2r-system-ballot-test` directory;
it never installs an application, modifies a system driver, or touches saves.
`tools/android/patch_native_apk.py` optionally creates an in-place test APK from
a native build manifest and a separately verified shell APK, preserving all
non-native payloads and checking signatures and 16 KB alignment.
