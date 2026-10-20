# Android 与 iOS 功能对齐记录

2026-10-21 补充：上游四项语言设置已合并；Android 常驻系统／模式／额外菜单按钮已移除，改为“选项 → 自定义 → Android 设置”，FPS 默认隐藏。实现、保存语义与本次验证范围见 [HUD 与自定义设置](ANDROID_HUD_2026-10-21.zh-CN.md)。下文历史设备记录保留其各自构建的验证范围。

更新：2026-10-19。本文对应 D2R 3.3.94012 的 Android 实现检查点。Android 已接入更多 iOS 同源模块，但尚不能宣称全部功能和呈现都与 iOS 一致；Android 平台云同步和 Zelda 移植均明确停止。

本次为阶段性代码检查点。Android 平台云同步明确不实现，相关 SAF/JNI/原生事务接线、界面入口、测试和构建目标已删除；iOS iCloud 继续独立保留。Zelda 移植也已彻底停止，专用 Activity、探针、profile 和工具已移除。已部署和完成原游戏验证的基线仍为文末 batch3；该检查点当时的编译记录不等于本次整合后的验证，主线边界见 [回归 main 记录](ANDROID_MAIN_INTEGRATION_2026-10-19.zh-CN.md)。

## 如何理解验证状态

| 状态 | 可以说明什么 | 不能据此推断什么 |
| --- | --- | --- |
| 同源代码接入 | 模块已编译、链接，所需 pump、observer、事件或服务端 tick 已连接 | 游戏资源、原版对象和 Android 界面已一起跑通 |
| ARM fixture | 测试在真实 Android ARM64 设备运行；可能调用真实宿主库，也可能包含真实源文件并替换原版函数 | 已连接实体手柄、已操作真实角色、已完成原游戏存档 |
| 原版函数/资源 fixture | 执行了实际游戏函数或处理了实际提取的资源 | 整个游戏操作流程或所有同类资源都已验证 |
| 原游戏端到端 | 指定 APK、游戏产物、设备和场景下实际执行了完整操作，并留下结果 | 其他版本、设备、场景也一定成立 |

“设备已经连接”仅表示可以部署和测试，不等于实体手柄硬件或真实存档已经通过。本轮独立 fixture 使用临时目录和合成对象；不会把 fixture 的提交成功计作原游戏完成存档。

README 中 2026-10-15 的选角、镇内、近战、第一幕往返、存档退出和冷启动记录属于之前的 Android 构建，不能直接覆盖本轮上游新产物与完整 UI 的验证。

## 功能矩阵

