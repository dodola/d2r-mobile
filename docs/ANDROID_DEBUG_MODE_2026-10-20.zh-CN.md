# Android D2R 调试模式（第一版）

后续已增加系统菜单的地图一键全开和 MOD 选择入口，见 [MOD 与地图说明](ANDROID_MODS_2026-10-20.zh-CN.md)。启用 MOD 时 Debug 存档进一步隔离到 `mods/<id>/debug-save`；回执会显示实际模式对应的目录。

用于没有测试存档时准备道具、切换地图、恢复角色状态。实现位于 `dm/android/d2r_debug_mode.c`，命令工具位于 `tools/android/debug_console.py`。当前是代码移植及离线验证版，未执行原版游戏、未连接设备、未打包或安装 APK。

## 构建和启用

调试控制台默认不编译。沿用现有 Android 构建所需的 NDK、生成资源和私有适配配置，额外设置：

```text
-DDM_ANDROID_D2R_HOST_MODULES=ON
-DDM_ANDROID_DEBUG_CONSOLE=ON
```

仅接受 D2R 3.3.94012、Build ID `db0507f36b84200ac05f75d76ed5026f`，并要求原版服务器 tick 观察点存在。不支持其他游戏版本的偏移。普通构建保持 `DM_ANDROID_DEBUG_CONSOLE=OFF`。

编译后还须在启动前显式启用：应用私有 port root 下存在 `debug/enable`，或启动环境中精确设置 `DM_DEBUG_MODE=1`。运行时开关在本次启动内不变。开启后 `DM_SAVE` 指向 `debug/save`，首次为空，需要新建测试角色；工具不会复制正式存档。后台保存同步也使用该目录。关闭后下次启动恢复普通存档路径，调试存档保留。

下列命令在仓库根目录执行。`--package` 必须是实际安装的、支持 `run-as` 的 debuggable QA 包名；脚本默认 `org.d2r.binaryprobe`，其他构建可能使用不同包名。可通过 `--adb` 指定 ADB 路径，通过 `--serial` 选择设备。所有全局选项放在子命令前。

```powershell
# 正常退出游戏并停止应用之后启用，随后手动启动游戏、新建/进入测试角色。
python tools/android/debug_console.py --package org.d2r.binaryprobe enable

python tools/android/debug_console.py --package org.d2r.binaryprobe status
python tools/android/debug_console.py --package org.d2r.binaryprobe exec give r30 2 1 inventory
python tools/android/debug_console.py --package org.d2r.binaryprobe exec scene lut
python tools/android/debug_console.py --package org.d2r.binaryprobe exec heal
python tools/android/debug_console.py --package org.d2r.binaryprobe exec gold 10000

# 正常退出并停止应用之后关闭，下次启动使用普通存档。
python tools/android/debug_console.py --package org.d2r.binaryprobe disable
```

工具不会安装、启动或强制停止应用。`enable`/`disable` 会先检查应用是否正在运行；环境变量启用方式需要同时清除 `DM_DEBUG_MODE` 才能关闭。编译开关关闭时，标记文件不产生效果。

## 命令及范围

| 命令 | 行为和限制 |
| --- | --- |
| `give <code> [quality] [count] [inventory]` | 从当前角色版本的物品表校验 1–4 位字母数字物品码，调用原版掉落函数；默认普通品质、1 件，单次最多 16 件。`inventory`/`inv` 通过原版拾取包尝试入包。 |
| `scene <id或别名>` / `warp <id或别名>` | 请求基础游戏 1–136 号区域；经典角色拒绝大于 108 的区域。最多等待 30 秒，观察到目标区域且结束加载才返回成功。 |
| `heal` | 按原版属性读取和写入路径恢复生命、法力，并读取服务器值确认；不复活死亡角色。 |
| `gold <amount>` | 设置身上金币，范围 0 到角色等级 × 10000；不修改仓库金币。 |
| `status` | 命令工具直接读取当前回执；`exec status` 则由服务器 tick 处理一次状态请求。 |

品质参数：`0` 原版随机、`2` 普通、`4` 魔法、`5` 套装、`6` 稀有、`7` 暗金。实际结果受原版掉落规则约束，不能保证指定名称、词缀或品质；`give r30 2 1 inventory` 表示一枚 Ber 符文，其物品码是 `r30`。

