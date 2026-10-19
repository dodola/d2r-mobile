# Android title bring-up

This branch starts Android support for **The Legend of Zelda: Echoes of Wisdom**
(`01008CF01BAAC000`). A visible first scene is verified; a playable port is not
yet established. iOS/Metal
and the existing D2R application are outside this work.

The [profile](echoes-of-wisdom.json) pins the locally inspected 1.0.0 (content
version 0) and 1.1.0 (196608) executables by SHA-256, full NSO build ID, NPDM,
and module set. **1.1.0 is the current bring-up target.** The Android
application ID and data directory are separate from D2R. A native-only diagnostic
APK now uses `org.switchports.echoesofwisdom`; it contains no game content.

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

The standalone SDK-startup result uses temporary fixtures and does not call
`nnMain`. The independent NativeActivity additionally passes all module
constructors, enters the real `nnMain`, loads actual resources, submits two
initialization presents, and records the first offscreen indexed draw (256×64).
Its vertex and fragment programs match the original control and machine code.
These startup checks alone do not establish visible output or a playable port.
The current test device is **ROD-W09, Mali-G610 MC6, Android API 34**. The latest
diagnostic APK additionally presents a **verified visible castle scene with guards
and torches**, confirmed by a device screenshot and nonblack source-image GPU
readback. Normal input, gameplay, saves and later scenes remain unverified.
The user has confirmed normal sound in the castle intro and correct orientation
and brightness after the Android presentation fixes. A later run still stops at
unsupported writable depth feedback.
No Zelda performance or thermal claim
is supported: validation and diagnostic timing is not a performance baseline.

### Android feature parity status

The Android host now has three separate pieces that should not be confused with
complete Echoes gameplay validation:

| Area | Implemented in this branch | Current limit |
| --- | --- | --- |
| Physical controller | `KeyEvent`/`AInputQueue` face/shoulder/stick-click buttons, key/hat D-pad, analog/digital triggers and both sticks feed an independent physical channel. Controllers are merged per device (button OR, farthest stick); removal preserves the remaining devices. Sticks use the iOS 25%/15% hysteresis and must return to centre after touch before taking over again. | Android input fixtures pass on a device, but physical-controller callbacks and the complete gameplay path still need a connected controller. D2R's game-private layout-switch/free-portal adapters remain separate work. Rumble is intentionally unsupported, as on iOS. |
| Save data | Android save mounts use a work tree and atomic snapshot/rename on `nn::fs::Commit`, with directory `fsync` and commit counting. The Echoes exit path queues the original applet exit request and waits for a real commit. | Echoes has no reviewed title adapter yet for `ingame` detection or a direct in-game save request. The Java Activity lifecycle cannot claim iOS-equivalent background-save waiting; it flushes/synchronizes the transactional tree and uses the generic adapter when one is bound. |
| Touch bridge | The Java Activity and Echoes NativeActivity route multi-touch events into Android `nn::hid`; the portable iOS touch state machine is compiled for Android, including portal/difficulty/skill/belt targets, hit testing, dynamic target masks and event snapshots. `dm_android_touch_bind_game_bridge()` runs optional callbacks on the guest HID sampling thread. | No Echoes-specific observer currently fills the state snapshot or dispatches events. The bridge is an ABI and integration point, not proof that every Echoes panel is touch-playable. |
| Cloud sync | `dm/android/cloud_android.c` can apply the portable sync policy to an explicit directory provider and flush snapshots on background/exit. | Android has no iCloud provider in this repository; the provider is disabled unless `DM_ANDROID_CLOUD_DIR` or an embedding provider supplies a directory. |
| Host settings and utilities | The portable settings store (`dm_custom.c`), locale/string fallback (`dm_i18n.c`) and random-name utility (`dm_namegen.c`) are compiled into the Android core. Settings are initialized beside the app save tree before the guest mounts it; D2R Activity passes its Android locale through `DM_LANG`. | The custom page widget pump remains disabled until a title UI adapter is reviewed. The `q3d` setting is persisted but has no Android 3D-scale adapter yet; Echoes does not receive guessed D2R widget/object offsets. |