| 功能 | 当前 Android 实现 | 已有证据与剩余差异 |
| --- | --- | --- |
| 实体手柄输入 | Java `KeyEvent`/`MotionEvent` 与独立物理输入通道已接入 Npad；支持按键、十字键、扳机、双摇杆。按设备合并状态，移除一只手柄保留其他设备；摇杆采用与 iOS 一致的 25%/15% 滞回，触摸接管后须回中再交还 | Java 状态测试与 Android 逻辑输入 fixture 已通过；尚不能据此宣称真实连接手柄的按键映射、蓝牙/USB 重连和原游戏操作都已通过 |
| 手柄/触摸模式 | 接入 `dm_padmode`、页面/符文之语/技能组过滤；最终过滤在输入锁外执行，再发布实际交给 guest 的摇杆值。输入代数变化清除延后按键及 L 边沿状态 | UI fixture 覆盖切换门、四轴、自动键、重置/断连后的首个 L 和延后按键取消；完整布局切换还依赖匹配 overlay 和原游戏验证 |
| 触控状态机 | 复用 `dm_touch_controls`，包含移动、技能拖动瞄准、回城、药水、难度、动态命中及取消语义；Android 负责事件传输、窗口坐标与绘制。D2R renderer 按 iOS 参数绘制技能嵌槽、空槽与浮动摇杆，按 enabled_targets 隐藏按钮；药水/菜单保留命中区并交给底栏美术 | 通用输入 fixture 已验证事件确认、取消代数和坐标；D2R Java 已通过编译和同源资源校验，最终包常态画面、触摸移动和菜单/选项页面已实机验收；死亡及全部按下状态仍待逐场景验证。其他游戏保留通用圆钮 |
| D2R 技能绑定与瞄准 | 接入 iOS 同源 `dm_touch_game_*` observer、六槽身份/反馈状态和 0x86 原版方向消费者；安全读取使用 `process_vm_readv`。松开瞄准会恢复临时目标及技能记录 | 真实宿主库 ARM fixture 覆盖原函数先调用、调用点/玩家守卫、零轴、按下/按住、距离策略和锥选怪恢复；实际技能与特效仍须游戏内验证 |
| 技能图标 | Android 原生 Canvas renderer 已接入同源技能图集及五个技能键的普通/按下帧；0x85 仅在 Java 实际 `drawBitmap` 后、250ms 内且场景/世界/HUD/绑定身份/技能组/技能匹配时隐藏原版图标。已按 iOS 在手柄／触屏布局重建完成后递增世界代数，重新确认六个 HUD 控件 | 最新 schema 2 真实动态库 HUD fixture 通过 317 项检查，覆盖五键正向隐藏、原生身份守卫、仅写 x8、重建后两轮重新确认及旧绘图凭据失效；同一回归在旧库上失败。旧 schema 1 无图集配置另有 16 项历史回退检查。最新统一包已在原游戏确认布局往返后的真实主技能、空槽、回城、地图及底栏画面；fixture 的绘图回执仍为受控输入 |
| 面板与鼠标桥 | `cursor/actions/world/cursor_item/uiscreen` 已接入；从真实控件树读取面板、腰带、持物、堆叠与鉴定状态，处理 UI 消息及触点事件 | 最新 UI fixture 通过 55 项检查，含失效控件重验证、归一坐标、消息虚表投递顺序、滚动/滑块消息及事件确认。fixture 虚表使用合成接收器；最终包另有真实触屏打开菜单及直接点击选项页签的原游戏证据，不覆盖全部 handler |
| 拖放、滚动与滑块 | Android 外壳已补按下点异步分类、`dm_ui_drag_begin/move/end`、抬起落点查询和释放等待；商人/堆叠拖出按背包落点成交，最终坐标与物理抬手时刻保留。成交/放置须取得最终 UP 点的查询答复，250ms 超时回原位，商人/堆叠不成交 | 真实 Android 传输与共享触控源文件 fixture 通过 753 项检查，覆盖查询、最终落点、释放等待、滚动/滑块、快速滑动的首帧交还、商人/堆叠目的地及 UI/world/input 代数取消；另覆盖历史成交结果不能授权新 UP 点、无 MOVE 的终点变化、发布查询时抬手及迟到答复。原游戏物品与交易流程仍需端到端验收 |
| 提示框、小地图与商店跟手图 | Android shell 已接松手后的光标停靠、面板默认腰带焦点让位和小地图先问世界目标的策略；Vulkan 已按货架格网与触点识别原物品 draw，在 pass 末使用原 shader/纹理与不可变顶点/UBO 重放，源货架按 82% 黑色遮暗。堆叠仅剩一件时遮暗；物理 UP/取消的本地 atomic guard 立即撤销旧绘制 | shell ARM fixture 372 项检查通过；真实 NVN/Vulkan GPU fixture 使用原离线 shader 与合成纹理/几何，通过 17262 项检查，包括 256 像素平移、遮暗、物理 UP、CPU 数据复用、原采样计数保持及不重新绑定状态的后续绘制。没有截图或通用物品替图；仍须最终包验证真实商人物品、堆叠和面板场景 |
| 技能与属性触控外壳 | 已补 `bindTapSync`、点击空主技能及两组四技能位选技能、技能树长按绑定、同目标二次点击加点、属性长按连续加点。已绑定战斗技能按住保持持续施放；首点技能/属性只选中 | 设备共享源/传输/生成图集规则 fixture 通过 1782 项检查，含 drag 分类完成后再释放的组合流程。真实宿主库额外 20 项检查核对命中控件、当前焦点和真实技能号，拒绝旧快照/跨代同地址；所有 guest 读取/请求在游戏线程。它们仍是受控对象测试，不等于原游戏绑定或消耗角色点数已验收 |
| 世界目标与物品动作 | 同源世界点选、拾取、长按、使用、鉴定、页签、腰带与堆叠动作已连到主线程；0x81 屏蔽非摇杆点地移动，保留目标交互及允许窗口 | policy fixture 覆盖 8 个移动分支；实际 NPC、出口、尸体、物品拖放和所有面板仍待逐项原游戏验证 |
| 自动药水与满包拾取 | 同源自动拾取与腰带整理已接入。服务端收包时再次检查腰带；自动药放不进就留地上。符合条件的手动拾取可改为原版“拾到方块”命令 | policy fixture 覆盖 6 个药水和 11 个方块分支，检查重写后的全部 29 字节；不等于真实战斗掉落场景已通过 |
| 高价值掉落震动 | 同源掉落判档/最高档原子队列接入独立 JNI 与 Android `Vibrator`，支持重/中/轻、预定义效果能力检查及分档单次效果回退；同源已应用的 `drop_haptic` 设置与开发强制开关共用同一判断 | 真实动态库开/关各 10 项、实际共享源文件 24 项与强制开/关各 5 项、Java 生命周期 23 项通过。后台/失焦取消且清队，恢复不补播；尊重系统触感反馈设置，未请求绕过标志。ROD 系统触感反馈当前为关，尚未确认实际系统效果调用或原游戏掉落体感。`nn::hid` 持续游戏震动与 iOS 一样未提供设备 |
| HUD 数据与数值绘制 | tick observer 发布区域、等级、生命/法力、腰带、回城、点数、任务提示、持物与人数；经验千分比依赖匹配游戏的生成表。JNI schema 2 复制 107 words，追加回城 glyph、导航/难度展开/取消/技能组按住/可见掩码与浮动摇杆原点。Canvas 复用同源底栏、菜单四态、药水数量、难度 1–8 选择条及 HP/MP/Lv/经验字图；技能图用 .76 直径比例、银灰、按下帧及副手/充能角标 | 合成游戏/单位/属性 fixture 已验证读取与失效清理；缺经验表时不伪造中间等级百分比。Java 编译和 97 项资源结构/哈希检查通过；绘制完成回执仍只包括实际画出的五个原生技能，不把回城算进隐藏授权。最终包常态画面已集中截图确认，全部 UI 状态仍需逐场景验收 |
| 自定义选项页 | Android 已补 15 个可见设置行（共享键表仍为 16 项）的真实 Toggle 类/所属树/生命周期校验、勾选重摆、待应用值、Apply/Discard/Default、脏状态和三组 radio 转换；保留原手柄导航与模态交还语义 | 真实宿主库合成控件 fixture 通过 127 项检查，包含省电设置的实际 60/30 返回值、换组布局与已有触点取消，以及 `q3d` load/pending/Cancel/Apply 的实际 getter。`q3d` 已接当前 94012 的 3D 比例消费者；其他消费者也各有下述测试边界 |
| 自定义设置消费者 | `lang_en` 在下次启动同时用于 Android locale/`DM_LANG` 和挂存档前原版 Language/VoiceLanguage；`auto_buff` 已读同源开关并保留 `DM_AUTOBUFF` 开发覆盖；`touch_swap_above` 改共用触控布局；`item_notes` 启动选择完整变体目录；过滤规则首次补缺且保留用户文件 | 默认/关/开三模式真实库 fixture 分别通过 178/183/184 项检查，调用真实语言查询和 MountSaveData 绑定；覆盖已有编辑/空文件/符号链接规则保留、链接 Settings 目录不改外部文件、缺资源回退和首次英文设置。自动 buff 实际源策略另通过三模式各 4 项检查；未据此宣称原游戏已施放 buff 或显示了所有注释 |
| 省电帧率 | D2R 专用 `dm_android_d2r_fps_setting` 回调将已应用的 `eco_fps` 送入 Android present pacing；开时将较快节奏限制到 30，保留更慢的 guest 节奏及显式固定 60 诊断优先级；未提供该回调的宿主不受影响 | Custom fixture 验证未 Apply/Cancel/Apply 的 60/30 策略变化。真实 pacer 的 60→30→60 与保留慢 guest 节奏通过 41670 项检查，最终 ELF 已确认 D2R 强定义及通用图形弱调用；当前实机场景约 12–15 fps，30 帧上限不保证满 30 帧。动态 3D 控制器使用同一个实际目标周期 |
| 动态 3D 比例与热观察 | 94012 严格 Build ID、布局、7 组指令锚点及两个原版通知调用点守卫；复用 iOS 生产控制器，缺省 0.75、动态下限 0.60，`q3d` 画质优先保持配置比例。按 eco/guest/固定 60 实际周期观察成功 `mtl_present` 返回间隔，温度只记录；加载、回前台、重建及换档有停判，UI/输出尺寸不随 3D 降低 | 独立设备 fixture 通过 146 项检查，含 30/90 秒恢复、防抖、慢掉帧阈值、配置/版本/线程拒绝、实际原版 wrapper prologue 与受控重建回调及后置条件；D2R Android 已构建并确认 present/resume 链接。数据源是 `queue-present-return`，不是物理 scanout 完成；统一包已完成原游戏逐档降至 0.60，输出保持 1280×720；恢复升档与长期热稳态仍需验收，动态输出分辨率尚未实现 |
| 本地事务存档 | `hle_fs` 使用工作目录、真实 `nn::fs::Commit`、原子目录换入、`fsync` 和提交计数 | 平台事务与 D2R adapter fixture 已通过；只有原游戏自己序列化并提交角色文件，才能证明角色进度已保存 |
| D2R 后台存档 | 专用 adapter 识别当前局号集合及受控玩家，在原版 tick 线程请求游戏自己的定时存档；异步生命周期等待真实提交，回前台以代数取消旧请求 | 已有超时、取消、旧局、失败回滚等 ARM fixture。本轮新版原游戏已完成普通后台存档、暂停后台存档及冷启动重入；具体 APK、Commit 与角色文件哈希见文末 |
| 暂停时后台存档 | 四个 freeze observer 保持世界、客户端、动画及渲染时间冻结，临时释放原暂停以让原版存档路径运行，结束/取消后恢复 | fixture 验证四门、暂停标志保留、在途 tick、失败和取消；最终包实机确认冻结后释放暂停、两次原版 Commit、恢复暂停再 thaw，577ms 后完成。缺门时仍关闭此适配 |
| 个人多页仓库、编年史、珍宝匣 | 同源 `pages/vault/vault_ui/pouch/elysium` 已编译，服务端 tick、UI 消息与 Pouch C2S 已连接；编年史表来自匹配 RomFS | 没有生成表时 D2R 构建目标明确失败。实际存取、跨页、角色重入、侧文件和角色文件一致性尚需原游戏端到端验证 |
| 其他同源功能 | `freetp/kills/runeword/lootfilter/autobuff/shoprefresh` 已进入闭包，补齐相应消息、C2S、pump、filter 和 tick 接口 | 可核查接线和专门 fixture；不能因为依赖闭包能链接就宣称每个玩法都已测通 |
| 秘境与首领连战 | `dm_rift/dm_bossrush`、服务端 tick/C2S、出口改道 0x89、存档幕号 0x8c、佣兵装备校验 0x8d/0x8e、专用 TZ 时钟调用点已接入 | 上游层数、波次、计时等测试及私有门 ARM fixture 通过；完整开局、串图、奖励、死亡/复活、退出重入仍需实游验证 |
| 秘境菜单与进度显示 | `dm_rift_ui.c` 使用原游戏 `MessageLogPanel` 的 `TouchRift/TouchRF_*` 控件实现选层、继续/放弃等，Android 已编译该同源实现 | 菜单依赖匹配的布局/文本 overlay，不能与“缺少 Android 原生菜单”混为一谈。Android 已补独立 copy-only 快照和 `D2rWorldRenderer`，沿用原字体、六目标同源文案、进度条和倒计时；142 项 ARM producer 与 18 项 JVM 投影测试通过，真实 Canvas/秘境场景仍待统一包验收 |
| 秘境进度侧文件 | 与角色 `.d2s` 同次提交前写入；Android 使用 `st_mtim` 判断角色是否被本次存档更新 | ARM 文件 fixture 验证未更新角色不侧写、1ns 修改、文件大小变化、提交被拒/写入失败重试、待提交快照及读回；测试只使用临时角色目录 |
| 平台云同步 | Android 明确不实现平台云同步；SAF/JNI/原生事务接线、Custom 页面入口、测试和构建目标均已删除。共享 `dm_cloudsync` 代码只继续服务 iOS iCloud，不以隐藏托盘或空实现伪装 Android 同步 | Android 不在云同步验收范围内；iOS iCloud 功能与文档继续保留，不能据此推断 Android 已具备同等能力 |
| 界面资源与分辨率 | 已由同源生成链生成布局、图片、文本、手柄/触屏分目录及物品注释变体，并通过布局白名单、12 张 PNG 与 429 项技能索引固定哈希检查。Android HUD asset staging 已包含 97 文件：同源技能/银灰图集、circle/map/menu、现有 mobile_socket 与底栏生成资源；摇杆及缺图时的嵌槽按 `DmTouchControlArt.h` 同参数 Canvas 绘制 | overlay 生成、打包和原游戏加载是不同阶段。D2R 操控不再使用常态文字圆钮，导航回退标签随游戏中英文；3D 内部分辨率已接自适应比例，但游戏输出仍使用固定档位，未实现 iOS 全套动态输出分辨率；使用 Switch 素材放大，不是 PC 双倍素材，未生成 ASTC |
| 更新器 | Android 已接入选角页构建号、HTTPS 清单检查、旧/同版本清除、失败提示、普通/紧急提示，以及点击或 Y 键打开 HTTPS 安装页；纯 JVM 协议测试与华为设备上的 HTTPS 实际传输（含 HTTPS 重定向和多跳拒绝）已通过 | 94012 选角页完整提示、浏览器跳转、连续点击去重和失败提示恢复仍待可用游戏设备验收；其他游戏不会借用 D2R 私有偏移 |

