# Android Debug / MOD / UI 重设计交接（2026-10-23）

> 用户要求暂停，先整理交接。本文区分源码已实现、主机验证、设备已安装和实机未验收；不要把构建成功当成实机通过；最新本地构建为 r16（未实机验证）。后续需用户恢复工作后再继续开发或操作设备。

## 1. 工作目录与约束

- 当前工作树：`D:/AndroidSwitch/runtime/d2r-mobile-ppg-slots-20261023`
- 分支：`main`
- HEAD：`573106f3f243cbcb948e5d4029650193390600c4`
- 本轮 Debug、MOD、UI 改动全部尚未提交、尚未推送。不要覆盖或清理这些未提交/未跟踪文件。
- 另一个工作树 `D:/AndroidSwitch/runtime/d2r-mobile` 有其他改动，不要混用。
- 用户要求使用游戏内置 UI；减少截图，优先日志；不使用子代理。
- PPG 是验收设备；之前跳过最终验证只针对 TNA。
- 用户最后指令：暂停，整理交接文档。没有授权本轮额外 commit / push。

## 2. 用户目标与当前范围

已授权集成原生游戏 Debug、可选商店 MOD、符文之语 MOD，沿用现有大箱子。

最新 UI 要求：设置增加是否开启 Debug；开启后主界面左侧出现按钮；点击进入独立全屏 Debug 页面；按钮加大，改善触摸操作。

用户同时反馈开启 MOD 会崩溃，已澄清是「启动游戏的时候闪退」，尚未明确具体 MOD 名称。

Debug 和 MOD 均保持独立存档：

- 正式：`files/d2r-mobile/save`
- Debug：`files/d2r-mobile/debug/save`
- MOD：`files/d2r-mobile/mods/<id>/save`
- MOD + Debug：`files/d2r-mobile/mods/<id>/debug-save`

开关在下一次启动应用生效，运行中不切换存档挂载，不自动复制正式角色。

## 3. 已实现源码

### 3.1 Debug 后端与初版集成

- `dm/android/d2r_debug_mode.c/.h`：next-boot marker、原生 UI 请求 API、单条命令队列、run token、30 秒超时、换局取消、暂停时等待；新增 `catalog kind index [base-class]`。
- `dm/android/d2r_debug_items.inc`：原版服务端物品生成、强制暗金/套装条目、符文 / 底材生成；物品不再放入背包，而是以 descriptor mode 3 掉落在玩家身旁地面（落点 `0x5c84f0`+`0x6529e0`），并置已鉴定标记（data+0x18 的 0x10）。目录仅支持 kind 0=套装、1=暗金；符文之语生成已从 Debug 页与后端移除（符文之语 MOD 不变）。新增 `d2r_debug_shop.inc`：见 3.3.1。
- `dm/android/CMakeLists.txt`：`DM_ANDROID_DEBUG_CONSOLE` 默认 ON，并编译 `d2r_debug_ui.c`。
- Manifest / APK 工具默认版本：versionCode 11，versionName `1.2.5-94012`。
- APK 打包器新增重复 `--mod-package`；校验 MOD 包和对应基础覆盖层，打包只读资源。
- GameActivity 允许安装内置 MOD 的 package.json / package.txt / overlay/data，存档和 active selection 不包含在 APK 资产中。

**物品生成的重要约束**：编年史预览物品没有真实暗金随机属性，并有 display-only flag，因此已删除“序列化预览物品进存档”的临时方案。当前代码用原版服务端生成器。PPG 实机（r10-r14）已验证暗金生成带真实属性并可保存；地面掉落的可见性尚未实机验证，套装物品也未验证。

### 3.2 当前全屏 UI 重设计

