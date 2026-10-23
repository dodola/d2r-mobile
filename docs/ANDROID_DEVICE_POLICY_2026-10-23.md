# Android device policy and upstream fix acceptance (2026-10-23)

中文: [ANDROID_DEVICE_POLICY_2026-10-23.zh-CN.md](ANDROID_DEVICE_POLICY_2026-10-23.zh-CN.md)

Scope: isolated worktree `codex/ppg-render-slots-20261023`; this does not mean the shared integration checkout has been merged. No upstream APK was installed and TNA was not changed. Combat acceptance belongs to the user.

## Should every device be enabled?

Use common defaults for general optimizations, but retain capability checks, driver workarounds and experimental guards. Two slots are already the default for every device and driver, not just SM8750.

| Item | Actual condition | Recommendation |
|---|---|---|
| Two slots, FIFO publication, UBO and non-uniform versioning | All devices and drivers; independent disable markers and explicit slot count provide rollback | Already enabled globally; retain |
| NaN min/max/clamp semantics | Enabled for every driver; `disable-nan-minmax` rolls back with separate cache identity | Retain the semantic correction; it did not fix the system driver's local black blocks |
| Launcher automatic Turnip choice | API >=31 and `SOC_MANUFACTURER=QTI`; SM8750 excluded by the old presentation-bug list. Explicit Turnip selection always takes precedence | Other Qualcomm models already have no model allowlist. Remove the SM8750 exception only after pre-rotation combat/lifecycle acceptance; do not choose Turnip for non-Adreno GPUs such as Mali |
| `VERIFIED_TURNIP_SOCS` | SM7325 and SM8750 | Measurement records, not a Turnip allowlist |
| Turnip pre-rotation | New default for SM8750 with actual Mesa Turnip driver ID; `present-prerotate.txt=0/1` overrides it; checks transform type and swapchain color-attachment usage | Tested on this device. Explicit testing can expand to other models; do not default every driver to it without validation. Eventually use driver and window capabilities |
| Native BC upload | Automatically requested when the GPU name parses as Adreno >=800; also requires `textureCompressionBC`, per-format sampling/linear filtering/transfer and image-size support. Storage images and other exclusions still decode | Candidate for replacing the name gate with capability checks plus representative-device comparisons. Retain capability checks. TNA and other GPUs were not accepted in this round |
| Qualcomm system UBO/ballot rewrites | Actual Qualcomm proprietary driver ID; ballot also requires compute subgroup arithmetic | Retain compiler workarounds; do not blindly apply them to Turnip or Mali |
| Dynamic UBO | Turnip, explicit enable or UBO versioning enabled, with ordered bindings; can be disabled | Default UBO versioning already extends this to other drivers; not locked to one Qualcomm model |
| Mali depth-input experiment | Exact Mali-G610 MC6 vendor/device/driver/version/info, precision and extension match, plus explicit experiment marker | Disabled by default; keep the calibrated guard, do not extend to all Mali |
| Turnip `sysmem` | Normal launcher Turnip path sets it for all models, based historically on Adreno 642L. Diagnostic `private-vulkan` does not force it | Driver mode rather than a model allowlist; test separately. Ordinary Turnip results here do not accept the sysmem path |
| PSO compilation workers | Online CPUs >=6: 3; >=4: 2; otherwise 1; overridable, maximum 4 | General capability policy; retain |

Retain fallback after two automatic starts without a presented frame, subgroup checks, sampler/format limits and GPU/driver cache identity. These are not performance locks to remove.

Main code: [GraphicsDriverPolicy.java](../dm/android/src/org/d2r/binaryprobe/GraphicsDriverPolicy.java), [GraphicsDriverSettings.java](../dm/android/src/org/d2r/binaryprobe/GraphicsDriverSettings.java), [nvn_vk_slot_policy.h](../dm/gfx/nvn/nvn_vk_slot_policy.h), [nvn_vk_profile.h](../dm/gfx/nvn/nvn_vk_profile.h), [nvn_vk.c](../dm/gfx/nvn/nvn_vk.c), [nvn_vk_texture_upload.inc](../dm/gfx/nvn/nvn_vk_texture_upload.inc), [nvn_vk_pipeline.inc](../dm/gfx/nvn/nvn_vk_pipeline.inc).

## Fixes and evidence

Upstream baseline: `88b322158ae3d6100293de0470d4c43277710269`.