## 剩余差异与实现优先序

此清单对照 `front_ios.m`、`DmTouchOverlay.m`、`dm_rift_hud.m`、`dm_gamepad.m`、`dm_mode_items.h` 和当前 Android 事件分派。共用 C 状态机不包含全部 iOS 外壳行为；下列“缺少”指前端消费/呈现或验收尚缺，不能反过来把已编译的宿主玩法也归为未实现。

| 优先序 | 剩余工作 | 对照与验收条件 |
| --- | --- | --- |
| P0 | 原游戏真实流程验收 | 最终包已完成触摸移动、菜单/选项桥、暂停后台存档与强停冷读档。继续验证实体手柄按键/摇杆/重连、跨角色和更多 UI 场景；ARM 合成对象不能代替这些场景 |
| P0 | 拖放释放协议实游验收 | 前端事务及设备 fixture 已完成；继续在原游戏验证商人/仓库/背包/堆叠、释放失败放回、系统取消和场景重建，确认真实原版选择/落点与动作一致 |
| P0 | 技能绑定与加点实游验收 | `bindTapSync`、空主技能/两组技能位、技能树长按、二次点击与属性连点已有实现及 1782 项组合 fixture、20 项真实库焦点守卫检查。仍需原游戏确认界面开合、原生绑定框和实际点数变化，不能把合成返回值当作角色加点成功 |
| P1 | 提示框与小地图实游验收 | Android 已实现 tooltip 停靠、打开面板让出腰带默认焦点、先问世界目标再决定小地图切全图。372 项设备 shell fixture 覆盖 NPC 优先、无目标展开、全图空白返回、移动取消与过期世界答复；继续核对真实物品 tooltip、面板/手柄切换和地图目标 |
| P1 | 自定义页与消费者实游验收 | 控件桥通过 127 项真实库 fixture，语言/变体/规则消费者通过 178/183/184 项。当前 94012 的动态 3D 已接入并完成原游戏逐档重建；继续验证真实设置页的 Apply/Cancel、启动语言、物品注释、自动 buff 与换组位置 |
| P1 | 原生 HUD 呈现验收 | Android 已接同源完整底栏静态图、药水数量、难度数字及 1–8 选择条、点数/任务十字四态、回城图标、技能嵌槽/空槽与浮动摇杆。继续在最终统一包核对按下态、充能/副手角标、面板/死亡隐藏、球液透出、宽屏/平板几何和原游戏操作，不能以 Java 编译或 asset 哈希代替画面验收 |
| P1 | 秘境与地图原生图层实游验收 | Android 已补 `D2rWorldRenderer` 与独立 1568 字节快照；142 项设备 producer 和 18 项 JVM 几何检查通过。原字体与 70 PNG 已校验，继续核对手柄模式仍显示、面板/场景隐藏、真实倒计时、mini/全图和目标符号；不把数值 fixture 当作 Canvas/原游戏验收 |
| P1 | 商人拖货跟手图实游验收 | `shopGhostSync` 对应流程与 Vulkan 原 draw 重放/货架遮暗已实现，原离线 shader 的 GPU fixture 通过 17262 项；继续验证真实货架的候选识别、商店/堆叠图标与实际落点，以及系统取消和换场景。fixture 的合成纹理/几何不算原游戏物品绘制验收 |
| P1 | 掉落震动系统与实游验收 | 本轮已补代码、真实库/共享规则/生命周期 fixture；仍需在允许系统反馈的设备确认系统效果，并在原游戏掉落验证触发来源及三档效果 |
| P1 | 图形资源预算与回收 | 已按实际 VkMemoryRequirements 建立九类存活/峰值分配账，CPU BC baseline 单列；暂停/drain 后回收 retired 临时 buffer、可重建采样/阴影副本及 writeback。4579 项设备 GPU fixture 通过；统一包后台实际释放 49,464,226 字节，恢复后派生阴影和 HUD 重建正常。主纹理和 pool 仍保留，自动预算及主图驱逐未实现；分配账不是物理驻留或系统 mtrack |
| P2 | 动态输出分辨率与热状态实游验收 | 当前 94012 的 `q3d`、共享动态 3D 控制器、热观察及前后台停判已接入，146 项设备 fixture 通过；统一包原游戏已确认 0.75→0.70→0.65→0.60、3D 内部 960×540→768×432，UI/输出保持 1280×720。恢复升档与长期热稳态仍待验收，物理 scanout 完成采样及动态屏幕输出分辨率仍未实现 |
| P2 | 前端语言、无障碍与布局验收 | 菜单、触控后备文案、虚拟无障碍节点与输入框按钮现跟随当前 HUD[18] 语言，首帧前采用系统语言；不会提前使用待重启的语言选项。全部 Java 编译与 35 项映射检查通过；中英文真实设置切换、切屏和不同长宽比仍待验收 |
| P2 | 可复现完整打包 | 将原游戏模块改写、门表、RomFS/overlay、字体、HUD 图集、shader 缓存与 APK 装配收敛为有输入哈希及检查的 D2R 流程 |
| P2 | 更新器真机验收 | 94012 选角页实际提示、浏览器跳转及连续点击去重仍待可用游戏设备验收；Android 平台云同步和 Zelda 均不在排期内 |

