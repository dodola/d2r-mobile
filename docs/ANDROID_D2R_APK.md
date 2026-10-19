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