场景别名：`rogue`/`camp`=1、`blood_moor`=2、`lut`=40、`kurast`=75、`fortress`=103、`harrogath`=109、`tristram`=38、`canyon`=46、`throne`=131、`worldstone`=132。换图不自动完成任务或解锁传送点，也不包含自定义或额外内容的场景。

物品按底材码生成，尚未实现“任意指定名称的暗金/套装”、精确词缀、孔数、无形、等级或技能点设置。下一步可复用 `dm/host/dm_rift.c` 的物品构造逻辑，增加经过表校验的唯一物品行号选择，再在同一命令层上加物品搜索和场景列表 UI；当前没有游戏内调试面板。

## 执行和回执

命令只在原版服务器 tick 中执行，位于其他 host 模块之后、存档互斥锁之外。通过检查角色、区域和窗口状态，拒绝在死亡、后台、加载/导航、暂停、手持物品或活动裂隙期间执行。界面重建、换角色或游戏轮次变化会更新 run token，旧上下文的命令失效。

应用目录为 `files/d2r-mobile/debug`。协议为单行 ASCII，包含换行在内小于 256 字节：

```text
<request-id> <run-token> <command> [arguments...]
```

使用工具自动生成请求 ID、读取 `status.json` 中的 run token，并通过临时文件原子改名提交 `commands.txt`。服务端先改名为 `processing.txt`，读入并删除后才调用游戏函数。不要手动批量写多行；同一时刻使用一个客户端，等当前命令返回再提交下一条。

`status.json` 原子替换，包含 `enabled`、`attached`、`ready`、`run`、`tick`、`area`、`request`、`state`、`detail`、`made`、`picked`、`target` 和 `save`。命令工具必须匹配自己的请求 ID 才接受结果。

- `ok`：服务器观察到命令的成功条件；不能替代画面、存档和触控验证。
- `partial`：生成或观察到的入包数量不足。背包满时道具可能留在地上；合并堆叠后无法确认原对象位置也会保守报告此状态。
- `transition`：换图已请求，尚在等待；`timeout` 表示未在 30 秒内观察到目标。
- `rejected` / `failed` / `io_error`：参数或状态不允许、调用结果不满足条件、或文件处理失败。
- `cancelled` / `interrupted`：运行上下文变化或上次请求被中断，需在游戏里核对实际结果。

相邻重复请求 ID 不重复执行。此机制不是持久化的 exactly-once 保证；崩溃可能发生在生成部分道具之后。脚本不会重试变更命令，超时或断连时结果未知，先检查游戏和回执。程序重启也不会重放遗留的 `processing.txt`。

当前每 200ms 检查请求并同步状态文件，模式开启有额外 I/O 开销。正式性能对比应关闭调试模式。

## 离线验证与后续设备检查

2026-10-20 的验证：

- C 行为夹具正常构建及 ASan/UBSan 构建各通过 478 项检查：参数、物品表校验、拾取包/处理标志、背包满与合并、暂停/死亡/后台保护、旧 token、多行/NUL/超长请求、FIFO/符号链接、写回执失败不变更游戏、换图等待/超时/取消。
- Python 命令工具通过 11 项测试：ADB 参数引用、stdin 传输、运行时拒绝切换模式、回执匹配、超时/提交失败不重试。全部使用 mock，不接触设备。
- Android ARM64 调试开关 ON/OFF 的共享库及存档探针构建验证；主机测试报告标明 `original_game_executed: false`。

复现命令：

```powershell
python -m unittest discover -s tools/android -p test_debug_console.py -v
wsl -d Ubuntu --exec bash -lc 'cd /mnt/d/AndroidSwitch/runtime/d2r-mobile && python3 dm/android/tests/run_debug_checks.py --output /mnt/d/AndroidSwitch/analysis/android-debug-20261020/host'
cmake --build D:/AndroidSwitch/android-build-debug-console --target d2r_mobile_android -j 6
cmake --build D:/AndroidSwitch/android-build-current --target d2r_mobile_android dm_android_d2r_save_probe -j 6
```

主机夹具替代了原版二进制函数，不能验证偏移对应的运行时行为、地图资源加载、实际装备品质、属性 UI 刷新或保存后的重入。拿到设备后依次验证：测试角色创建和独立存档、符文/药水/装备生成与拾取、背包满、同幕/跨幕换图、加载中拒绝命令、回血/金币显示、后台和重启后存档、关闭模式后正式角色不受影响。
