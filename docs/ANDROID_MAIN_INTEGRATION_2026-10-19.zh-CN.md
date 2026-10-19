# Android 工作回归 main（2026-10-19）

本次将 D2R Android 的有效工作整合回 `main`。暂停 Zelda 移植；此前暂停的 iOS 功能对齐目标不在本次恢复，云同步继续低优先级。

## 分支处理

| 来源 | 处理 |
| --- | --- |
| `codex/d2r-offline-installer`，`ed20612` | 已包含在原 main `dd0e3c7`，不重复合并。保留离线安装器、启动图标、严格包校验和驱动选择。 |
| `upstream/main`，`9393b00` | 合并上游后续修复，包含 v1.0.18 的 guest_dylib 修正及 iOS 音频行为。 |
| `codex/android-measured-optimizations`，`ed0db1c` | 合并剩余管线缓存持久化及预热；复用 main 的驱动缓存与锁，保留异步翻译、驱动兼容及顶点格式处理。 |
| `codex/zelda-echoes-of-wisdom-android`，`b1546ff` | 仅转入该检查点内的 D2R Android/iOS 对齐工作，手工解决与 main 修复的重叠。旧历史存在补丁等价的重放提交，因此不整体 merge 此分支。 |

旧分支保留作历史参考。Zelda 分支的 Git 祖先关系不会显示为全部已合并；这不代表应继续合并其整条历史。新增 Zelda Activity、应用 manifest、应用构建 target 和专用 APK 打包工具未转入 main；main 原有、明确标为未完成游戏验证的独立探针资料保留。

## 主线约定与版本边界

- 后续 D2R Android 工作从 `main` 开始，避免继续提交到 Zelda 命名分支。
- 离线生产链 `tools/d2r-mobile-port/build_apk.py` / `build_offline_package.py` 仍绑定 **91738**。默认 CMake 构建不需要新版专用资源表，保留通用触控、手柄输入、事务存档、系统画质菜单和驱动选择。
- **94012** 私有场景、技能、物品及后台主动存档桥使用显式 `DM_ANDROID_D2R_HOST_MODULES=ON`，并要求匹配资源生成的 `DM_ANDROID_VAULT_TABLE`；完整 HUD 还需对应经验表与原生图集。运行时继续验证模块身份和锚点，拒绝把 94012 地址应用到 91738。
- `dm/tools/android_d2r_apk.py` 为 94012 独立诊断包流程；其模块、RomFS、overlay、字体、shader 与 HUD 素材必须匹配，不能混用 91738 离线资源包。未建立新的 94012 生产离线发布链。
- 91738 与 94012 的手动 3D 缩放分别验证地址/指令；默认请求 1.0，手动画质选择优先于动态降档。
- 保留 main 的 B/Back 消费、短按锁存、多设备合并、设置弹窗 B 关闭和启动日志轮转。完整取消快照同时保留其他设备的按钮及摇杆，不产生新的按键边沿或输入模式接管。

## 验证与限制

整合时对默认 baseline 与完整 94012 配置分别执行 Android NDK 严格编译和链接；全部 Android Java 源验证 API 兼容性。工具测试 57 项及缩放证据测试 3 项通过；管线缓存 key/profile/IO/store 覆盖正常和 ASan/UBSan。91738/94012 独立设备 CPU 缩放 fixture 分别通过 80/171 项检查。新控制器取消快照及触控桥安全回退设备 fixture 通过，仓库内新增回退目标含 47 项检查。输入 raw/logical 75/157 项、触控流程 753 项、触控 shell 372 项、技能流程 1,782 项设备检查亦全部通过。

主线 merge runner 全部通过，包含 4,463 项触控状态检查、73 项存档恢复检查、dirty/pacing/presentation 回归，以及 200 个真实 pthread 的生命周期测试（800 次分配全部释放，覆盖显式 detach 与线程退出）。整合同时修复了 TLS 视图改为独立 owner 后遗留的 Android 释放路径。

本次检查不代表整合版已完成原游戏端到端验收。设备游戏未替换，已部署基线仍见 [功能对齐记录](ANDROID_IOS_PARITY.md)。实体手柄硬件、94012 全部物品/技能流程、云同步和跨设备效果仍按该记录的未完成项处理。

本地整合收据：`D:/AndroidSwitch/analysis/main-integration-20261019/`；Java候选与API编译收据：`D:/AndroidSwitch/analysis/parity-20261019/main-integration/frontend-candidate/`。生成资源和游戏文件留在 checkout 外。