模式登记表中符文之语重建、替按键取消、UI 缓存清理、死亡弹窗与魔盒并排的进/出接口已经进入 Android 闭包；Apple 更新 widget 被明确排除。它们需要随真实布局切换验收，不应重复列为“未移植模块”。

## 原生解码与私有调用

Android 的 D2R 私有分派使用实际解码和原版函数，不以空实现返回成功：

- **0x80 TACT/BLTE zlib**：复用 `hle_tact.c` 与 libdeflate，保留 override、编码标记、输出容量与错误码语义。真实宿主库 fixture 通过 128 条独立压缩流及 8 个边界条件。Apple 的文件预取/后台预解码优化不等于已经移植到 Android。
- **0x83/0x84 Granny BitKnit2**：调用原版逐块解码器。原版资源 fixture 实际执行了游戏解码函数，86 块、704 项检查，解码上下文与输出一致；候选预解码关闭。此结果只覆盖所用资源和两个流式入口。
- **0x81/0x82 C2S**：通过原版收包函数投递，前者安装 Android 产品策略；fixture 检查原参数和保存寄存器保持。诊断重放工具不属于“玩法功能已经对齐”的证据。
- **0x85/0x86 与 0x89/0x8c–0x8e**：已连到对应技能/秘境同源逻辑。0x85 进一步要求 Android 绘图回执与原生 widget 身份同时有效；未满足就走原图。新增真实动态库 HUD fixture 已在生产身份守卫下检查五键正向隐藏、过期/错场景/错类/非 cohort/未画槽位/手柄模式等回退及仅写 x8；回执仍是受控输入，不声称 Java Canvas 或原游戏已经执行。活动秘境、佣兵新穿与读档落位的语义另外以真实源文件 fixture 检查。