- `dm/ios/tools/debug_layout.py`：完全使用原生 Widget / ButtonWidget / TextBoxWidget / NineTileImageWidget。
- Debug 不再占编年史第五页签；编年史仍为暗金、套装、符文之语、首领四页。
- 独立工作台借用已经注册的 ChroniclePanel 生命周期；Debug 打开时隐藏普通编年史内容，关闭后恢复；不是新建 Android 对话框。
- 黑色全屏背景，居中 3060×2040 工作区，按游戏 2160 高坐标系布局。
- 左侧 9 个分类（暗金装备、套装装备、符文、宝石、武器底材、护甲底材、药水与杂物、场景跳转、调试工具）；暗金 / 套装有「全部 / 武器 / 护甲 / 饰品」筛选 chip；选中行与分类高亮，暗金名金色、套装名绿色；ListGroup / ToolsGroup 分组布局；每页 6 条，列表高 150、间隔 180；分页、选择说明、添加 / 跳转、地图工具、恢复与金币操作。金币按钮每次 `gold add 100000`，携带金币总额上限低于 10,000,000；游戏是否接受超过 等级×10000 的金币并保存重载，尚未验证。
- 工作区由 UI pump 按宽高比居中；命令 index 上限恢复为 2047。
- 原生图片按钮在加载时忽略 width/height 并按 sprite 尺寸设边界，当前 UI pump 把按钮实际边界改为父容器尺寸；边框与文字为兄弟层，避免一起拉伸。
- 主画面左侧入口复用 HelpButtonPanel，Help 原按钮移出屏幕。原版在控制器模式会隐藏 HelpButtonPanel；r7 已在 UI pump 按 in-game 状态恢复父容器可见性。
- `d2r_debug_ui.c` 包含 `ANDROID_DEBUG_UI` 入口、分类和操作日志。

**输入修复（r8 引入；r10-r14 实机已验证入口点击可打开页面）**：

r7 左侧 DEBUG 能显示，但点击被世界触摸路径吞掉。r8 起新增：

- `dm_android_debug_ui_touch()`：Android 输入线程只读取原子发布的入口区域 / 页面打开状态；入口抬起时排队打开，带移动取消；页面打开时转入原有 `dm_ui_touch` 原生鼠标队列。
- UI pump 发布宽高比及入口可点状态，原子交换 open_request。
- `d2r_ui_adapter.c` 在 in-game 时允许这条受控原生 UI 输入路径。
- GameActivity 先尝试 frontendTouch，再进入虚拟手柄 / 世界输入；并为 TouchControlsView 设置 `setOnTouchListener` 先走 frontendTouch（该覆盖层原先吞掉游戏内触摸，实机发现并修复）。
- JNI `nativeDebugUiOpen()`；状态轮询中工作台打开时隐藏 Android 虚拟手柄层。

仍需继续检查：入口点击、全屏按钮点击、返回、取消触摸、多指、角色切换、普通游戏触控和编年史是否回归。

### 3.3 Debug 开关

- 启动器设置增加 Switch，读写现有 `debug/enable` marker。
- 游戏内「选项 → 自定义」追加 Debug 行，通过原生按钮切换 marker，动态显示状态。
- `dm_custom.c` / `dm_custom_ui.inc` 末尾追加 K_DEBUG，不改变原有索引；iOS 不显示 Android 新行。
- `ui_text.tsv` 添加开关文本。
- 当前部分新增提示仍为中文硬编码，需要后续补齐多语言；不要宣称国际化完成。

### 3.3.1 实时低价商店（`d2r_debug_shop.inc`）

Debug 开启时，对已加载的物品表（692 行，BIN 记录布局，价格在 +232）把全部物品价格置 1，并把 MOD 商品清单（符文 r01-r33、完美宝石、rvl、toa、jew、pk1-3 含钥匙类型）加入所有商人；每张表只处理一次，并抽查 hp1。设备日志：`ANDROID_DEBUG_SHOP version=3 rows=692 cheap=685 stocked=46`，Akara 药水价格为 1。商店 MOD 的购买流程尚未实机验证。

### 3.4 可选 MOD 与大箱子

新工具 `tools/android/build_gameplay_mod.py` 只处理从 Switch 94012 提取的原生 BIN，并逐字段对照相应 TXT，不使用 PC BIN 编译或任意 D2RMM JS。

