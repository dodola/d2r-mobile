# Android 调试工作台与可选玩法 MOD

## 入口与交互

- 启动器设置增加 Debug 开关；游戏内「选项 → 自定义」也可切换。
- 开关在下次启动应用时生效。Debug 使用 `debug/save`，MOD + Debug 使用对应包的 `debug-save`；不迁移正式存档。
- 开启后游戏主画面左侧显示 DEBUG，打开独立全屏工作台。界面完全使用游戏内 Widget、ButtonWidget、TextBoxWidget、NineTileImageWidget。
- 工作台复用已注册的 Chronicle 面板生命周期，打开时隐藏其普通内容。编年史原本四个页签保持原有入口，Debug 不占用编年史页签。
- 左侧 9 个分类：暗金装备、套装装备、符文、宝石、武器底材、护甲底材、药水与杂物、场景跳转、调试工具。暗金 / 套装提供「全部 / 武器 / 护甲 / 饰品」筛选 chip；选中的分类与行高亮，暗金名称金色、套装名称绿色。列表与工具分别放在 ListGroup / ToolsGroup 两个分组，每页 6 项，列表行高 150、间距 180；底部显示选中项、分页和执行反馈。
- 物品目录（`catalog kind index`）只支持 kind 0=套装、1=暗金；Debug 页面与后端已移除符文之语生成（符文之语 MOD 本身不变）。
- 调试物品不再放入背包：暗金 / 套装 / 符文 / 底材以 descriptor mode 3 掉落在玩家身旁地面（落点来自 `0x5c84f0` + `0x6529e0`），并标记为已鉴定（data+0x18 置 0x10）。
- 添加物品 / 跳转场景在返回游戏后由服务器线程执行；单次排队、超时与换局取消保护继续保留。
- 调试工具包括地图揭示、生命法力恢复、金币。金币按钮每次增加 100,000（`gold add N`，携带金币总额限制在 10,000,000 以下）。
- 开启 Debug 时附带实时低价商店（`d2r_debug_shop.inc`）：对已加载的物品表（692 行，BIN 记录布局，价格在 +232）把全部物品价格置 1，并把 MOD 的商品清单（符文 r01-r33、完美宝石、rvl、toa、jew、pk1-3 含钥匙类型）加入所有商人；每张表只处理一次，并抽查 hp1。设备日志：`ANDROID_DEBUG_SHOP version=3 rows=692 cheap=685 stocked=46`，Akara 的药水价格显示为 1。

## 可选 MOD

商店、符文之语及组合包各提供普通资源与物品注释资源版本。选择列表只显示与当前基础资源 SHA-1 匹配的包；底层继续校验包内容完整性。UI 资源变化后必须重新生成 MOD，不能只修改 descriptor。

不兼容或损坏的启动选择不会挂载存档，也不会 abort，游戏页显示错误，返回启动器重新选择即可。大箱子沿用现有 14×10 格与多页仓库实现。

## 本次验证

- 原生 Android 编译通过；Debug 主机测试正常 / ASan+UBSan 各 551 项通过。
- MOD 选择与开关持久化主机测试、启动器存档回归、MOD 加载与地图请求主机回归通过；MOD 已针对新 UI 资源重新生成（`analysis/debug-ui-20261023/mods-r6`）并逐个校验。
- 实机（PPG，r10-r14）已验证：入口点击打开页面；暗金生成带真实属性并可保存。
- 实机发现并修复：TouchControlsView 覆盖层吞掉游戏内触摸（GameActivity 设置 `touchControls.setOnTouchListener`，先走 frontendTouch）；工作区由 UI pump 按宽高比居中；命令 index 上限恢复为 2047。
- 最新本地 APK：`D:/AndroidSwitch/output/releases/D2R-Mobile-1.2.5-94012-debug-workspace-r16-20261023`（r16，在本文上次更新之后构建，尚未实机验证）。
- 证据与脚本在 `D:/AndroidSwitch/analysis/debug-ui-20261023/`（`package-ui-r16.py`、`rebuild-mods-r6.py`、`deploy.sh`）。

### 仍未验证

- 地面掉落的可见性与拾取；套装物品；金币超过 等级×10000 后存档重载是否被游戏接受。
- 有效 MOD 启动；错误包拒绝页；商店 MOD 购买流程。
- 切后台后的 Vulkan surface-lost abort 未修复，与 MOD 包兼容性拒绝分开跟踪。
- 多语言：新增文本仍为中文硬编码。
- 普通非 debuggable 离线 APK 尚未生成。