## 版本身份与完整性边界

当前 D2R adapter 对 `main` 使用 `db0507f36b84200ac05f75d76ed5026f`，并检查模块范围与相应 observer/SVC 门。UI 桥还要求完整模块大小及 `OBS_UIMSG`、`OBS_TICK`、`OBS_C2S` 和 0x81 等接口。

这里的 `build_id` 是改写器写入 fixup 元数据、由 loader 解析保存的 **NSO BUILD_ID 标签**（原 NSO 构建标识的前 16 字节），不是运行时对整个已加载映像计算 SHA-256。模块大小只提供访问范围约束，也不是版本身份。缺失/不匹配标签会关闭相应私有 adapter；这不等于拥有对任意产物的密码学认证。

原始文件、改写映像、动态库和 shader 缓存的 SHA-256 收据属于另一层验证。交付/部署应保留实际产物的哈希、元数据、生成参数及对应设备日志，不用一个版本标签代替这些证据。

## 构建与打包

Android 的宿主与 CMake 已在仓库中；D2R 完整 UI 目标需要从匹配 RomFS 生成的 `dm_vault_tab.h`，通过 `DM_ANDROID_VAULT_TABLE` 指定。HUD 经验表通过 `DM_ANDROID_HUD_EXP_HEADER` 指定；技能图集通过 `DM_ANDROID_NATIVE_ART_SOURCE` 指定已校验的生成源文件，未提供时保留原版技能图标。对应表生成工具为 `dm/ios/tools/gen_vault_table.py` 和 `dm/ios/tools/gen_hud_exp.py`；原游戏派生文件留在外部构建目录。

