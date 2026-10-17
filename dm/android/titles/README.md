# Android title bring-up

This branch starts Android support for **The Legend of Zelda: Echoes of Wisdom**
(`01008CF01BAAC000`). It does not yet provide a playable Zelda APK. iOS/Metal
and the existing D2R application are outside this work.

The [profile](echoes-of-wisdom.json) pins the locally inspected 1.0.0 (content
version 0) and 1.1.0 (196608) executables by SHA-256, full NSO build ID, NPDM,
and module set. **1.1.0 is the current bring-up target.** The future Android
application ID and data directory are reserved separately from D2R; an app with
that identity has not been built yet.

## Verified, 2026-10-16

- Both NSZ packages were decompressed locally using NSZ 5.0.0. All eight NCA
  content IDs matched the first 128 bits of their SHA-256 hashes. CNMT/NACP
  confirmed the content and display versions rather than relying on filenames.
- NPDM identifies an AArch64 application, a 1 MiB main-thread stack, and
  priority 44. The program modules are `main`, `subsdk0`, `subsdk1`, and `sdk`.
  The host replaces `rtld`; it must not execute the original `rtld` as a fifth
  game module.
- In 1.1.0, all nine expected startup symbols are defined and all guest imports
  have providers in those four modules. This does not establish compatibility
  of host service replacements or SDK object layouts.
- All four 1.1.0 modules passed the existing Android instruction rewriter with
  zero title-specific CUTS, OBS, or PARAMS rows. The SDK produced 101 host gates
  (58 SVC sites and 43 counter reads).
- Six synthetic integrity tests cover NPDM identity/architecture/bounds, NSO
  segment hashes and compression, output isolation, and rejection of mixed or
  incomplete content. The extraction/audit/prepare commands also ran against
  the actual local 1.1.0 files.
- The independent four-module Android startup probe builds with NDK
  27.2.12479018, API 29, and arm64-v8a. It checks the rewrite build labels and
  rejects title-specific patch rows. Only this explicit probe profile permits
  unused D2R HLE_FORCE entries and selects Zelda's SVC program ID; D2R's default
  loading policy and program ID remain unchanged.
- On **PPG-AN00, Android API 37, arm64-v8a**, all four standalone probe modes
  passed: `load`, `os-init`, `os-services`, and `sdk-startup`. This includes
  original SDK clock/TLR checks, 32 original SDK threads with 4 KiB guest
  stacks exiting cleanly, three thread/event cycles, and three SDK startup
  stages using temporary platform/filesystem fixtures.
- Android loading now handles the newer program's **DT_RELR** tables:
  448,722 + 70,917 + 3,424 + 15,002 relative pointers across the four modules.
  The decoder validates the complete stream before writes, rejects targets
  outside the writable image, and passed optimized plus ASan/UBSan host tests.
- New SDK initialization queries `AliasRegionExtraSize`; this host returns
  zero because it reserves no extra alias area. Memory-use queries now charge
  the same mapped-image footprint as the heap-limit check. Previously the
  fixed 64 MiB estimate overstated free memory, causing this game's one-shot
  heap allocation and allocator startup to fail.
- With the same rebuilt probe, the existing D2R three-module `load` and
  `sdk-startup` regression checks also passed. This checks SDK bootstrap only;
  no D2R gameplay or performance regression run was performed.

No GPU frames, input, audio, saves, or gameplay have been verified for this
title. The SDK-startup result uses empty temporary resources and does not call
`nnMain`. These results must not be reported as a playable port.

## Reproduce the program preparation

