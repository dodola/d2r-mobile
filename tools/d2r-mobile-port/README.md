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