构建 `libd2r_mobile_android.so` 成功，只说明原生代码链接完成。D2R APK 还需要 Java 界面/JNI、匹配改写模块及门表、RomFS/overlay、shader 缓存与字体的准备和部署。当前完整 D2R 打包与数据准备仍涉及开发工作区，不能将 iOS 的 setup/IPA 步骤直接当作 Android 构建流程。

本轮 overlay 已生成基础包和物品注释变体，分别有 825/830 条 payload；独立审计通过 BLTE 块校验、编码键/解码大小、配置内容及 archive 范围覆盖检查，原有路径保留。上述结果属于生成物结构与内容验证，实际游戏载入和显示仍须由部署日志确认。

Android 启动按已保存的 `item_notes` 选 `romfs-overlay-variants/item_notes` 或 `romfs-overlay`；变体必须同时有非空 `ctrl_touch`、`touch_strings.tsv`、`data/.build.config`、`data/00-00000007.data` 且 `features.txt` 标记匹配。缺变体会明确记录并回退完整基础目录，不混用两个目录的 archive/index。这里是启动完整性门，不能替代上段的内容/哈希审计。

仓库内 `dm/tools/android_d2r_apk.py` 已支持已准备 D2R 输入的严格 APK 装配：必需原生图集/XP ELF身份、HUD/world 清单及全部 payload 的尺寸/哈希，编译全部 Java、签名/16KiB 对齐并逐字节复核最终包。用法见 `docs/ANDROID_D2R_APK.md`；模块改写和外部 RomFS/overlay/shader 生成与部署仍属另一步。本文只描述 D2R Android 链路。

## 本轮验证索引

设备：ROD-W09，Android API 34，ARM64。独立测试不重启生产游戏、不使用玩家存档。验证源文件可从仓库直接复核：

