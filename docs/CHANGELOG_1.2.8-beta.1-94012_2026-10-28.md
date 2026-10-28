# D2R Mobile 1.2.8-beta.1-94012 (2026-10-28)

中文: [CHANGELOG_1.2.8-beta.1-94012_2026-10-28.zh-CN.md](CHANGELOG_1.2.8-beta.1-94012_2026-10-28.zh-CN.md)

Android beta update from 1.2.7-94012, with version code 14. It uses release package `org.d2r.mobile`, the existing release certificate and a non-debuggable build, allowing upgrades over the same package/signature. Fresh installations use the existing `D2R-Offline-94012.d2rpack`; users with imported 94012 data only need the APK update.

## Added

- Original, shoulder and first-person camera choices on the custom page. Original remains the default; apply to switch.
- Right-side pitch dragging, first-person horizontal turning, turn buttons, map heading and offscreen threat indicators. Sticks, taps, names and health bars follow the camera.
- Runtime inlining of subgroup-helper shaders defaults on Adreno 750–799, using existing offline resources after an APK update. Other devices retain their previous default policies.

## Fixes and behavior

- Restore fullscreen-map preference after panels/area changes without rewriting player configuration; restore minimap after movies.
- Fix inventory drifting right after closing the widened stash with +.
- Clean controller legends when leaving pages and bound the strip to eight entries.
- Normal demanded pipelines finish compilation before drawing, avoiding omitted objects when a cold-compilation budget expires. Background warm-up remains; first encounters with new effects can still stall.
- Failure records isolate drivers, shader configurations and complete pipeline states, with clear diagnosis/recovery. Quarantined pipelines are not silently skipped by default.

## Offline installation and compatibility

- Package `org.d2r.mobile`; `versionCode=14`; `versionName=1.2.8-beta.1-94012`; unchanged release certificate.
- Reuses the 94012 resource pack, bundled Turnip, six optional MOD packages and existing save-management features.
- Install updates over the existing app without uninstalling or reimporting matching 94012 data. For a fresh install, install the APK and select the `.d2rpack` in the launcher.
- APK `debuggable=false`. The optional in-game Debug feature is separate from Android's debuggable flag and remains available.

## Verification and beta limits

Ten new CPU regression groups pass. All 187 helper modules among 6,929 shaders in the prepared local pack inline and validate. Honor TNA_AN00 / Adreno 642L passes 188 real-library settings checks, 6,201 texture-view checks and a real Vulkan dispatch of a synthetic shader. Native builds, APK signing, 16 KiB alignment and payload verification pass; the resource pack is separately verified per file and as a whole.

The new camera has not completed acceptance on Adreno 750, Mali or full gameplay. All areas, combat, movies, sensor operation and long sessions remain to be tested. Known 1.2.7 issues with Chinese character renaming, background surface loss on some devices and incomplete Debug/MOD acceptance remain applicable. This is explicitly a beta; CPU/synthetic device probes do not prove a complete game frame.

See the [upstream port record](UPSTREAM_ANDROID_PORT_2026-10-28.md) for implementation/evidence. Delivery files `build-report.json`, `source-provenance.json`, `release-verification.json` and `SHA256SUMS.txt` record actual build and verification results.