- 商店：符文 1–33、完美宝石、恢复药、传送门卷册、珠宝、钥匙等；对应 Akara / Drognan / Ormus / Jamella / Malah。
- 符文之语：武器类型扩展至 weap、圣骑士盾扩展至 shld、可解除非天梯限制。
- 当前没有实现护甲跨盾 / 头盔的任意互换。
- 同时覆盖 `global/excel/*.bin` 与 `global/excel/base/*.bin`。
- 大箱子沿用现有 14×10、5 个个人页及已有 shared/vault；没有增加新容量或叠加 PC 大箱子布局。

## 4. MOD 崩溃排查与修复边界

### 已确认存在的启动终止路径

`app_jni.c` 原先对 `dm_android_mod_configure()` 失败直接 die/abort。任意选中与当前 item_notes 资源不同 SHA-1 的包会走到此处，用户感受为闪退。

本轮修改：

1. Java ModSelectionStore 只列出与当前 item_notes / base 覆盖层 `.build.config` SHA-1 相同的 MOD。
2. 游戏内 MOD 列表也通过新增 `dm_android_mod_matches_overlay()` 做对应筛选。
3. 底层完整包校验仍保留，启动失败时停止启动、显示明确 phase 错误、保留选择，不挂载存档，不直接 abort。
4. 所有 6 个内置包针对重设计后的覆盖层重新生成，不能只改 descriptor 的 base hash。

**不能说已完成用户崩溃实机复现/修复验收**：目前尚未在 PPG 上选择有效 MOD 完成启动；也没有故意选择错误资源版本验证错误页。

### 实机发现并已修复的 Java 路径问题

r4 测试包打开显示“游戏文件准备失败 / Symbolic settings path”。原因是 Android `/data/user/0` 系统别名进入新包兼容性检查的路径验证。ModSelectionStore 读取 descriptor 前 canonicalize 已知包目录，r5 后已正常进游戏。正式可信根处理仍由 LauncherSettingsStore 负责。

### 另一个尚未修复的问题

崩溃日志还记录两次切后台后：`STOP swapchain acquisition (-1000000000)`，在 `nvn_vk.c` 的 mtl_present / vkAcquireNextImageKHR 后 die，值为 surface lost。与 MOD 校验拒绝不是同一问题，本轮未改渲染生命周期，不能宣称修复。

证据目录：`D:/AndroidSwitch/analysis/debug-ui-20261023/mod-crash/`，有 crash.txt、startup.log、previous.log、logcat.txt、mod.txt。当前 active.txt 捕获时为空，具体触发启动崩溃的包名称没有从该状态恢复出来。

## 5. 设备状态与备份

- ADB：`C:/Users/dodol/AppData/Local/Android/Sdk/platform-tools/adb.exe`
- PPG serial：`AVAY025422000548`
- 包：`org.d2r.mobile`
- 启动器：`org.d2r.binaryprobe.InstallerActivity`；游戏：`org.d2r.binaryprobe.GameActivity`。
- 设备已更新到后续构建（r10-r14 做过实机验证）；r16 为最新本地构建，未安装验证。以设备实际安装包为准，先查 SHA。
- Debug 开启，MOD active.txt 为空；入口点击、暗金生成已在 r10-r14 验证。
- 用户创建的 Debug 角色日志名为 yyy；之前正式角色为 ii。不要把调试角色当正式存档。
- `before-install-save-settings.tar`：初版安装前备份，70278 字节。
- `before-ui-r4.tar`：重设计前备份，23205376 字节，包含 save、debug、mods、sw_custom.txt。
- r5 / r6 后的安装前曾按 Home，并检查 lifecycle_focus=3、background_save_done_epoch=lifecycle_epoch=3 后才更新。
- focus=3 是后台；不要再用 focus=0 作为后台保存条件。

## 6. 构建与产物

构建目录：`D:/AndroidSwitch/build/android-ppg-slots-20261023`

最新本地 APK 目录（r16，本文更新后构建，未实机验证；APK 文件名与下列哈希为 r8 记录，需对 r16 重新核对）：