| 测试 | 范围 |
| --- | --- |
| `dm/android/input_probe.c`、`tests/GameControllerStateTest.java` | 通用触摸、逻辑手柄、多设备与取消行为；不替代真实硬件 |
| `dm/android/tests/t_d2r_ui_device.c` | 真实 UI 模块、合成控件树和消息接收器，55 项检查；包含重置边沿、延后按键清理与滚动/滑块消息 |
| `dm/android/tests/t_d2r_touch_flow_device.c` | 真实传输与共享触控源文件，753 项检查；包含起手分类、拖放、滚动/滑块、最终落点确认、历史交易结果超时放回和取消；原游戏 callee 使用受控替身 |
| `dm/android/tests/t_d2r_touch_skill_flow_device.c` | 真实传输、共享触控/技能外壳及生成图集规则，1782 项检查；包含二次点击加点、属性连点、绑定与拖放分类/释放组合；不使用玩家点数 |
| `dm/android/tests/t_d2r_touch_shell_device.c` | Android 真实传输与 shell 源文件，372 项检查；导航 tooltip/默认焦点让位、小地图世界目标优先、取消/场景代数与所有原版查询仅在 pump 执行；原版 callee 为受控替身 |
| `dm/android/tests/t_ui_ghost_device.c` | 原离线 shader、实际 NVN commandstream/Vulkan replay 与 GPU 像素，17262 项检查；含遮暗、平移、物理 UP、顶点/UBO/纹理 CPU 复用、guest sample counter 和不重新绑定的状态恢复；纹理/几何为合成 fixture，未操作原游戏 |
| `dm/android/tests/t_d2r_skill_focus_device.c` | 真实宿主库命中分类与技能焦点 accessor，20 项检查；控件/命中虚表为合成，核对 opaque ID、真实技能号与跨代失效 |
| `dm/android/tests/t_d2r_custom_device.c` | 真实宿主库设置/手柄桥与合成控件，127 项检查；包含实际 FPS 设置回调、换组布局、触点取消及 q3d load/pending/Cancel/Apply getter；临时设置文件，不调用原游戏设置页 |
| `dm/android/tests/t_d2r_scale3d_device.c` | 共享生产控制器及当前 94012 Android adapter，146 项设备检查；7 组原版指令与真实 wrapper prologue，renderer/时钟/重建回调受控；UI 输出保持、版本/线程/配置拒绝、加载/恢复/eco/guest 周期、q3d 与热观察均覆盖，不等于原游戏或物理上屏验收 |
| `dm/android/tests/t_d2r_custom_consumers_device.c` | 真实动态库与原 HLE 语言/挂载绑定，默认/关/开分别 178/183/184 项；临时目录、规则保留、链接拒绝和完整 overlay 选择；不使用玩家存档 |
| `dm/android/tests/t_d2r_world_overlay_device.c`、`tests/D2rWorldRendererTest.java` | 独立图层 producer+i18n 142 项 ARM 与真实 native bytes 的 18 项 JVM 投影检查；受控 getter，不调用 Canvas/原游戏；72 个资源逐字节校验 |
| `dm/android/tests/t_d2r_autobuff_setting.c` | 实际自动 buff 源码启用策略，三种环境覆盖各 4 项；替换设置/标题门输入，丢弃私有玩法函数，不执行原游戏施法 |
| `dm/android/tests/t_d2r_hud_device.c` | 最新 schema 2 真实动态库带图集 317 项，含真实 Android producer 消费布局重建计数、新控件集合重新确认、旧绘图凭据拒绝；旧库负向回归失败。旧 schema 1 无图集 16 项为历史回退证据；生产 widget 身份与受控绘图回执，不调用 Java Canvas |
| `dm/android/tests/t_d2r_haptics_device.c`、`t_d2r_haptics_host_device.c`、`tests/GameHapticStateTest.java` | 共享真实掉落规则/队列、设置 Apply/Discard、实际动态库 JNI 出口与 Java 生命周期；不调用实际系统 vibrator，也不使用原游戏物品 |
| `dm/android/tests/t_d2r_host_attach_device.c` | 8 个身份/范围/门检查，6 个场景转换 |
| `dm/android/tests/t_d2r_aim_device.c` | 真实动态库，9 次原函数调用与瞄准/释放恢复 |
| `dm/android/tests/t_d2r_native_host_device.c` | 真实动态库的 zlib、C2S 与寄存器语义 |
| `dm/android/tests/t_granny_device.c` | 实际原版 BitKnit2 函数与外部原版资源 |
| `dm/android/tests/t_d2r_save_device.c` | 合成局/玩家、真实 HLE Commit、HUD 与暂停/取消协议；结果明确标记 `original_game_save_executed=false` |
| `dm/android/tests/t_d2r_packet_policy_device.c` | 实际 Android 策略源文件及受控 ARM 原版函数替身 |
| `dm/android/tests/t_d2r_product_host_device.c` | 完整私有配置下的真实动态库分派、移动策略、系统包和 TZ 时钟守卫 |
| `dm/android/tests/t_d2r_rift_private_device.c` | 活动/非活动秘境、存档幕号与佣兵装备语义 |
| `dm/android/tests/t_d2r_rift_commit_device.c` | 临时文件中的秘境侧文件提交与失败重试 |
| `dm/host/tests/t_rift_*.c` | 上游层数、波次、计时等模块回归；不调用原游戏完整流程 |

开发收据、派生资源和完整日志留在仓库外的 `analysis/parity-20261019/`。独立测试库快照与逐项日志的对应记录为 `d2r-new/bridge-port/VERIFICATION.md`、`HAPTICS_VERIFICATION.md`、`SKILL_FLOW_VERIFICATION.md`、`CUSTOM_CONSUMERS_VERIFICATION.md` 和 `WORLD_OVERLAY_VERIFICATION.md`；后续最终打包可能产生不同动态库哈希。

### 上一批新版原游戏端到端证据

最终独立测试包 `org.d2r.newprobe` 已安装于 ROD-W09（API 34、Mali-G610 MC6），使用新建测试角色 Kord。APK SHA-256 为 `4b090c4ee302302a96d3b93eaa5711b8261264836e5fb0ee041f57d662f418f9`，原生库为 `f8f64c2ec6ad19a916dbcf107bdaa9f6b324e2fb6e1bb0520ec4940827552d3f`；安装后的 APK 哈希与构建产物相同。模块是完整 touch-run-v4 改写，主程序 BUILD_ID 为 `db0507f36b84200ac05f75d76ed5026f`，RomFS 使用 `f5f3bcfb…1f12785` 的 8,973,550,584 字节资源。物品注释变体在启动日志中实际选中，其 6 个部署文件逐字节匹配生成物；APK 内 97 项 HUD 资源及清单通过打包审计。

| 场景 | 原游戏实际结果 |
| --- | --- |
| 选角、进入营地与移动 | Kord，1 级野蛮人，区域 1；触屏摇杆使坐标从 (4009,5209) 变至 (4015,5212)，松手后四轴归零 |
| iOS 风格操控画面 | 集中截图确认主技能/回城/地图图标、金属嵌槽、空槽刻印、摇杆、底栏、药水数量、难度格、十字及 HP/MP/Lv/经验；未以截图宣称所有按下/死亡/充能状态都通过 |
| 原版 UI 触控桥 | 点底栏十字打开菜单，`scene.panels=1`；直接点击顶部选项页签后为 `8`，并保持触摸导航模式 |
| 暂停后台保存 | `all_four_gates=1`，先冻结再临时释放暂停；原游戏 Commit 从 1 增至 3，日志确认 `restored_pause=1 thawed=1`，577ms 完成，后台完成 epoch 与生命周期 epoch 均为 3 |
| 强停后冷启动读档 | 保存完成后执行 force-stop，再冷启动选择 Kord 并进入区域 1；save/save.work 的 1010 字节 D2S 均校验有效，SHA-256 同为 `5aa103b6ef969f7bbc379143092465f84451a186a5a29b789f5f10db2b4f6fa1`，与后台完成时完全相同 |

