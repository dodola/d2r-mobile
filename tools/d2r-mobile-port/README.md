# Android offline installer

The launcher accepts a single `.d2rpack`, streams and hashes every payload, stages
the files privately, and starts GameActivity only after a verified commit. No
network or broad storage permission is required. The game image and shaders are
never added to Git; saves are not part of the installer allowlist.

This tooling uses the existing **D:\AndroidSwitch development workspace** for
the pinned Android native build, module conversion/verification tools, SDK/JDK,
driver libraries and locally prepared game resources. It is not a replacement
for those native build and resource preparation steps. The resource-pack tool
also supports explicit `--image`, `--cache`, and `--output` paths.

When running the copies committed on the installer branch, set the workspace
location. The APK's Java sources and manifest are taken from that checkout:

```powershell
$env:ANDROID_SWITCH_WORKSPACE = 'D:\AndroidSwitch'
python tools/d2r-mobile-port/build_offline_package.py
python tools/d2r-mobile-port/build_apk.py --output D:/AndroidSwitch/analysis/d2r-mobile-port/offline-release/D2R-Mobile.apk
python tools/d2r-mobile-port/verify_offline_installer.py --full
python tools/d2r-mobile-port/verify_offline_installer.py --device DEVICE_SERIAL
python tools/d2r-mobile-port/write_release_notes.py
```

`D2R_ANDROID_SOURCE_ROOT` optionally selects a different source checkout. The
normal package ID and debug signing key are preserved for compatible in-place
updates. Device QA uses **org.d2r.offline.installertest**, never clears or updates
the production app, and has an explicit device serial. Remove the two QA apps
and the specifically named QA download directory after testing.

The release directory contains `D2R-Mobile.apk`,
`D2R-Offline-91738.d2rpack`, the pinned `offline-manifest.json`, provenance reports,
Chinese installation instructions and SHA-256 checksums. The pack contains the
existing build-91738 game resource image plus the generated shader cache;
native game modules and driver libraries stay in the APK.

Container format: eight bytes `D2RPKG1\n`, a four-byte big-endian manifest length,
the exact UTF-8 manifest embedded in the paired APK, then each uncompressed file
in manifest order. Both parser and path allowlist are bounded. Files use 64-bit
sizes. Every input byte is hashed, including payloads whose installed/staged
files can be reused. Trailing and truncated input are rejected.

Copying is cancellable; verified completed files survive a retry. An incomplete
individual file is restarted. The app cancels foreground import when leaving
its screen. A durable commit marker permits interrupted final installation to
recover without the original pack. On later launches a receipt checks the pinned
manifest and each file's length/mtime; legacy or changed files receive SHA-256
validation. Import does not touch saves, uninstall deletes private app data, and
the downloaded pack can be deleted only after installation succeeds.

Verification combines host/Android corruption and recovery tests, a real 9.18GB
import with `-Xmx64m`, actual system document selection, and first-launch evidence.
These checks establish installation behavior, not compatibility with every GPU
or complete-game playability. Source/build provenance is recorded beside the APK.

Version 0.4 automatically selects the APK's bundled Turnip driver before native
Vulkan initialization on Android 12+ with `Build.SOC_MODEL == SM7325`. On the
tested TNA-AN00 / Adreno 642L / Android 14, the stock Qualcomm driver returned
`-13` from `vkCreateComputePipelines` and aborted before the first frame. The
bundled Mesa 26.2.3 driver passed that startup and rendered the game. Other SoCs
retain their existing driver behavior. The existing native loader and native
binary are reused; automatic selection needs no persistent diagnostic marker.
The prepared launcher now stays open to offer three remembered driver choices:
automatic recommendation, system driver, and bundled Turnip. The user taps
Start game after choosing; first-time resource import still launches on success.
Explicit choices supersede old private-driver diagnostic markers. The choice
is applied before the native renderer initializes and can be changed after a
crash by reopening the launcher.
This APK update retains the package ID, signing key, resources and saves, and
uses the same offline resource pack. Host policy checks cover the API guard and
ensure that unknown SoCs do not opt in.

The optional `make_install_guide.py` creates the eight-page illustrated Chinese
PDF using the retained real QA screenshots in `build/offline-installer-tests`
and the TNA-AN00 driver-entry screenshot in `analysis/d2r-mobile-port`.
It needs ReportLab, Pillow, pypdf and the Windows Microsoft YaHei fonts. The PDF
is written under `output/pdf` and copied into the offline release directory.
System installation and file-navigation diagrams are labelled as illustrations.
