# Issue #1：冰冷之原房屋闪退的候选修复

反馈：https://github.com/dodola/d2r-mobile/issues/1 。角色进入冰冷之原的房子时闪退，用户补充有多人反馈。尚无该场景的崩溃日志、故障栈或实际命令记录，因此本轮不能确认房屋闪退根因，也没有关闭 Issue。

已修复一个能够离线复现的 Vulkan 回放退出点：原 `OP_POLY_OFFSET` 只要 factor/units/clamp 任一非零就 abort，甚至没有启用 offset 时也退出；draw 又拒绝任何 offset enable。合法状态设置本身不应退出。

现在独立保存 polygon offset 参数，填充图元按 NVN FILL 位（4）启用 Vulkan depth bias，factor→slope、units→constant、clamp→clamp。未启用时使用全零有效状态。完整有效状态进入 PSO key，避免偏移不同的 draw 复用错误管线；`depthBiasClamp` 按实际能力启用，并加入持久 pipeline cache 的 feature identity。日志记录 clamp 能力及拒绝时的具体参数。

能力限制仍存在：当前只支持填充多边形；设备没有 depthBiasClamp 且 draw 真正启用了非零 clamp 时仍会明确退出，不会丢掉 clamp 或偷偷替换驱动。本次没有证据证明房屋实际使用了 polygon offset，不能将已修复的命令问题等同于 Issue 已解决。

同时修正崩溃证据丢失：nativeConfigure 原来直接截断 `diagnostics/game-startup.log`；现在在新会话开始前原子重命名为 `game-startup.previous.log`。崩溃后第一次重进可保留上一局日志，只保留一代。文件位于应用私有目录：`files/d2r-mobile/diagnostics/`。应读取 previous 中的 STOP / ANDROID_FATAL 和前面几十行，而不是重启后的日志。已有旧包重启时被截断的日志无法恢复。

本轮还发现并修复打包流程中的独立缺陷：此前 `controller-back/package-final` 的 manifest 启动 InstallerActivity，但 DEX 没有该类及安装器依赖；签名和对齐检查通过仍不能保证可启动。现支持从保留的离线安装器工作树补齐 base-only Java 源，记录各来源 SHA，并在签名前解析二进制 manifest 与全部 DEX，缺少任何应用组件或出现重复类就拒绝出包。

离线安装器仍保留在原工作树；本次 APK 合并其 InstallerActivity、OfflineInstallSession、OfflinePackageInstaller、GraphicsDriverSettings 四个源文件，使用 main 的 GraphicsDriverPolicy。自动档默认系统驱动，与 main 实际行为一致；入口显式系统/Turnip 选择在 nativeConfigure 前应用。没有以自动切换 Turnip 处理此次闪退。

验证结果：

- 真实 `nvn_vk.c` opcode dispatcher 的 AArch64 离线对照：`b0f012c` 旧代码在 `Begin → PolyOffset(1.5,-2,0.25) → End` 上命中指定 STOP；新代码回放完成。另覆盖禁用/FILL/非 FILL、参数保存和重置、clamp 能力、非有限值、有效偏移的 PSO key 区分。
- 离线 ELF 只执行 CPU 回放和状态断言；Vulkan 驱动没有调用。未走到的其他 replay 函数有未解析目标，保持地址 0 不可访问，误入即失败，不能把此测试称为 GPU/场景验证。
- Android native 构建、Java/DEX 编译、APK 签名和 16 KB zipalign 通过。
- 旧候选 46 个 DEX 类、缺 InstallerActivity；新候选 70 个类、两个 manifest Activity 都存在。完整打包反例确认未提供安装器源时明确拒绝生成 APK。
- 没有操作设备，没有进行房屋场景或系统驱动 GPU 验证，也未测试所有入口资源及安装流程的实际 UI。

重跑 CPU 对照（需要 NDK 与 Python unicorn）：

```powershell
python -X utf8 dm/android/tests/run_polygon_offset_offline.py --ndk C:/Users/dodol/AppData/Local/Android/Sdk/ndk/27.2.12479018 --unicorn-dir D:/AndroidSwitch/tools/python-analysis --before-ref b0f012c --output D:/AndroidSwitch/analysis/issue-1-house/runner-check
```

本轮候选 APK：`D:/AndroidSwitch/analysis/issue-1-house/package-verified/d2r-attachment-menu.apk`

- APK SHA-256：`655cf9c52c3aa9c950289402c9a20e4aa6904c62ca38d0d5e08dcfe4a151741a`
- Native SHA-256：`db7920f2c53b5aa41f7d0a96d033e81243ff8acd6790ed91612286cd2535a8f6`
- 证据：`D:/AndroidSwitch/analysis/issue-1-house/` 下 native-final.json、polygon-regression.json、runner-check、launcher-regression.json、package-negative.json 和 package-verified/result.json。

下一步应使用系统驱动复测冰冷之原房屋：进门、停留、出门、重复往返；若仍闪退，第一次重启后取 previous 日志，按真实 STOP 或故障栈继续定位。当前候选只能证明已修的 CPU 命令退出点及打包入口完整性，尚不能证明多人反馈的房屋问题已经消失。
