# D2R Android APK packaging

`dm/tools/android_d2r_apk.py` packages prepared D2R 3.3.94012 inputs. It compiles
all Java sources, creates the APK, aligns native libraries for 16 KiB pages,
debug-signs it, verifies the signature/alignment and compares every packaged
module, asset and native library with the validated input bytes.

It does not extract games or generate patched modules, RomFS, overlays, tables
or shader caches. Those external inputs must already exist. Deployment of the
external RomFS/overlay/shader data and original-game verification are separate.

Build the native library with `DM_ANDROID_D2R_HOST_MODULES=ON` and matching
`DM_ANDROID_VAULT_TABLE`, `DM_ANDROID_HUD_EXP_HEADER` and
`DM_ANDROID_NATIVE_ART_SOURCE` inputs. The default native build retains the
91738 baseline and does not enable these 94012 private adapters. The existing
91738 production offline installer is a separate release path; never mix its
resource package with 94012 modules.

Example in PowerShell, run from the checkout:

```powershell
python dm/tools/android_d2r_apk.py `
  --sdk "$env:ANDROID_SDK_ROOT" --java "$env:JAVA_HOME" `
  --native <build>/libd2r_mobile_android.so `
  --images <prepared-modules> --hud-assets <prepared-hud-assets> `
  --out <new-output-directory> --package org.d2r.newprobe
```

`--repo` defaults to this checkout. `--sdk` and `--java` also read
`ANDROID_SDK_ROOT`/`ANDROID_HOME` and `JAVA_HOME`. The default build tools and
platform are `35.0.0` and `android-35`; both have explicit CLI overrides.
`--keystore` defaults to the Android debug keystore with password `android`.
Release signing is separate. The output must be fresh and outside the checkout
and input trees. `--validate-only` checks prepared inputs without creating
output or invoking Java/Android tools.

Full packaging requires:

- An AArch64 shared ELF with 16 KiB compatible segments, the actual generated
  native-art functions/table, HUD/world JNI and the read-only
  `dm_android_hud_exp_table_entries=100` datum. The latter follows the same
  compile-time branch as the real HUD experience consumer.
- A `module-manifest.json` for the reviewed D2R title/build, with exactly the
  main/sdk/subsdk0 `.patched.bin`, `.fixups.tsv` and `.gates.tsv` files. Binaries
  must be nonempty; empty gate/fixup payloads are permitted and still hashed.
- `d2r-hud-assets-manifest.json` and `d2r-world-assets-manifest.json`, each with
  `passed: true` and a nonempty `files` array. Every entry contains `path`,
  `bytes` and `sha256`; PNG `dimensions`, when present, must match IHDR.
- HUD manifest `native_art` fields `manifest`, `geometry` and `binding`, each
  naming a hashed payload under `d2r_hud/`. Copy the original generated
  `native_art_manifest.json`, `native_art_v1_geometry.json` and
  `touch_layout_spec_native_binding_v5.json` without reserializing them. Their
  byte hashes must match `dm_ta_manifest_sha`, `dm_ta_geometry_sha` and
  `dm_ta_binding_sha` in the ELF. The nine skill PNG hashes and native table
  dimensions must also match that provenance.
- All world labels in Chinese/English, seven tomb symbols, the grey cross,
  mapgoal metadata and the original BlizzardGlobal font, covered by the world
  manifest. Asset files outside the two manifests are rejected.

All manifest paths must be canonical relative paths. Missing files, symlinks/
junctions, traversal, duplicate paths (also across manifests and ignoring case),
duplicate JSON keys and size/hash mismatches are rejected before output is
created. Validated bytes are retained for packaging, so a later input-file
change cannot silently change the copied payload.

The output includes `build-report.json` with native/module/asset/Java hashes,
ELF configuration identities, SDK choices, final APK hash and verification
counts; `commands.json` records tool diagnostics. Hash consistency establishes
which provided inputs were packaged, rather than authenticating an arbitrary
external manifest or establishing original-game behavior.

Validation tests:

```powershell
$env:PYTHONPATH="dm/tools"
python -m unittest discover -s dm/tools/tests -p test_android_d2r_apk.py -v
```

## Offline release build (game 94012)

