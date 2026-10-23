# Android 启动设置与存档管理（2026-10-23）

English: [ANDROID_LAUNCHER_SAVES_2026-10-23.md](ANDROID_LAUNCHER_SAVES_2026-10-23.md)

本轮参考 [sydeEvans/d2r-android](https://github.com/sydeEvans/d2r-android/tree/5d8e32210b6d54bc0f8140865332a8616766779f) 的启动设置和 ZIP 存档交互（MIT）。借鉴集中设置、显示当前值、系统文件选择器的方式，按本仓库已有配置和存档事务协议重新实现。

## 用户操作

启动器分为游戏资源、画面设置、存档管理三张卡片，窄屏单列，宽度至少 720 dp 时双列。页面可滚动，底部固定“开始游戏”或首次“选择资源包”按钮。资源校验、流式导入、取消与重试沿用原有安装状态机。

| 入口 | 本轮支持的行为 |
|---|---|
| 3D 渲染比例 | 自动、100%、90%、75%；读取并保留已有 50%–100% 自定义值 |
| 图形驱动 | 自动、系统驱动、内置 Turnip；显示当前选择，下次启动应用 |
| 显示帧率 | 与游戏内 Custom 共用开关 |
| 启动 MOD | 选择已经安装的 MOD 或原版；同时更新存档操作目标 |
| 导出存档 | 将当前模式已提交存档的全部文件保存为 ZIP |
| 导入存档 | 先显示目标、文件数、角色文件数和同名覆盖数；确认后合并导入 |
| 导出上次自动备份 | 取出本模式最近一次导入前的旧存档 ZIP，可再次用“导入存档”恢复其中的文件 |

设置选择后立即持久化，下次游戏启动生效。3D 比例使用 `files/d2r-mobile/diagnostics/render-scale.txt`；FPS 使用 `files/d2r-mobile/sw_custom.txt` 的 `show_fps`，保留语言、MOD 和其他未知键。驱动使用现有 `GraphicsDriverSettings`，MOD 使用现有 `ModSelectionStore`。无新增驱动推荐或机型策略。

本轮没有实现上游的掌机/主机模式切换、任意 1080p/1440p 输出分辨率。渲染比例改变 3D 渲染工作量，不代表输出分辨率选择。系统驱动黑块不属于本轮修复范围。

## ZIP 内容与导入规则

原版操作目标为 `files/d2r-mobile/save`，MOD 为 `files/d2r-mobile/mods/<id>/save`。导出包含角色、共享仓库、Settings、隐藏元数据及其他文件；不导出 native 的 `.work`、`.new` 工作副本。

ZIP 根目录直接对应存档目录内容，无额外的 `save/` 外壳，例如：

```text
d2r-save.json
hero/hero.d2s
hero/hero.d2s.meta
Settings/Settings.json
```

说明文件 `d2r-save.json` 使用 `format=d2r-save`、`version=1`，并记录 `game_build=94012` 和 `scope=base` 或 `mod-<id>`。导入时说明文件不会写进游戏存档。结构上接受上游同格式但不含 scope 的 ZIP，也接受无说明文件的普通 ZIP；这不代表已经验证任意游戏版本、存档内容或 MOD 的语义兼容性。带 scope 的包拒绝跨原版/MOD 或跨 MOD 导入；无 scope 的包由用户根据预览选择正确目标。

导入覆盖相同相对路径的文件，保留其他角色和文件，不会用 ZIP 替换整个存档目录。“恢复备份”同样执行合并，不会删除备份后新增的角色。角色计数为 `.d2s` 文件计数，不解析角色数据。

导入先解压到独立 staging，确认前不修改存档。确认后复制并合并到同级 next 目录，自动备份旧目录，写 journal，再以原子 rename 发布新目录。文件和目录在提交边界同步；失败尝试回滚，进程中断后在下次传输或游戏启动前恢复/清理 journal。发布后的旧代清理失败会留待下次完成，不回滚到已部分删除的旧目录。

自动备份保存在应用私有目录 `files/d2r-mobile/save-backups/<scope>-<时间>-<UUID>.zip`，写完 `.partial` 并同步后才发布为 `.zip`。“导出上次自动备份”只查找当前模式的已完成备份。首次导入前如果没有旧文件则不生成备份。备份不会自动清理；卸载应用会一并删除私有备份，需要保留时应导出到系统文件选择器指定的位置。

拒绝路径越界、符号链接、重复路径、目录大小写别名和文件/目录冲突；限制单文件 32 MiB、全部文件 256 MiB、4096 个文件和 12 层路径。读取时检查 ZIP CRC。无效包提示失败，保留当前已提交存档。

存档传输从打开文件选择器开始锁定目标，与游戏启动互斥。选择器取消会解锁；选择、预览和后台任务跨 Activity 重建保留。游戏已启动时重新打开启动器会返回现有游戏；游戏 Activity 销毁后仍需彻底退出应用进程才能再次管理，避免异步 native 退出与文件操作重叠。进程被系统终止时需重新选择尚未提交的导入文件。

## 验证与交付

测试均使用合成数据和独立模拟器包 `org.d2r.launcher.qa`，不含真实角色、游戏客体或 native 游戏库：

| 检查 | 结果 |
|---|---|
| `LauncherSaveTest` | 100 项通过；ZIP 往返、覆盖/保留、备份、提交同步故障、恢复、CRC 损坏、解压大小限制、路径、符号链接、共享配置和启动互斥 |
| `LauncherTestInstrumentation` | 39 项通过；真实 Activity、SAF 流、设置持久化、旋转重建、预览确认、备份重发现及导出、MOD 隔离、系统选择器取消、横竖屏固定启动入口 |
| 原有资源安装与状态测试 | 40 + 21 项通过 |
| APK 打包单元测试 | 17 项通过 |
| i18n 与启动器译文生成检查 | 475 条统一文本通过；本轮新增 69 条，启动器无需加载 native 即可显示译文 |
| 原文字面量扫描 | Windows 下与 HEAD 基线输出完全一致，仍有原有路径分隔符相关失败，无新增发现 |
| Android Release native 构建 | 通过 |
| 实际 APK 构建 | 原签名、非 debuggable、版本 6 / `1.2.0-94012`；204 项负载校验通过 |

复现启动器主机/模拟器检查：

```powershell
python dm/android/tests/run_launcher_checks.py --output <新证据目录> --java <JDK17目录> `
  --sdk <Android-SDK目录> --emulator emulator-5580
```

省略 `--sdk` 和 `--emulator` 可只跑主机检查；脚本拒绝物理设备 serial。模拟器应为本任务专用实例，因为脚本会调整分辨率、密度和方向。QA 包不进入产品 APK；构建器将 Java 类放入一个 JAR 后交给 D8，避免 Windows 批处理命令长度限制。

本机证据位于 `D:/AndroidSwitch/analysis/launcher-functional-20261023/qa-release/`，含日志、`result.json`、竖屏存档区域和横屏启动器截图。预览 APK 位于 `D:/AndroidSwitch/output/releases/D2R-Mobile-1.2.0-94012-launcher-preview-20261023/D2R-Mobile.apk`。SHA-256：`99c2e768f1c1c09bb3278f4b76d546f500e1315f322d597f319791f6afeb0162`。

PPG 当前不在线，尚未进行真实角色导入后加载、战斗或驱动回归验收；TNA 未安装、未启动、未读取存档。原版/已安装 MOD 的真实角色加载验收需要在 PPG 连接后完成。此次源码提交不包含游戏内容、真实存档、签名材料或构建包。