此前 HUD 诊断构建还分别完成普通后台和暂停后台保存，两种场景各增加两次原版 Commit、D2S 哈希变化且校验有效；暂停保存后进程被系统以 `LOW_MEMORY` 回收，随后冷启动读回的角色文件与回收前一致。此记录证明对应构建的落盘和恢复，不能解释为后台始终常驻。当前场景约 12–15 fps，图形内存占用仍需优化。

完整状态、日志、备份、校验与产物关联记录位于工作区外部 `analysis/parity-20261019/d2r-new/device-ROD/device-report.json`，最终常态截图为 `final-touch-visual.png`。实体手柄未实际连接；ADB 注入按键只用于选角。原游戏物品交易/拖放、实际技能加点及完整秘境/首领连战仍未在本轮端到端验收。

### Android 平台云同步（明确不做）

此前的 Android SAF 目录选择、持久授权、provider 状态轮询、JNI/原生云事务接线、Custom 页面 `icloud` 行、专用测试与构建目标已经移除。共享 `dm_cloudsync` 和 iOS iCloud 实现继续保留，但 Android 不加载该模块，也不保留隐藏入口或低优先级排期。

### 最新统一包：布局重建、首触与后台缓存回收

当前已安装的 `org.d2r.newprobe` 为 `test-apk-parity-batch3/d2r-94012-test.apk`，APK SHA-256 `9e4d9f6219f3f5a4d4cbff04d7f9105c6421a8cc592e2150b4c9f61c4a2e71ff`，原生库 `af5d773741b40066fbf7532ef12900258c17ce1e16812ed849dac094d751aa30`。安装后 APK 哈希匹配；185 项打包 payload 校验通过，D2R Android 构建成功。此前 batch2 存在布局切换后的技能图标回退，不能作为最终画面通过版本。

| 场景 | 本包原游戏结果 |
| --- | --- |
| 手柄布局切回触摸 | 按 iOS 在 `dm_padmode_rebuilds()` 完成计数变化后更新 worldEpoch；六控件 cohort 重认，保留身份校验与旧绘图凭据失效规则。真机 worldEpoch 5→6，cohort_seen=63、binding.ready=1；HUD 317 项回归在旧库失败、新库通过 |
| 第一个触摸手势 | inactive overlay 在局内接管 DOWN，避免下层旧菜单按钮截走；无需先点模式切换。Kord 坐标 (4013,5205)→(4018,5200)，UP 后四轴归零；强停冷启动后再次通过 |
| 图标与面板切换 | 原游戏菜单→选项→关闭对应 panels 1→8→0；回到场景后仍显示真实主攻击图标、四个空技能嵌槽、回城/地图、摇杆及完整底栏。实际集中截图两张，没有把空槽伪造为已学技能 |
| 动态 3D | 0.75→0.70→0.65→0.60，内部 960×540→896×504→832×468→768×432；输出/UI 始终 1280×720，四次原版重建后置检查成功 |
| 暂停后台存档 | all_four_gates、frozen_before_unpause、restored_pause、thawed 均为 1；Commit 1→3，559ms 完成，background done/lifecycle epoch 均为 3 |
| 安全后台 trim 与恢复 | suspend/drain 后实际释放 49,464,226 字节（约 47 MiB），主图 732,693,952 字节、pool 1,167,065,088 字节、CPU baseline 170,356,224 字节保留。恢复后阴影副本 221,184 字节重建、HUD ready；这表示分配释放，不是系统物理驻留下降测量 |
| 强停后读档 | 后台完成、强停前和冷启动重入后的 save/save.work D2S 均为 1010 字节、校验有效，SHA-256 全为 `9b8a6b41d69a4a136ada3229e9a5084a874dfc9585a192ab3af5063c651d9b80` |

Vulkan 分项账/trim 的 4579 项独立设备检查还覆盖真实在途 GPU fence 拒绝回收，以及派生图重建前后实际 GPU 采样像素一致；不以合成对象代替上述原游戏验证。前端双语映射 35 项与全部 Java 编译通过，菜单/触控/无障碍语言跟随当前 HUD，而非尚未重启的 Custom 语言选项。

本包完整收据为外部 `analysis/parity-20261019/d2r-new/device-ROD/batch3-device-report.json`；画面为 `batch3-touch-visual.png` 和 `batch3-cold-touch-final.png`。实体手柄硬件、真实交易/技能加点及合资格秘境角色仍有待完成验收；更新器已完成代码接线、协议测试和设备网络传输验证，但 94012 选角页上的完整交互仍待可用游戏设备验收。不能称已全部等价。