Python 3.11+ is required. Use a virtual environment with `lz4` and `capstone`;
the optional NSP extraction command additionally needs `nsz==5.0.0`. NSZ's
[official usage](https://github.com/nicoboss/nsz/blob/master/docs/usage.md)
documents decompression of NSZ to NSP. Keep the source files, keys, extracted
files, rewritten binaries, shader caches, and generated reports outside this
checkout. The tool rejects output inside the checkout and nonempty output
directories.

Run from the repository root, substituting your own absolute paths:

```powershell
# For an already decompressed update NSP. Keys are read locally, never logged.
python dm/tools/android_title.py extract `
  --nsp D:/local-game/update.nsp --keys D:/local-keys/prod.keys `
  --out D:/local-build/echoes-1.1.0-exefs

# Static inspection only; also checks all pinned source hashes before analysis.
python dm/tools/android_title.py audit `
  --exefs D:/local-build/echoes-1.1.0-exefs `
  --out D:/local-build/echoes-1.1.0-audit

# Audit, then Android rewriting. This can take several minutes.
python dm/tools/android_title.py prepare `
  --exefs D:/local-build/echoes-1.1.0-exefs `
  --out D:/local-build/echoes-1.1.0-prepared

python -m unittest discover -s dm/tools/tests -p test_android_title.py -v
```

`--version 0` selects the pinned base program instead. It has not received the
four-module rewriting validation described above. Unsupported hashes/versions
stop before rewriting. `title-audit.json` records source and product hashes,
startup symbols, imports, NVN name differences, and explicit verification
limits. Failed preparation never publishes `rewritten: true`.

## Standalone device probe

Configure `dm/android` with the Android NDK CMake toolchain, `ANDROID_ABI=arm64-v8a`
and `ANDROID_PLATFORM=android-29`, then build target `dm_android_host_probe` in a
separate build directory. No D2R resource-sample header or APK packaging is
required for that target. The included runner checks all local/pushed hashes,
uses an independent temporary directory and fresh result files, and stops at
the first failing stage:

```powershell
python dm/tools/android_title_device.py --adb C:/tools/platform-tools/adb.exe `
  --serial AVAY025422000548 `
  --probe D:/local-build/android/dm_android_host_probe `
  --prepared D:/local-build/echoes-1.1.0-prepared `
  --out D:/local-build/echoes-device-results
```

Replace the serial and paths with the connected device and local products.
`--mode load` selects only the first stage. For a manual run, after checking
every pushed product's SHA-256 against `title-audit.json`, the initial command is:

```sh
/data/local/tmp/echoes/dm_android_host_probe \
  --title-profile echoes-of-wisdom-1.1.0 \
  /data/local/tmp/echoes/patched /data/local/tmp/echoes/load.json load
```

Proceed to `os-init`, `os-services`, then `sdk-startup` separately, inspecting
each result/log before advancing. `sdk-startup` takes a final absolute fixture
directory argument; this exercises service initialization with empty temporary
fixtures, not Zelda resources. All modes retain `full_game_verified: false`.
The initial `load` mode calls the original SDK clock/current-thread functions;
it is not a static loader-only check. This title's 1.1.0 getters were inspected
offline: current thread reads TLR+0x1f8 and tick reads CNTPCT, consistent with the
probe's existing basic context checks. Other object layouts still need device
validation. Use a separate `/data/local/tmp/echoes` directory and do not replace
or restart the D2R application.

## Remaining work, in order

1. **Resources:** both program NCAs contain `CompressionInfo` for RomFS. The
   existing hactool cannot interpret that compressed storage; treating its
   output as an ordinary full RomFS fails directory/hash checks. Obtain a
   validated decompressed base plus patched 1.1.0 resource view with a tool
   supporting this format. Do not reuse D2R's RomFS size or file samples.
2. **Native startup:** the four probe modes pass on the device above. Proceed
   to module constructors and `nnMain` only with the actual resource view and
   title-specific graphics/service configuration ready. Unknown
   services must remain named failures rather than fabricated successes.
3. **Graphics:** `main` has 536 NVN-like name strings. Fifteen are outside the
   existing 523-name registry, including the separately intercepted
   `nvnBootstrapLoader`; the other fourteen require lookup/semantic review.
   Name presence alone does not prove that a function is called. Prepare and
   verify this game's own Maxwell-to-SPIR-V shader cache, then pursue first
   present on system Vulkan before driver-specific tuning.
4. **Platform and app:** review AudioRenderer as well as AudioOut, fonts,
   accounts, save mounts, and optional NFP interfaces. Build an independent
   Android package with controller input first, then touch controls, aspect
   handling, and FPS. The current D2R app/bootstrap contains title-specific
   paths, resource checks, and startup hooks and cannot simply be repackaged.
5. **Gameplay/performance:** cold start, title screen, new game, movement,
   combat/echo actions, area transitions, save/reload, and audio; then measure
   CPU/GPU frame time and thermal stability across Adreno and Mali. D2R's 60 FPS
   experiment is not evidence of Zelda performance or timing compatibility.
