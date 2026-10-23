# Android launcher visual update (2026-10-23)

中文：[ANDROID_LAUNCHER_STYLE_2026-10-23.zh-CN.md](ANDROID_LAUNCHER_STYLE_2026-10-23.zh-CN.md)

This update refines the completed [launcher settings and save manager](ANDROID_LAUNCHER_SAVES_2026-10-23.md), with version `1.2.1-94012` / `versionCode=7`.

## Visual design and implementation

- Original static scenery: ruined arches, masonry outlines, warm/cool haze, dust and a vignette. `LauncherStyle.Backdrop` uses Android Canvas with gradients cached by viewport size, without downloaded pictures, game assets or continuous animation.
- Cards: translucent dark gradients, fine metal borders, section icons and title rules. Titles use a system serif font; body text and values retain clear sans-serif fonts.
- Settings: functional icons, distinct selection surfaces and trailing chevrons, with press ripples and focus borders. Disabled text and icons become dimmer.
- Action buttons: a brass gradient and play icon for launching; outlined buttons with matching icons for import, export, backup and help. The start action remains fixed at the bottom.
- Toggles and dialogs: the FPS switch and selection dialogs share brass accents, with rounded dark dialog panels and dimmed backgrounds.

`LauncherStyle.java` centralizes scenery, icons and control states. Only `InstallerActivity` adopts `launcher_theme.xml`; `launcher_dialog.xml` supplies the dialog surface. Visible text continues to use the existing shared string table.

## Validation and scope

The separate emulator package uses synthetic files: Chinese at default font scale and English at 120% each pass 39 checks; the host save/settings suite passes 100 checks. Visual inspection covers landscape columns, the scrolled portrait save panel, the fixed start button and setting dialogs. Controls remain clickable, text wraps, and navigation bars do not obscure buttons. The 17 APK packager tests, 475-string i18n check and translation generation check pass.

Save protocols, guest startup flow and native rendering code are unchanged; native SHA-256 remains `47d7f4bf68faaf7227096b74de7c22a369d7c421c37c9c97bad42c45161ff162`. This update received no physical installation or in-game acceptance. TNA was not operated; PPG remains disconnected.

Local screenshots and logs are under `D:/AndroidSwitch/analysis/launcher-style-20261023/qa-zh/` and `qa-en-font120/`; the Chinese contact sheet is `qa-zh/review.png`. The isolated QA version label is separate from the product APK version.

Local preview APK: `D:/AndroidSwitch/output/releases/D2R-Mobile-1.2.1-94012-launcher-style-20261023/D2R-Mobile.apk`. It retains the release signature, is non-debuggable, and can upgrade the previous version while reusing existing resource packs. APK SHA-256: `56bd05871333fbf8e43d71e366578f683aa3d3e87815445ef9fb7f183abfc3cb`.

The source change contains original drawing code and Android theme resources, with no game artwork, real saves or built packages.
