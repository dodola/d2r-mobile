# 手柄 B / Android 返回键修复候选

后续更正：本页原候选 APK 虽通过签名和对齐检查，但 DEX 漏了 manifest 的 InstallerActivity，不能用于普通桌面启动。已在 Issue #1 排查中修正打包流程；请改用 `ANDROID_ISSUE_1_HOUSE_CRASH_2026-10-17.zh-CN.md` 记录的新候选包。B/Back 源码修复仍保留。

在 `main` 合入上游 `6edec84` 后处理用户反馈的“按手柄 B 闪退”。当前没有反馈现场的 logcat/tombstone，因此不能确认所有反馈都是同一根因，也尚未完成真机修复验证。

代码检查确认：原 GameActivity 未接管 Android Back，默认处理会结束 Activity；nativeConfigure/nativeStart 是进程内单例，Activity 再次打开可能遇到 EALREADY。若设备/手柄将 B 映射或回退为 Android Back，这条路径会表现为退出游戏、随后无法重新打开。它与 native SIGSEGV 等真正进程崩溃需要现场日志区分。

本次处理：

- GameActivity 消费 KEYCODE_BUTTON_B 和 KEYCODE_BACK，转换为客体 Npad B（mask 2），不走默认 Activity 退出。加载中、暂停或失败状态消费按键而不发送游戏输入。
- targetSdk 33 的现有安装壳保持 legacy Back 分发，`onBackPressed` 将系统返回转成游戏 B 短按。没有启用 predictive Back。
- B 使用独立原生输入通道，与触控按钮 OR 合并；短于一帧的普通按下/释放保留一次采样，触控释放或触控取消不会清掉仍按住的手柄 B。
- 去重 repeat，分别记录设备及 Back/B 键。失焦、暂停、销毁清理 held 和 pending；设备移除/按键 canceled 在一个原生锁内更新剩余 held，避免全清再发布造成虚假松开。无关设备移除或无关 canceled 事件不清短按锁存。
- 渲染设置和系统文字输入 Dialog 有独立 Window，单独消费手柄 B，并在非 canceled 松开时关闭弹窗，取消文字输入走现有回调。

这不是完整 Android 手柄适配；本次接入 B/Back，未扩展其他物理按键和摇杆轴映射。

离线验证通过：

- `BackButtonStateTest`：28 checks，覆盖 repeat、多设备、同设备 Back+B、无关移除/取消和清理。
- `t_controller_back.c` 链接真实 `input_backend.c`，NDK 编译 AArch64 ELF，在 Unicorn 执行原断言：短按锁存一次、触控共存、各通道取消、保留另一设备 held 不产生新沿、无效位拒绝、source 未 ready 时拒绝按下但允许释放。
- 离线 ELF harness 仅单线程；pthread 锁、日志、触控清理使用桩，不证明并发、真实 Activity/Dialog 分发或设备行为。
- Android native 构建、所有 Java 编译和 DEX 打包通过；APK 签名、16 KB zipalign 和 base APK 保留条目的哈希检查通过。保留原二进制 manifest、离线安装入口、图标、资源和其他库。

候选包：`D:/AndroidSwitch/analysis/compat-performance-20261017/controller-back/package-final/d2r-attachment-menu.apk`

- APK SHA-256：`03e5d83e509eef728d732b89f53a68d6511c3f120672d1f97c10fbc926cfe1fd`
- Native SHA-256：`168f6f5894ff9238c5eb0224d20566fdd8eaec1e28b7285f0951db68e6033f5b`
- 构建和离线执行证据：`D:/AndroidSwitch/analysis/compat-performance-20261017/controller-back/`，其中 `native-final.json`、`controller-back.result.json` 和 `package-final/result.json`。

遵照用户要求仅离线执行，没有安装、启动、清数据或覆盖存档。仍需在实际反馈设备上验证游戏各界面的手柄 B、Android 返回、弹窗、失焦和设备拔插，并收集仍出现的 native 崩溃日志。性能模式也仍未做真机画质/FPS 验证，不能据此宣称达到 30 FPS。