The D2R Activity target also links the iOS-compatible `dm_touch_game_*`
binding observer. It is installed only when the loaded D2R artifact advertises
`OBS_BINDING_GETTER` and passes the exact module-size/identity checks; Android
uses `process_vm_readv` for checked same-process guest reads. This does not
enable the observer for Echoes, whose stripped 1.1.0 image has no reviewed
HUD/UI/save ABI.

The exact implementation is in `dm/android/input_backend.c`,
`dm/android/touch_controls_android.c`, `dm/android/lifecycle_android.c` and
`dm/host/hle_fs.c`. A device run that reaches only the castle scene does not
validate the rows above end to end.

On 2026-10-19 the independent Android input fixture passed on the connected
TNA-AN00 (API 34): 68 touch-only checks and 150 logical-controller checks.
The final revision also rejects stale raw-touch snapshots after a full input
reset. That revision passed 74 touch-only and 156 logical-controller checks on
ROD-W09 (API 34), after TNA-AN00's ADB connection went offline.
These call the registered `nn::hid` host bindings and verify short taps,
independent button latches, per-stick touch priority, cancellation, drift
hysteresis and the post-touch centring gate. They also verify that dispatching
an older touch snapshot preserves a new difficulty/belt request, background
cancellation discards pending events, and a pre-reset controller snapshot
cannot revive released input. The JVM controller reducer also
passes device-merge, disconnect and overlapping digital/analog button cases.
Neither fixture injects Android hardware events or proves a playable game
scene; their JSON results retain those explicit limits.

A **default-off depth-input experiment** is available only for the calibrated
Mali-G610 MC6 proprietary driver `v1.r38p1-01eac0.91ad9bb3ee6ee6b8afda80c5afcd3137`.
Creating the title-private `files/depth-input-experiment` marker and restarting
requests it; removing the marker and restarting restores the original path.
It additionally requires ARM ordered depth input attachments and the inspected
shader pair, D24S8 view, 1280x720 viewport and sampler state. Every replaced sample
checks an outward bound on its original projected coordinates; the GPU flag is
fence-checked before subsequent rendering, guest publication or presentation.
Unknown state or a failed check stops explicitly. Independent GPU fixtures and
host SPIR-V checks support this device experiment; they do not establish a
portable texture-filtering equivalence rule or complete gameplay compatibility.
With only that marker, the first integrated game draw passes this conservative
guard, but the next draw fails it (frame 810 in one run, 812 in another). The
captured draw exposed 16 conservative rejections and one exact half-step tie.

A second private marker, `files/depth-input-calibrated-ties`, selects an exact
integer guard with calibrated endpoint equality when **both markers** and the
same measured vendor, device and driver match. The original conservative and
strict APIs keep their behavior. Four immutable D24 depth planes covering
9,191,424 sample events showed no difference for tested centers, interior points
and exact ties; differences occurred outside the interval. This is finite device
evidence, not a portable Vulkan rounding guarantee. Coordinates beyond the
half-step still stop before guest publication or presentation.

APK117 passes all five encountered PS3749/VS4a25 input draws, with real fence
checks and zero VUID or validation-error messages (11 non-VUID warning lines).
The last profile is labelled frame842; the run then stops at another unsupported
pair, PS73adabd418bb541e / VS72f3a02ea695e6be, depth binding1, draw134302.
APK118 captured that pair before its draw at frame834/draw134281: all 39 files
match device SHA-256, including its distinct eight-attribute geometry, effective
uniforms, support/C1 and pre-draw depth. This run again passes five earlier
input draws and records zero validation-error/VUID lines and 11 warning lines
(the callback throttles repeated warnings).