`D:/AndroidSwitch/output/releases/D2R-Mobile-1.2.5-94012-debug-workspace-r16-20261023`

- r8 APK SHA256（旧）：`9a05ebb665d8821b9ff9455bd6909775ded28d2418b5afafb71ca1a5d34a9966`
- r8 Native SHA256（旧）：`18dfc979ae43c7205cc5b778a7450879de4436aaeb401b9ccc7e3439ab56b978`
- 证书 SHA256：`db6a9e98d27f4037df39b284699fef125ff83f6e92d6631ea250ce98ac747d0d`
- r8 验证 228 个 payload。
- 所有版本沿用 1.2.5-94012 / code11；r4–r16 只是本地产物目录，不是正式发布版本号。
- 当前还没有此重设计版本的普通非 debuggable 离线发布包。

旧设备 r7 APK SHA256：`8169db407c264b45b66469c6b5ca021b28fd4e78d1d7da3077f9ac1dabde39dd`。

资源与证据根：`D:/AndroidSwitch/analysis/debug-ui-20261023`

- `romfs_overlay`、`romfs_overlay_variants/item_notes`：当前新 UI 覆盖层。
- `mods-r6/`：当前重新生成的 MOD；旧 `mods/`、`mods-r4/` 不匹配当前新 UI，不应打入新包。
- `shop-data`、`runewords-data`、`gameplay-data`：MOD 输入。
- `source/`：提取的原版 TXT / BIN，仅分析使用。
- `generate_overlay.py`、`rebuild-mods-r6.py`、`package-ui-r16.py`、`deploy.sh`：本地构建辅助脚本。
- `package-ui-r8.log`、`package-ui-r4-command.json`：最终构建日志和最近脚本写入的完整参数。注意 command.json 文件名沿用 r4，内容实际由最后脚本覆盖，核对 --out。
- 生成覆盖层复用已验证 HUD artwork；原仓库缺失 oldnames.tsv / comments.tsv，生成器有相应警告，与之前原始状态一致。

## 7. 已完成验证

| 检查 | 结果与限制 |
|---|---|
| Android 原生构建 | r16 构建，包含最新输入路由 |
| APK Java / 签名 / 资产 | r8 通过，228 payload；r16 以构建日志为准 |
| Debug 主机正常 / ASan+UBSan | 各 551 项 |
| MOD loader 正常 / ASan+UBSan | 各 148 项，mod-map-r5/result.json |
| Map request 正常 / ASan+UBSan | 各 222 项，同上 |
| ModSelectionStoreTest | 26 项，包括不匹配包拒绝、Debug 开关持久化；r7 前重跑通过 |
| LauncherSaveTest | 100 项，launcher-r4/result.json |
| MOD 新包构建 | 6 包全部 verify_package，基于新资源构建 |
| PPG r5–r7 原版 + Debug | 启动及进入独立角色成功 |
| PPG 左侧入口显示与点击 | r10-r14 已确认，点击可打开页面 |
| PPG 全屏页面 | 入口打开已确认；其余交互未完整验收 |
| PPG 有效 MOD 启动 | 未验收 |
| PPG 暗金生成属性及存档 | r10-r14 已验证（真实属性，存档保留） |
| PPG 地面掉落可见性 / 套装物品 | 未验收 |
| PPG 低价商店 | 日志已确认 version=3 rows=692 cheap=685 stocked=46，Akara 药水价 1；购买流程未验收 |
| 错误包拒绝页、切后台 surface-lost、多语言、普通离线 APK | 未验收 / 未修复 |

未跑完整 Android 模拟器 instrumentation；已修改其合成 MOD fixture 以适配 descriptor 筛选，但不应写成模拟器通过。

## 8. 恢复后按顺序推进

