# Upstream Android port · 2026-10-28

中文: [UPSTREAM_ANDROID_PORT_2026-10-28.zh-CN.md](UPSTREAM_ANDROID_PORT_2026-10-28.zh-CN.md)

Comparison baseline: local main `766ef032d1a34f51517bf5e6eee8d9b26f81ac64`. Behavior is adapted from [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) main `1e0e7934c9ccdf95494aee5065d1d07ab3cc3918` (Android 2.1.0); its former android branch is deleted. The port targets the local C Vulkan backend, retaining `.vkmeta/.spv`, asset imports, warm-up manifests, application identity and release signing contracts.

## Completion audit

- [x] Shared UI: restore map preference after panels/area changes without rewriting configuration; minimap after movies; widened stash restoration; legend cleanup/eight-entry bound.
- [x] Adreno: three runtime passes, one path for demand/warm-up/workers, default on 750–799, marker/environment overrides and updated shader/cache identity.
- [x] Pipeline diagnostics: complete stable keys, driver/shader isolation, per-thread durable markers, two consecutive abandoned boots, recovery and error classification.
- [x] Drawing completeness: normal demand always waits, background warm-up remains, missing-effects degradation requires explicit opt-in.
- [x] Existing equivalents: retain and verify R8/RG8 BC4/5, 30 FPS, NaN, Android stack allocation and driver-isolated manifests.
- [x] Camera: original by default, shoulder/first-person settings, Vulkan constant copies and previous/current matrices, input/name/health-bar conversion, walls/height/culling exception.
- [x] Camera frontend: right-side drag, turn buttons, optional sensors, offscreen threats/map heading, foreground/window/menu/dialog lifecycle.
- [x] Build verification: CPU regressions, i18n/mode/layout gates, Android native linkage, complete signed APK and available native device checks.
- [x] Bilingual documentation, attribution and evidence. Full gameplay acceptance limits are below.

## Sources and adaptation

Map behavior comes from `195ae11` / `d1275c7`, stash from `985e5ec`, legends from `d93985b`, inlining/diagnostics from `2428e5b`, and camera from `dc5778d`. `dm/third_party/spirv-tools` preserves upstream source/licenses. Pure camera math retains upstream algorithms and integrates with local immutable UBO snapshots without changing guest memory. See the [camera guide](over-the-shoulder-camera.md).

The upstream quad/warp32 switches were not copied. Local Qualcomm passes use equivalent OR reduction for compute ballot and vec4 loads followed by component extraction for skin UBOs; these differ from upstream rewrites and retain existing driver/subgroup selection. Helper inlining follows local adaptation and NaN fixes in both warm/demand loads. Local BC4/5 already has one/two-byte layouts; the upstream global BC fallback and its mip-size omission were not imported.

## Controls and diagnostics

- Inline precedence: cache-directory `gfx-inline` > `DM_VK_INLINE_HELPERS` > Qualcomm Adreno 750–799 default. A leading `0` disables the marker; other values enable it. ABI 5 and NaN/inline bits enter cache/manifest identity; future rewrite changes must bump the ABI.
- Records use cache-directory `pipe-guard-<identity>.*`. Identity includes device/driver UUID/shader index/options; keys contain complete stable state. Duplicate markers count once per boot. A boot without a matching abandoned marker resets an unquarantined streak.
- Only explicit `VK_ERROR_INVALID_SHADER_NV` immediately persists quarantine. OOM/device lost/unknown errors do not. Quarantined demand fails clearly by default; `DM_VK_PIPE_DEGRADE=1` or `gfx-pipe-degrade` containing `1` explicitly permits missing draws/dispatches.
- Create `gfx-pipe-reset` to clear current-identity records next initialization; `DM_VK_PIPE_RETRY=1` temporarily retries. Driver/configuration changes isolate old records automatically.

## Actual verification

Windows NDK 27.2 / Android API 29 arm64 Release builds pass, including static C++ runtime, Java/JNI and complete touch overlays. The APK uses existing release signing and verifies 204 payloads. Generated assets, game content, signing material and APKs are excluded from Git.

| Check | Result |
|---|---|
| `run_upstream_port_checks.py` | 10 groups: map, stash, camera state/math/integration/touch, guard/wait, BC8 and real optimizer; ASan/UBSan except optimizer group |
| Demand wait | Production function: graphics/compute workers delayed 30 ms still provide pipelines; synchronous fallback passes |
| BC8 | 324 cases, signed/unsigned BC4/5, dimensions 1–9, output guards |
| `run_merge_checks.py` | All pass; touch 49 cases/4474 checks, NaN 20,000 cases, dirty tracking/queues/pacing/save recovery and more |
| `run_pipeline_checks.py` | 415 checks each in normal and ASan/UBSan builds |
| i18n/modes/layout | Literal scan, 484-row generated table, mode scan and both layout gates pass |
| Honor TNA_AN00 / Adreno 642L | Real-library settings fixture: 188 checks including camera commit/discard/defaults/reload |
| Same phone Vulkan | Synthetic subgroup helper uses production inlining and real dispatch; all 32 lanes output 32 |
| Actual local shader pack | 187 helpers among 6,929 modules all inline/validate, preserving descriptor decorations |
| Device texture-view ABI | 6,201 checks through real NVN entries/native thunks |

Linux/WSL reproduction: configure with `cmake -S dm/third_party/spirv-tools -B <spirv-build>` and build `dm_spirv_opt`, then run `python3 dm/android/tests/run_upstream_port_checks.py --out <receipt-dir> --spirv-lib <spirv-build>/libdm_spirv_opt.a`. Device targets are `dm_android_inline_probe` and `dm_android_d2r_custom_probe`; the latter takes the native library path.

Machine evidence is outside the checkout at `analysis/upstream-android-port-20261028/`: `final-cpu/result.json`, `final-merge/result.json`, `final-pipeline/result.json`, `device-custom.log`, `device-inline.log`, `device-view.log`, `local-shaders.json`, `apk-build.log`. Upstream analysis/fixed baselines are at `analysis/upstream-android-review-20261028/REPORT.zh-CN.md`.

## Verification limits

Adreno 750, Mali and iOS/Metal were not accepted on hardware. Full phone gameplay, camera gameplay screenshots, all areas/combat/movies, human sensor operation and original-game ground truth were not tested. The settings fixture uses synthetic guest memory with the real library; the Vulkan probe uses a synthetic shader. Neither proves a game frame. Existing apps/saves were not overwritten, cleared or uninstalled. Camera defaults off; full visual/gameplay acceptance remains necessary before release.