Independent replay covers 12,440 fragment invocations for each of two immutable
depth planes. The captured plane has one sampled-versus-current-pixel mismatch;
an asymmetric control has six. Complete event capture and exact rational checks
find three endpoint ties and six coordinates outside the calibrated interval;
all observed mismatches are outside it. A separate fixture using the complete
transformed original fragment shader also reports closed-guard rejection with
immutable depth and controlled dummy textures; it makes no alpha/output claim.
**This new pair is not
admitted to the input-attachment experiment.** Its alpha also consumes an
unwritten vertex-output component, which needs an original export-semantics
audit; no default value or harmless-discard behavior is assumed. These checks
do not establish later scenes or playable operation.

### Optional Adreno driver experiment

The independent APK packager accepts `--turnip-driver PATH --adrenotools PATH`
together to include externally supplied AArch64 libraries. Without these options,
the APK uses the system-driver package layout. Each library's SHA-256 and ELF
load alignment are recorded separately; APK signing and ZIP alignment are checked.
The prepared APK119 bundle's libraries have 16 KiB-compatible ELF load segments,
which alone does not establish compatibility on a particular Android device.

Driver selection is separate from bundling. The title-private `files/private-vulkan`
marker requests Turnip; `files/private-vulkan-sysmem` additionally requests sysmem
and implies Turnip. Restart after changing markers. With both absent, the system
loader remains selected. Keep these markers absent on Mali. The shared loader
checks that a requested private driver actually reports the Mesa Turnip driver ID.

An earlier **D2R** run on Adreno 830 / Turnip Mesa 26.2.3 reports native attachment
feedback support, making that combination a concrete next validation target.
**Zelda has not been tested on that combination.** APK119 is built but not
installed; the current Mali device remains on diagnostic APK118.

### Resource and shader progress

- Current Atmosphere **hac2l** understands the Program NCA compression layer;
  the old hactool output was unsuitable. Verified base/update extraction produced
  a **8,036,390,636-byte** 1.1.0 RomFS, SHA-256
  `6854222fabc60f71a28fdc971c9aa8c0b686e8e94c8a65e4ca0bc8561252d60c`.
  Its tree has **10,965 files and 431 directories**, with every file hashed.
- This RomFS places metadata after file payloads. The inventory and Android
  range reader now accept that layout while rejecting overlap and overflow.
  The full image was deployed and its device SHA-256 matched the host.
- The C++ resource streams call `fopen`. The title now opens a guest SDK
  `fopencookie` stream backed by the existing checked Android mount/range reader.
  Its `FILE` remains owned by the guest SDK. Returning a Bionic `FILE` to guest
  libc++ would violate the ABI. This bridge currently supports read-only modes.
  The initial vibration-resource failure was this missing stdio route, rather
  than an async-wait bug; no artificial wait was added.
- **96 BF SHA/BNSH archives**, 23,771 stage records and **3,576 distinct programs**
  were extracted. All programs translate to SPIR-V. Typed texture instructions
  and typed surface reflection supply sampler facts, including lowered
  bindless surface handles. Missing/ambiguous facts remain explicit failures.
- Identical control blocks can describe different active programs in this
  game. The original control-only cache was quarantined. Index version 2 stores
  and compares the complete original control **and code**, with bounded records
  and exact byte comparison after hash lookup. D2R's version 1 format is retained.
  The title bypasses the pointer-only control cache during bring-up.
- NVN combined texture handles use c2 byte base `0x20`; the image bank starts
  at `0x120`. Image size queries share the image bank only when their SPIR-V
  uses prove they perform size/level/sample queries without sampling contents.
- Two shaders exposed upstream invalid `ConstOffset` composites. Their values
  are proved from single-assignment locals and immutable original c1 words,
  then emitted as constants. Dynamic or aliased inputs are rejected. This is
  required by Vulkan's constant-offset rules; it does not substitute shader logic.
- Two archives embedded in the original main module add **40 programs**.
  The original `Common.ptcl` particle container supplies **5,951 translated and structurally
  validated programs**. Two are byte-identical duplicates of the initially added
  particle pair; the full merge contains **9,567 unique cached programs**.
  Merge accepts duplicates only when original index entries and payload bytes
  match. Runtime-observed vertex-type corrections use explicit, hashed-evidence
  overlays while preserving the original control/code identities.