1. 先核对工作树、设备状态、用户是否正在手动操作。按日志优先流程保留备份；有游戏运行则正常保存/后台保存确认再安装。
2. 安装并实机验证 r16，不要无改动重复构建。启动 Debug 角色，确认入口、地面掉落可见与拾取、金币。
3. 检查全屏遮罩、分类、6 行列表、分页、选中说明、操作与返回；仅必要时截图。检验普通编年史恢复，虚拟手柄只在调试页隐藏。
4. 检查 r8 触摸取消：当前 phase=3 分支优先处理 captured，打开页时没有显式 dm_ui_touch(3)；需要实测/修正以避免遗留按下状态。还需检查从暂停页开关、多指手势与游戏退出后 opened 状态清理。
5. 检查入口区域换算依赖 `main+0x211da38/+0x211da3c` 的宽高，确认与 viewport 归一化一致；竖屏/翻转/其他比例不能只靠 PPG 默认横屏推定。
6. 修复/补测 MOD：选 item_notes 匹配包启动；再确认错误包被筛除及底层错误页不会 abort。当前六包的 base hash：base `2b03d0062f1b40f53e367c2d0a408542bfebf290`，notes `7b031da638bf15e927e1746a3b45c85f2af7d836`。
7. 在独立 MOD/Debug 角色验证商店商品、购买、符文之语 MOD、保存退出重进；不动正式存档。
8. 物品服务端生成器：暗金已验证；还需验证套装、地面掉落、金币超限存档、失败清理、保存重进。不能只看返回 ok 或图标。
9. 最后再决定补齐多语言、整理源码注释、生成普通离线 APK。更新本文和功能说明中的验收结果，用户明确要求后再 commit / push。

## 9. 常用命令

```powershell
& 'C:/Users/dodol/AppData/Local/Android/Sdk/cmake/3.22.1/bin/cmake.exe' --build D:/AndroidSwitch/build/android-ppg-slots-20261023 --target d2r_mobile_android -j8

wsl -d Ubuntu --exec bash -lc 'cd /mnt/d/AndroidSwitch/runtime/d2r-mobile-ppg-slots-20261023 && python3 dm/android/tests/run_debug_checks.py --output /mnt/d/AndroidSwitch/analysis/debug-ui-20261023/host-next'

& 'C:/Users/dodol/AppData/Local/Android/Sdk/platform-tools/adb.exe' -s AVAY025422000548 shell run-as org.d2r.mobile cat files/d2r-mobile/diagnostics/status.json

& 'C:/Users/dodol/AppData/Local/Android/Sdk/platform-tools/adb.exe' -s AVAY025422000548 shell run-as org.d2r.mobile cat files/d2r-mobile/debug/status.json
```

启动日志：`files/d2r-mobile/diagnostics/game-startup.log`；上一轮为 game-startup.previous.log。用 Python subprocess 保存原始 bytes，避免 PowerShell 重定向破坏 UTF-8 或二进制。

## 10. 服务端二进制调用参考（94012 专用）

Build ID：`db0507f36b84200ac05f75d76ed5026f`。

- 真生成器 `main+0x656780`，descriptor 大小 0xd0：player+0、game+0x10、level99+0x18、class+0x20、mode0+0x24、u16 one+0x38、difficulty+0x3a、force+0x3c、quality+0x40、record+0x50、seed+0x58。
- unique table+0x13e8 / count+0x13f0 / stride0x15c，set+0x13c8 / count+0x13d0 / stride0x1cc；code+0x28。
- runeword table+0x1880 / count+0x1888 / stride0xe8，complete+0x44、id+0x46、runes+0x5c。
- sockets `0x5754a0`，rune mode `0x5ca8a0`，insert `0x64d080`，matcher `0x576310`，需核 exact recipe 与 flag0x04000000。
- 背包空位 `0x55f620`，放入 `0x6f1940`，通知 `0x642f70`，最后参数 uint64_t；契约参考现有 dm_vault.c。
- 预览 `0x367890` 仅用于获得符文之语适配底材；及时销毁，不能作为成品保存。
- 可离线反汇编：`D:/AndroidSwitch/analysis/hud-custom-20261021/images/main.patched.bin`，偏移与地址一致。