Passing `--offline-manifest` switches the packager from the diagnostic test APK to the installer-launched offline
release: package `org.d2r.mobile` (coexists with `org.d2r.newprobe`), launcher `InstallerActivity`, launcher icons,
non-debuggable, release-signed. The diagnostic mode above is unchanged.

Extra inputs for the offline mode:

- `--offline-manifest`: the `offline-manifest.json` written next to `D2R-Offline-94012.d2rpack` by
  `tools/d2r-mobile-port/build_offline_package.py`. It must be format 1, `game_build` 94012, list only paths the
  on-device installer accepts (`game.romfs`, `d2r-mobile/shaders/…`), and its `game.romfs` size/SHA-256 must equal the
  `romfs` entry of the module manifest.
- `--overlay-base` / `--overlay-item-notes`: the touch-UI overlay directories. Exactly `ctrl_touch`,
  `touch_strings.tsv`, `data/.build.config`, `data/00-00000007.data` (base) plus `dm_textbox_plain.tsv` and
  `features.txt` (item-notes variant). They are packaged under `assets/overlay/` and copied by `GameActivity` to
  `files/d2r-mobile/romfs-overlay[-variants/item_notes]` on launch. Without them the native host logs
  `ANDROID_CUSTOM_OVERLAY base_unavailable=1` and runs with no touch-UI data.
- `--driver-libs`: a directory with exactly the seven bundled driver libraries (`libd2r_turnip.so` pinned to SHA-256
  `c2dc3ff0…8e9e`, `libadrenotools.so`, `libc++_shared.so` and the four hook libraries), all 16 KiB compatible.
  No validation layer is shipped.
- `--keystore-properties`: `key=value` file with `storeFile`, `alias`, `storePassword`, `keyPassword`. The passwords
  reach `apksigner` through the environment (`env:`), so they never appear in `commands.json`. An offline release is
  never debug-signed. `--expect-cert-sha256` makes the build fail if the signer differs.
- `--version-code`, `--version-name` (default `1`, `1.0.0-94012`); `--update-url` is optional and empty by default.
- `--debuggable` (diagnostics only): same identity and signature but debuggable, so `run-as` can read
  `files/d2r-mobile/diagnostics/game-startup.log`; installs over the release with `adb install -r`, keeping the
  imported game data. Never distribute it.

Typical sequence (see `docs/ANDROID_PERFORMANCE_2026-10-20_SHADER_LOOKUP.zh-CN.md` for measurement parameters):

```powershell
$env:ANDROID_SWITCH_WORKSPACE="D:/AndroidSwitch"
python tools/d2r-mobile-port/build_offline_package.py          # -> release-94012/pack/D2R-Offline-94012.d2rpack
python dm/tools/android_d2r_apk.py --native <libd2r_mobile_android.so> --images <modules> --hud-assets <hud> `
  --offline-manifest <pack>/offline-manifest.json --overlay-base <romfs_overlay> `
  --overlay-item-notes <romfs_overlay_variants/item_notes> --driver-libs <driver-libs> `
  --keystore-properties $env:USERPROFILE/.android/d2r-release.properties --expect-cert-sha256 <hex> --out <fresh-dir>
python tools/d2r-mobile-port/write_release_notes.py             # installation notes + SHA256SUMS
python tools/d2r-mobile-port/verify_offline_installer.py        # host installer checks (40)
```

`build_offline_package.py` pins the 94012 RomFS (8,973,550,584 bytes, SHA-256 `f5f3bcfb…2785`) and checks the shader
cache against its recorded `cache-checksums.sha256`. `GameActivity` no longer has a size-only RomFS fallback and skips
re-hashing the image when the installer receipt covers it. The 91738 offline chain still exists in the repository until
it is retired separately; its resource package must never be mixed with 94012 modules.

Default graphics driver of the offline release: `GraphicsDriverPolicy` resolves `auto` to the bundled Turnip driver
(`TU_DEBUG=sysmem`) on Qualcomm devices (Android 12+, `Build.SOC_MANUFACTURER == "QTI"`) and to the system driver
everywhere else. Two consecutive automatic starts that never present a frame fall back to the system driver; choosing a
driver again in the installer clears that. Measured on SM7325 and SM8750; see
`docs/ANDROID_PERFORMANCE_2026-10-20_SHADER_LOOKUP.zh-CN.md` for the table of fixed parameters.