- The Mali device lacks shader clip/cull-distance features. Android removes
  only proved unused distance members or proved redundant ClipDistance0 writes
  equal to final Position.z, retaining draw-time requirements for native
  zero-to-one clipping with depth clamp disabled. A guarded GPU fixture checks
  negative-z clipping, visible positive-z geometry and far-plane clipping.
  All **5,211 vertex modules** processed by the current helper snapshot pass
  `spirv-val --target-env vulkan1.1`: 1,005 need the native-clipping draw guard;
  4,206 have only unused distance declarations. These offline checks precede
  later runtime storage-bias/null-resource adaptation.
- SPIR-V validation and cache packing do **not** prove all variants' rendering,
  vertex attribute types, image views, physical device features or resource
  binding behavior. They do not establish a visible game frame.

### Audio and current graphics boundary

Startup passes depth ZCull allocation, 3D texture writes and loading all six real
shared fonts. **AudioRenderer is now connected**, using the original SDK's
configuration, decoders, effects and software DSP with real blocking AAudio
PCM16 stereo output at 48 kHz. SDK threads and host completion events preserve
its update pacing; no dummy work-buffer size or fabricated IPC success is used.
This backend currently supports the title's automatic 240-frame request, rather
than claiming generic AudioRenderer compatibility.

A standalone original-SDK voice/configuration test passed **three open/close
cycles**, nonzero PCM, actual AAudio device-frame consumption, completion events,
stop/restart and thread/event lifecycle checks. Actual game logs now also show
nonzero PCM blocks, and the user confirms normal sound in the castle intro.
Music/effect quality across gameplay,
ADPCM coverage and audio-device disconnect recovery remain unverified. Fixture
audio and nonzero samples alone do not establish complete game audio.

The current Mali run presents a **verified visible first scene**. Android's
`NVNtextureView` representation now fits the original **16-byte ABI**, preserving
adjacent views and copy regions; the standalone device fixture passes six cases.
Nearest-filtered, same-format D24S8 scaling also preserves depth and stencil in
the GPU copy fixture. These fixes resolve the observed black presentation path;
the first-scene screenshot shows castle guards and torches, and a source-image
readback contains 689,939 nonblack pixels out of 2,073,600.

The first-scene capture has no observed validation errors or named stops up to
that point. This does not prove later rendering or normal play. The user has
confirmed correct orientation and brightness in APK111: final presentation now
accounts for LOWER_LEFT origin and preserves encoded colors when the surface
offers a different sRGB/UNORM format. A standalone GPU fixture passes seven blits
and a bit-preserving copy, including crop, Y flip and guards. Continued
execution still reaches `DEPTH_FEEDBACK_UNSUPPORTED` for texture serial 7159;
unsupported overlapping reads/writes remain explicit stops. The user confirms
normal castle-intro sound; later audio remains unverified. The build still enables validation and
diagnostics, so its timing is not a Zelda performance baseline.

Android ZCull uses a **64-byte opaque host record**, aligned to the advertised
32-byte requirement. Color textures still require zero bytes. Returning zero
for depth previously sent a zero-sized request into NumberLineAllocator and
crashed. Save records the conservative external-hierarchy state with the active
depth format, extent, samples and view range, plus version and integrity fields.
The GPU writes the record into real pool storage, so normal GPU buffer copies
and fence-gated CPU publication apply. Restore orders queued writes, waits for
the real fence, validates the record and retains the destination VkImage's
depth/stencil pixels and driver-managed hierarchy. It does **not** serialize
Nintendo hardware metadata or restore old depth pixels. Matching single-layer
attachment views are covered; broader view/MSAA support is not established.

The standalone NVN regression passes eight cases, including save → GPU copy → restore, cross-submission
restore, 384 depth/stencil pixel checks, and actual LESS depth tests (0 hidden
pixels colored, 36 visible pixels colored), plus nearest-filtered D24S8 scaling
with 288 depth/stencil pixel checks and 128 guard bytes. Corrupt/incompatible records,
out-of-pool ranges, unaligned addresses and insufficient sizes stop by name.
These are fixture checks and do not establish the title's full ZCull usage or
its visible output.

