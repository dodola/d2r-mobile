# Android HUD 与自定义设置

本次先合并上游 `jhaizhou-ops/main` 的 `53e7c5b`（合并提交 `648a44d`），保留 Android 平台适配，并接入上游中文、英文、西班牙语、俄语的四项语言设置。

## 使用方式

- 主界面移除常驻的“系统”“菜单操控／触摸操控”按钮，以及额外的方向、确认、返回、开始按钮。
- 触摸与实体手柄按输入来源自动切换；局外页面直接点按游戏控件，局内继续使用同源触屏 HUD。
- 从游戏“选项 → 自定义”顶部点击“Android 设置”，打开画质、图形优化、地图与 MOD 设置。手柄可用方向键选择入口并按 A 打开，返回后仍停留在自定义页。
- 帧率默认隐藏，在“Android 设置 → 显示帧率”中开启；选择会保存在本机。

“Android 设置”使用游戏内按钮打开 Android 对话框。对话框里的画质、图形优化和 FPS 沿用即时保存行为；MOD 下次启动生效。原有自定义页各行仍遵守“应用／放弃”规则，打开平台设置不提交或清除这些待应用值。iOS 不增加 Android 入口。

对齐参考为 iOS 同源触控实现及 [vickwv/android-integration](https://github.com/vickwv/d2r-mobile/tree/android-integration)；没有整体合并该参考分支，也不代表所有平台界面已完全一致。

## 构建与验证

新布局及多语言字符串必须重新生成 overlay，并与新原生库和 Java 一起打包。只替换 APK 中的原生库不会更新自定义页入口。此次增量生成复用经哈希校验的现有图集，重建当前源码的布局与字符串；没有重新生成位图。

已通过原生 CMake 构建、全部 Android Java 编译、语言与选项布局检查、i18n 表检查及 WSL 字面量检查。ARM64 设备 Custom fixture 通过 144 项检查，包含平台入口、消息匹配、手柄重复按键、模态排除、错误控件类型和待应用值保留；fixture 使用合成游戏控件，不等同于原游戏界面验收。

完整 APK 通过签名、payload 哈希和装配校验（204 个 payload），使用同一签名覆盖安装到 HONOR TNA_AN00，未卸载或清除数据。原游戏已验证：选角页点按进入游戏、局内打开自定义页及 Android 对话框、返回原页、FPS 开关保存与显示／隐藏、自定义页待应用勾选跨对话框保留，以及 Home 后恢复。测试结束用“放弃”还原临时勾选，关闭 FPS。

局外点击由 Android 转发到 iOS 同源鼠标消息队列，不重复推送游戏触摸屏；第一根手指驱动点击，取消／失焦／切换输入来源会清理队列。UI 设备 fixture 通过 62 项检查，新增未接入、非法相位／坐标、局内拒绝及取消守卫。实体手柄硬件、蓝牙重连、所有语言完整游戏流程与长时间稳定性仍需单独验收。

最终可调试测试 APK 为 `1.1.0-94012-hud`，SHA-256：`fffaf2e4d8f27e5649423250b57f4f944b25fc2b8ec9b1f8d1cc764da6777f28`。本地构建报告、fixture 日志和截图保存在工作区外部 `D:/AndroidSwitch/analysis/hud-custom-20261021/`；最终包位于 `apk-verified/`。该包已再次覆盖安装并确认四项语言单行显示、新入口可打开，应用 UID 仍为 `10561`。截图和游戏派生资源不提交到仓库。

## English

Android now follows the shared touch/controller input source automatically. The floating system/mode and extra menu buttons are removed. Open **Options → Custom → Android settings** for quality, renderer options, map reveal and MOD selection. FPS is hidden by default and can be enabled there.

The game button opens an Android dialog; its settings keep their existing immediate-save semantics, while the game Custom rows retain Apply/Discard. Opening the dialog preserves pending Custom values. Rebuild layouts and localized overlay strings along with Java/native code; iOS has no Android entry. ARM64 fixtures cover the checked request transport and pending values, but do not replace real-game acceptance or physical-controller testing.
