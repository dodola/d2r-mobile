# Android launcher settings and save management (2026-10-23)

中文：[ANDROID_LAUNCHER_SAVES_2026-10-23.zh-CN.md](ANDROID_LAUNCHER_SAVES_2026-10-23.zh-CN.md)

This change references the launcher settings and ZIP save interactions in [sydeEvans/d2r-android](https://github.com/sydeEvans/d2r-android/tree/5d8e32210b6d54bc0f8140865332a8616766779f) (MIT). Centralized settings, visible current values and the system document picker are adapted to this repository's existing configuration and save transaction protocols.

## User flow

The launcher has game resource, display setting and save management cards: one column on narrow screens, two at widths of at least 720 dp. Content scrolls while the start game / initial package selection action stays fixed at the bottom. Resource verification, streaming import, cancellation and retries retain the existing installer state machine.

| Control | Implemented behavior |
|---|---|
| 3D render scale | Automatic, 100%, 90%, 75%; reads and preserves existing custom values between 50% and 100% |
| Graphics driver | Automatic, system, bundled Turnip; shows the selected preference, applied on launch |
| Show frame rate | Shares the in-game Custom toggle |
| Launch MOD | Selects an installed MOD or the base game; also changes the save transfer target |
| Export saves | Writes all files in the selected mode's committed save tree to ZIP |
| Import saves | Previews target, file count, character file count and overwrite count; merges after confirmation |
| Export previous automatic backup | Exports this mode's latest pre-import ZIP; its files can be restored using import saves |

Changes persist immediately and apply on the next game launch. Render scale uses `files/d2r-mobile/diagnostics/render-scale.txt`; FPS uses `show_fps` in `files/d2r-mobile/sw_custom.txt`, preserving language, MOD and unknown keys. Driver selection uses the existing `GraphicsDriverSettings`, and MOD selection uses `ModSelectionStore`. No driver recommendation or device policy changes are introduced.

This change does not implement upstream's handheld/docked mode or arbitrary 1080p/1440p output selection. Render scale changes 3D rendering work and is not an output resolution selector. System driver black blocks are outside this change.

## ZIP layout and import rules

Base saves target `files/d2r-mobile/save`; MOD saves target `files/d2r-mobile/mods/<id>/save`. Exports include character, shared stash, Settings, hidden metadata and other files, excluding native `.work` and `.new` working copies.

ZIP contents correspond directly to the save directory, without an extra `save/` wrapper, for example:

```text
d2r-save.json
hero/hero.d2s
hero/hero.d2s.meta
Settings/Settings.json
```

The `d2r-save.json` marker uses `format=d2r-save`, `version=1`, and records `game_build=94012` and `scope=base` or `mod-<id>`. It is excluded from imported save data. Structurally, upstream ZIPs with the same format but no scope and ordinary ZIPs without a marker are accepted; this does not verify the semantic compatibility of arbitrary game versions, save contents or MODs. Scoped packages cannot cross base/MOD or MOD/MOD boundaries. For packages without a scope, users must choose the correct target in the preview.

Imports overwrite matching relative file paths while retaining other characters and files; they do not replace the entire directory. Restoring a backup also merges and does not remove characters created after the backup. Character counts count `.d2s` files without parsing character data.

Imports extract into independent staging and leave saves untouched until confirmation. Confirmation copies and merges into a sibling next directory, backs up the old tree, writes a journal, and publishes with atomic renames. Files and directories are synced at commit boundaries. Failures attempt rollback; interrupted journals are recovered or cleaned before the next transfer or game launch. Cleanup failures after publication are deferred rather than rolling back to a partially deleted old tree.

Automatic backups live in private storage at `files/d2r-mobile/save-backups/<scope>-<time>-<UUID>.zip`; a `.partial` file is written and synced before publication as `.zip`. The backup export action discovers only complete backups for the current mode. An initial import with no old files creates no backup. Backups are not automatically pruned; uninstalling removes private backups, so export any that need to be retained through the system picker.

Transfers reject traversal, symbolic links, duplicate paths, parent-directory case aliases and file/directory conflicts. Limits are 32 MiB per file, 256 MiB total, 4096 files and 12 path levels. ZIP CRC is checked while reading. Invalid packages report failure while retaining committed saves.

A transfer locks its target from the document picker onward and excludes guest startup. Cancelling the picker releases the lock; picker, preview and worker state survive Activity recreation. Reopening the launcher while the guest runs returns to the existing game. After the game Activity is destroyed, the application process must fully exit before management is available again, avoiding overlap with asynchronous native shutdown. If the system kills the process, uncommitted imports must be selected again.

## Validation and delivery

All tests use synthetic data and the separate emulator package `org.d2r.launcher.qa`, with no real characters, game guest or native game library:

| Check | Result |
|---|---|
| `LauncherSaveTest` | 100 checks pass: ZIP round trips, overwrite/preservation, backups, commit sync failures, recovery, CRC corruption, expansion limits, paths, symbolic links, shared settings and startup exclusion |
| `LauncherTestInstrumentation` | 39 checks pass: real Activity, SAF streams, persistence, orientation recreation, preview confirmation, backup rediscovery/export, MOD isolation, system picker cancellation and visible fixed start action in both orientations |
| Existing installer and state tests | 40 + 21 checks pass |
| APK packager unit tests | 17 tests pass |
| i18n and launcher translation generation | 475 shared strings pass, including 69 additions; launcher translations require no native load |
| Existing literal scan | Output matches the HEAD baseline exactly on Windows; pre-existing path separator failures remain, with no new findings |
| Android Release native build | Pass |
| Actual APK build | Existing release signature, non-debuggable, version 6 / `1.2.0-94012`; 204 payload checks pass |

Reproduce host/emulator launcher checks:

```powershell
python dm/android/tests/run_launcher_checks.py --output <fresh-evidence-dir> --java <JDK17-dir> `
  --sdk <Android-SDK-dir> --emulator emulator-5580
```

Omit `--sdk` and `--emulator` for host-only checks; physical serials are rejected. Use a dedicated emulator because the script changes resolution, density and orientation. QA code is excluded from the product APK. The packager passes Java classes to D8 in one JAR to avoid Windows batch command length limits.

Local evidence is at `D:/AndroidSwitch/analysis/launcher-functional-20261023/qa-release/`, including logs, `result.json`, a portrait save management screenshot and a landscape launcher screenshot. The preview APK is at `D:/AndroidSwitch/output/releases/D2R-Mobile-1.2.0-94012-launcher-preview-20261023/D2R-Mobile.apk`. SHA-256: `99c2e768f1c1c09bb3278f4b76d546f500e1315f322d597f319791f6afeb0162`.

PPG is currently offline: real character import/load, combat and driver acceptance have not been performed. TNA received no install, launch or save reads. Real character loading for base/installed MOD saves still needs acceptance when PPG reconnects. The source commit includes no game content, real saves, signing material or built packages.