`WriteTexels`/`WriteTexelsStrided` update both block-linear guest storage and
the Vulkan image for uncompressed 2D/3D color textures, waiting for completion.
Device checks cover padded row/image strides, a partial volume box and preservation
of GPU-owned pixels outside that box (4,096 pixels checked). Other texture-write
targets/formats still stop explicitly. Mirror-clamp-to-edge samplers require the
queried/enabled Vulkan extension; no unconditional vendor-specific assumption
is used. Standalone regression coverage now includes both the earlier Adreno
device and the current Mali device; this does not establish full-game driver
compatibility.

Android-only startup additions cover permission/RELRO locking, original SDK
notification/style/vsync events, buffers, texture-view getters/swizzles,
the zero-extension form of `ProgramSetShadersExt`, and the draw/state calls
observed so far. Unsupported enabled features still stop by name. These are
bring-up implementations, not a complete newer NVN driver.

The rebuilt four-mode Zelda probe and D2R `load`/`sdk-startup` regressions pass.
Python tests cover identity, archive bounds, sampler facts, image-bank mapping,
constant-offset proofs, vertex-type evidence, strict cache merge/overlay provenance,
distance-capability preservation and shared-font bounds/checksums. C fixtures cover
leading/trailing RomFS, memory permissions, legacy/full-code cache identity,
malformed bounds, and ZCull record integrity (optimized and ASan/UBSan builds).
No D2R APK was restarted or gameplay regression-tested. iOS/Metal code paths
are unchanged. APK/ELF 16 KiB alignment is checked, but this is not validation
on a 16 KiB-page Android device; partial host-page permission changes are refused.

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

## Resource, shader and diagnostic APK commands