1. Ported NaN semantics from [3970f8b](https://github.com/jhaizhou-ops/d2r-mobile/commit/3970f8b07bcc09ae0cd97504431008b18d4efed7), adding instruction-boundary, extended-set and arity validation, failure atomicity and cache separation for the switch. Warm-up and demand loads share the fix; dumps capture the final transformed module.
2. Ported acquire-wait/layout-transition synchronization from [43a14d3](https://github.com/jhaizhou-ops/d2r-mobile/commit/43a14d3bfdfd78c4a0344687e381af5a33741664). The original `TOP_OF_PIPE` barrier did not chain to the submit's `TRANSFER` acquire wait. The pre-rotation draw path aligns both at `COLOR_ATTACHMENT_OUTPUT`.
3. Independently implemented application pre-rotation. The swapchain uses the window's `currentTransform`, with swapped width/height for 90/270 degrees. Authored full-screen-triangle shaders invert sampling coordinates, preserving crop and guest Y origin. Sampling is linear; the old blit can use cubic, so outputs are not claimed to be pixel-identical.
4. After `SUBOPTIMAL`, check orientation before recording the next present, covering 180-degree turns without a Surface size callback. Drain the GPU before rebuilding and clear framebuffer, image-view, descriptor and bridge-image caches. Update the single presentation descriptor only after the dedicated presentation slot's previous fence completes.

PPG: `AVAY025422000548`, SM8750 / Adreno830, Android17, ordinary Turnip Mesa26.2.3, two slots, fixed 1.0 scale. No automated movement or attacks. Later user observation/movement makes affected samples unsuitable for performance A/B.

| Experiment | Result |
|---|---|
| System driver, NaN off/on/off/on | Upper-right black frames: 522/961, 515/960, 402/960, 450/960; no demonstrated improvement |
| Final actual SPIR-V | All 94 dumped modules passed Vulkan1.1 validation; no old FMin/FMax/FClamp; NMin309 / NMax856 / NClamp719 |
| Upstream SHFL candidate, diagnostic override of 8 modules | 504/961 to 445/961 black frames; unresolved, override removed and excluded from defaults |
| Old Turnip full-screen flicker | Confirmed transient rotation/stretch. Static samples excluding first 120 frames: NaN on 21, NaN off 21, user-reported recording 64 |
| Acquire barrier alone | Still 44 transient transposed frames; not a flicker fix |
| First pre-rotation sample | 962 frames, zero anomalies after excluding first 120; user confirmed no further flicker |
| Reverse landscape, default enable and restored direction | Zero anomalies in each stable recording. Added missing same-size orientation rebuild; subsequent switching adds only one SUBOPTIMAL, then stops increasing |
| Final r2 cache-cleanup build | 961-frame recording: zero transient-transposition detections after excluding first 120, zero target-region black frames; sampling log contains zero errors. Separate logs verified 90→270→90-degree rebuilds, zero errors in both orientation samples, and SUBOPTIMAL stabilizing at two total occurrences |

Full-screen detection uses 384x174 RGB mean differences: >5 against both frames six positions before/after, with the outer pair differing by <6. The threshold was calibrated against actual transposition frames. The old threshold of 20 missed them, and local black-block counts cannot substitute for full-screen flicker acceptance. Raw data and old results are retained. Limited camp samples and user observation do not replace sustained combat acceptance.

## Validation and delivery

- Android native Release build, original APK signing and 16K alignment passed. Only the native library was replaced in the known debuggable package; Java/resources match byte-for-byte.
- Host merge regression passed: 48/48 cases, 4463/4463 checks, thread200 and save-recovery73. New malformed-input/idempotence/extended-set NaN tests and 20000 random cases passed, including separate ASan/UBSan checks.
- Local audit of 6929 modules passed; 5573 changed only extended opcodes. All 164 candidate transformed modules passed SPIR-V validation; the final 94 device dumps were validated separately.
- Authored presentation GLSL passed pinned NDK glslc generation, spirv-val and reproducibility checks. The i18n table passed with 406 rows; literal-scan output matches baseline; `git diff --check` passed.
- No PPG synchronization validation layer was enabled in this round. Upstream Redmi layer results are not local evidence. Apple/Metal tests were not run.
- All devices retain two slots by default. TNA final acceptance remains skipped. System-driver local black blocks remain unresolved; the user continues Turnip combat testing.

Local evidence: `D:/AndroidSwitch/analysis/upstream-android-20261023/` (review, shader audits, host checks) and `D:/AndroidSwitch/analysis/perf-runs/ppg-slots-20261023/` (recordings, logs, APKs, build receipts). Final package directory: `apk-turnip-prerotate-r2`; native SHA256: `0ae893f7bde7098ecdafe3cd8cba77898b4e0e962be5ac87b9e679b191edab4c`. The launcher's SM8750 automatic-driver exception is unchanged. This device uses Turnip through `private-vulkan`; pre-rotation needs no forced marker.
