# Android MOD 加载与地图一键全开

本版在 `codex/android-integration-optimizations` 分支增加启动时选择的 MOD 资源覆盖包，以及系统菜单的“当前地图一键全开”。不依赖 Debug 控制台开关，但要求现有 D2R 94012 私有 UI 适配构建。当前仅完成代码、资源转换和离线验证，未连接设备，未验证实际游戏效果。

## 参考仓库与兼容范围

参考 [uuidd/D2RMod](https://github.com/uuidd/D2RMod)，分析版本为 `f64b5b2d7fe67a1c9f01a77fe546372bf84ae176`。该仓库提供的是 D2RMM 组件，`mod.js` 通过 `readTsv/writeTsv`、`readJson/writeJson`、`copyFile` 等 API 修改资源。它不是 Android 可直接加载的插件，也没有可以直接移植的 ARM64 地图揭示入口。

本地游戏通过 Switch 静态 CASC/TVFS 索引读取资源，直接把 PC 的 `mods/name/name.mpq/data` 放在 RomFS 目录不会自动替换 CASC 内文件。因此采用离线生成完整的新资源索引、启动时叠加新数据档的方式：

```text
D2RMM 导出数据 / 纯 copyFile 组件
    → mod_package.py，基于 Switch RomFS + 当前 Android 触控覆盖层
    → package.txt + package.json + overlay/data/.build.config + 新数据档
    → mod_console.py 安装到私有目录
    → 系统菜单选择，下次启动加载
```

| 输入类型 | 本版处理 |
| --- | --- |
| 已导出的 `name.mpq/data` 目录或 `data/hd、global、local` | 支持转换已有逻辑资源路径；仍须确认资源格式适用于 Switch 94012。 |
| 仅含静态 `D2RMM.copyFile('来源','目标',true/false)` 的组件 | `--copy-mod` 解析字面量并复制文件，不执行 JS；已用参考仓库的“跳过开头暴雪图标动画”生成实包。 |
| 使用 `readJson/writeJson/readTsv/writeTsv` 等 API 的 `mod.js` | 先在电脑端通过 D2RMM 生成最终数据，再导入。本版没有完整 D2RMM 执行器。 |
| `global/excel/*.txt` 表格 | 不会自动编译；必须同时提供匹配版本的 BIN，经过检查后使用 `--allow-tables`。PC 编译 BIN 不自动获得 Switch ABI 兼容性。 |
| `global/ui/layouts/*` 布局 | 默认拒绝；人工适配触控后可用 `--allow-ui`。工具不自动合并 JSON 布局。 |
| 原索引里没有的新路径 | 默认拒绝；`--allow-additions` 使用现有 CASC 工具有限的新目录插入能力，不支持所有目录组合。 |
| DLL、Windows 程序、注入器、加密 MPQ 包 | 不支持；导入器接收解包后的资源目录。 |

不批量启用参考仓库全部组件；特别是大背包、佣兵装备、新物品等，会影响共享原版适配器假设及存档结构，不能仅凭“转换成功”判断兼容。

## 生成与安装

电脑端安装 Python。准备与当前应用一致的完整 Switch 94012 RomFS，以及当前安装的 Android 触控覆盖层。打包过程中使用硬链接读取原始数据档，输出目录和原始资源应位于支持硬链接的同一文件系统；不复制数 GB 的原始资源。

下面是本工作区已验证的路径；仓库外的游戏资源及生成包不提交 Git：

```powershell
python tools/android/mod_package.py `
  --romfs D:/AndroidSwitch/analysis/parity-20261019/d2r-new/RomFS `
  --base-overlay D:/AndroidSwitch/analysis/parity-20261019/d2r-new/romfs_overlay `
  --copy-mod "D:/AndroidSwitch/references/D2RMod/(2)跳过开头暴雪图标动画" `
  --id skip-intro `
  --output D:/AndroidSwitch/analysis/android-mod-20261020/final/skip-intro
```

导入 D2RMM 已导出的资源时，将 `--copy-mod ...` 替换为 `--data C:/path/to/name.mpq/data`。ID 只允许 1–48 位小写字母、数字、下划线和连字符，输出目录必须不存在。导入顺序及组件冲突应在 D2RMM 导出阶段解决；Android 每次选择一个最终组合包。

有设备后正常退出游戏、停止应用，再执行以下工具。`--package` 改成实际 debuggable QA 包名，可加 `--adb`、`--serial`：

```powershell
python tools/android/mod_console.py --package org.d2r.binaryprobe install D:/AndroidSwitch/analysis/android-mod-20261020/final/skip-intro
python tools/android/mod_console.py --package org.d2r.binaryprobe activate skip-intro
python tools/android/mod_console.py --package org.d2r.binaryprobe status
```

安装到 `files/d2r-mobile/mods/skip-intro`，通过隐藏暂存目录传输，逐文件校验后改名发布。工具不替换已有包，更新内容用新 ID，避免覆盖已有 MOD 存档。工具不安装 APK、不启动或停止应用。当前会话没有运行以上设备命令。

启动游戏前，可在启动器展开“启动设置 → 选择已安装 MOD”，选择已安装包或“不加载 MOD”。启动器会显示本次驱动和 MOD 设置，MOD 选择损坏或目标包缺失时，可直接在入口重新选择或关闭 MOD。

游戏中仍可从“系统 → MOD → 选择 MOD（下次启动生效）”选择。应用运行中选择只保存下次启动选项，不热切换资源和存档；正常退出后重新启动生效。

若包损坏、触控资源版本不匹配或选择文件非法，启动在挂载存档前停止。可以停止应用后执行：

```powershell
python tools/android/mod_console.py --package org.d2r.binaryprobe disable
```

触控覆盖层或“物品注释”等资源变体变更后，必须按新的基础覆盖层重新生成 MOD 包。校验用 SHA-1 标识基础配置及运行时文件内容，工具另外记录 SHA-256；它们用于一致性检查，不是 MOD 作者签名。

## 资源和存档

加载顺序为 `MOD 的 overlay → 当前触控覆盖层 → 原始 RomFS 镜像`。新 TVFS 基于触控覆盖层的当前索引生成，未修改的条目原样保留；不会把原版索引直接放在触控层前面导致触屏资源丢失。主机端自检还会解码新条目并比较实际字节。

| 模式 | 存档目录（相对 port root） |
| --- | --- |
| 普通游戏 | `save` |
| 仅 Debug | `debug/save` |
| MOD | `mods/<id>/save` |
| MOD + Debug | `mods/<id>/debug-save` |

首次启用某个 MOD 使用空存档目录，需要创建测试角色。各模式不会自动复制角色或共享仓库；关闭 MOD 后回到原来的普通存档。后台同步沿用实际 `DM_SAVE`，Debug 回执也会标明 MOD 调试存档位置。

## 当前地图一键全开

进入角色并完成区域加载后，打开“系统 → 当前地图一键全开”。点击后关闭系统菜单，原版客户端输入线程排队执行；完成、取消或部分失败会弹出提示，菜单中可查看已处理房间数。此入口在普通构建也可使用，无需开启 Debug。

揭示范围是当前区域的全部房间，包括未走到的房间，使用本地 `dm_world.c` 已有的原版调用合同：初始化客户端房间，再调用地图对象的单房间揭示函数。没有把服务器房间或 UI 线程传给客户端地图函数。

每次输入轮询最多处理 8 间房，目标预算 1.5ms；单次原版函数本身无法中断，因此不是严格耗时上限。处理中重复点击不会再建任务；换角色、换区域、世界重建、进入导航/加载、后台、死亡、暂停、手持物品或活动裂隙时取消。超时为 30 秒，最多检查 4096 间房，异常链表或无效回调会终止。

这不是跨幕预生成全部世界：没有到达的其他区域不会被自动生成。功能不解锁传送点、完成任务或持续显示怪物；新游戏或其他区域可再次点击。地图线条、图标和原版探索记录的实际变化仍待真机确认。

## 验证

本次没有运行原版游戏、没有设备安装或画面检查：

- `run_mod_map_checks.py`：MOD 加载器 147 项、地图队列 136 项检查，正常与 ASan/UBSan 各通过一遍；加载器还读取了参考组件生成的真实包。
- Python 工具共 21 项测试通过，包括已有 Debug 工具 11 项、MOD 工具 10 项；覆盖索引/字节往返、保留未修改资源、触控路径替换、表格限制、非法路径、包损坏、静态组件解析和设备命令的 mock 检查。
- `ModSelectionStoreTest`：20 项 JVM 检查通过；全部 Android Java 源码通过 android-35 API 编译。
- 原有 Debug 正常及 ASan/UBSan 各 478 项检查通过；调试开关 ON/OFF 的 Android ARM64 共享库及存档探针编译通过。
- 实际参考组件包位于 `D:/AndroidSwitch/analysis/android-mod-20261020/final/skip-intro`；`package.json` 记录替换路径、输出摘要及未修改索引检查数量。不包含原始数据档。

转换过程中修复了共享 CASC 工具的一处编码规格读取错误：CFT 中保存的是字符串表的字节偏移（如 `z=0、n=2、b:64K*=n=4`），之前按字符串序号处理会在视频条目转换时报错。已补回归用例。

主机测试：

```powershell
python -m unittest discover -s tools/android -p "test_*.py" -v
wsl -d Ubuntu --exec bash -lc 'cd /mnt/d/AndroidSwitch/runtime/d2r-mobile && python3 dm/android/tests/run_mod_map_checks.py --output /mnt/d/AndroidSwitch/analysis/android-mod-20261020/host --package-root /mnt/d/AndroidSwitch/analysis/android-mod-20261020/loader-fixture --base-overlay /mnt/d/AndroidSwitch/analysis/parity-20261019/d2r-new/romfs_overlay'
```

拿到设备后优先检查：无 MOD 启动回归；示例包加载及独立角色保存/重入；停用后普通角色恢复；损坏包拒绝启动及停用恢复；野外/地城/城镇地图揭示；大地图处理时间；处理中切场景、死亡和切后台；多次点击及新局重试。其他 MOD 的内容兼容性必须逐个验证。