Use a current [Atmosphere hac2l](https://github.com/Atmosphere-NX/hac2l)
with compression support to verify/extract the patched resource image. Supply
the original base Program NCA with `--basenca`; the 1.1.0 update Program NCA is
the final input. A Linux build can run through WSL. Keep key files local and
do not publish the tool's unfiltered key diagnostics.

```sh
hac2l --disablekeywarns --enablehash --verify \
  -k /local-keys/prod.keys --titlekeys /local-keys/title.keys \
  --basenca /local-game/base-program.nca \
  --romfs /local-build/game-1.1.0.romfs /local-game/update-program.nca
```

From the repository root, use fresh directories outside the checkout:

```powershell
python dm/tools/romfs_inventory.py --image D:/local-build/game-1.1.0.romfs `
  --out D:/local-build/resources --extract-suffix .bfsha --extract-suffix .bnsh
python dm/tools/bnsh_inventory.py --resources D:/local-build/resources/romfs-inventory.json `
  --out D:/local-build/shader-pass1
dotnet build dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -c Release -o D:/local-build/translator
dotnet D:/local-build/translator/dmsh_xlate.dll D:/local-build/shader-pass1/jobs.json D:/local-build/shader-pass1/spv

# A second inventory incorporates typed surface reflection after bindless
# handles have been lowered to the original c2 image bank.
python dm/tools/bnsh_inventory.py --resources D:/local-build/resources/romfs-inventory.json `
  --image-reflection D:/local-build/shader-pass1/spv --out D:/local-build/shader-final
dotnet D:/local-build/translator/dmsh_xlate.dll D:/local-build/shader-final/jobs.json D:/local-build/shader-final/spv
python dm/tools/bnsh_vulkan_cache.py --inventory D:/local-build/shader-final `
  --resources D:/local-build/resources/romfs-inventory.json --spv D:/local-build/shader-final/spv `
  --validator C:/local-ndk/shader-tools/windows-x86_64/spirv-val.exe --out D:/local-build/shader-cache

cmake --build D:/local-build/android --target echoes_android
python dm/tools/android_title_apk.py --native D:/local-build/android/libechoes_android.so `
  --sdk C:/local-android-sdk --java C:/local-jdk17 --out D:/local-build/apk
adb -s DEVICE_SERIAL install -r D:/local-build/apk/echoes-bringup.apk
```

The translator uses .NET 10 and the bundled Ryujinx source. DebugMode remains
enabled because the upstream AST optimizer fails on many of these programs.
Read `validation.json` and check its excluded programs before deploying a cache.
The regular RomFS pass above contains **3,576 structurally validated programs**.
Adding the original main module's embedded archives and full particle container,
then checking identical duplicates and explicit vertex-type overlays, produces
the **9,567-program** development cache described above. Those additional sources
are required to reproduce the full cache; the regular RomFS commands alone do
not produce it. Payload hashes and runtime metadata bounds are checked, but this
is not verification of every GPU program or a first game frame.

Deploy only to the separate package's external files directory:
`/sdcard/Android/data/org.switchports.echoesofwisdom/files/`. It requires
`game.romfs`, the prepared four-module products under `modules/`, and
`shader_index.bin` plus `.vkmeta`/`.spv.gz` files under `shaders/`, and decoded
firmware shared fonts `fonts/0.sfnt` through `fonts/5.sfnt`.
The private files directory contains `bringup.log`, `bringup-status.json` and
optional shader captures. The APK includes none of those game resources.

After extracting the firmware font NCAs with hash verification outside the
checkout, decode their BFTTF files and validate every SFNT table checksum:

```powershell
python dm/tools/shared_font_decode.py --input-dir D:/local-build/firmware-fonts `
  --out D:/local-build/shared-fonts
# Deploy only 0.sfnt ... 5.sfnt to this title's external files/fonts directory.
```

The six types are standard, simplified Chinese, extended simplified Chinese,
traditional Chinese, Korean and Nintendo extended. The runtime maps actual font
bytes read-only for its process lifetime; no host substitute font or placeholder
font size is used. Firmware/font data and any keys remain outside the repository.

Reproduce the standalone ZCull/texture-write regression on an explicitly selected
device, using the existing original D2R fixture shader cache (index version 1,
keys `6ec4bfde22385501`, `8ac93c54cfb3cbe3`, `0d721a0acc36c0bf`):

```powershell
python dm/tools/android_nvn_zcull_check.py --build-dir D:/local-build/android `
  --shader-cache D:/local-build/original-fixture-cache `
  --out D:/local-build/zcull-device-check --serial YOUR_SERIAL `
  --adb D:/Android/platform-tools/adb.exe --cmake D:/Android/cmake/bin/cmake.exe
```

This builds the explicit `dm_android_zcull_probe` target, runs one positive and
seven negative cases, and writes a JSON receipt and text logs. It never installs
or restarts an APK. Depth readback test helpers are compiled only into the probe.

## Remaining work, in order

1. **Graphics compatibility:** resolve the reproduced later writable-depth-feedback
   boundary, preserving the verified orientation and brightness. Use original
   shader/resource identities and actual draw-state evidence; keep unsupported
   behavior explicit. Verify resource synchronization,
   compute writes, vertex types and shader variants on Adreno and Mali separately;
   retain the proved near-clipping draw guards.
2. **Beyond the first visible scene:** verify normal input and continued rendering
   through the next scenes. The confirmed castle image does not establish normal
   gameplay or a complete port. Use text diagnostics first and capture an image
   only when needed to confirm output.
3. **Audio in the game:** extend the confirmed castle-intro sound to later
   scenes, then exercise pause/resume, device changes and longer
   playback. The standalone
   AudioRenderer regression does not establish complete game audio quality.
4. **Platform and app:** check shared-font language/glyph coverage, accounts,
   writable stdio/save mounts and optional NFP interfaces. Validate controller
   input, then add touch controls, aspect handling and FPS to this independent
   application. No D2R app paths or save data should be reused.
5. **Gameplay/performance:** cold start, title screen, new game, movement,
   combat/echo actions, area transitions and save/reload; then remove diagnostic
   overhead before measuring CPU/GPU frame time and thermal stability across
   Adreno and Mali. D2R's 60 FPS experiment is not evidence of Zelda performance
   or timing compatibility. Android 16 KiB-page device coverage is still pending.
