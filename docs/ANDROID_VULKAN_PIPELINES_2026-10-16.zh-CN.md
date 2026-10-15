# Android Vulkan 管线缓存与预热

日期：2026-10-16。分支：`codex/android-measured-optimizations`。基线：`642480b`（已合入 main）。

已完成实现、编译、独立 GPU 验证及三组完整游戏采样。实测营地首次进入所需的 36 条管线，在保留缓存并重启预热后由同步创建约 3.14 秒，变为直接领用约 0.915 毫秒、零编译等待。整体入场卡顿和持续 FPS 的收益尚未证实；入场窗口有动态刷新与显示图层缺失，不能用不完整的显示数据推导掉帧改善百分比。

## 1. 上游复用范围与 Android 实现

上游 `nvn_mtl.m` 的 `pso_user_record / warm_start / warm_claim` 已经实现“记录本机真正使用过的完整管线，下次启动按清单后台创建并领用”。Android 原先只有进程内 `VkPipelineCache` 和 PSO 表，`mtl_warm_wait` 为空。本轮沿用上述记录、预热、领用流程及该等待接口，保留公共 NVN 命令与上游 Metal 实现。

Metal 的管线描述、函数对象和系统缓存不能直接作为 Vulkan 管线使用。因此 Vulkan 侧保存独立的 `pso_user.vkbin` 清单，并用 `vkGetPipelineCacheData` 导出驱动缓存、下次以 `VkPipelineCacheCreateInfo.pInitialData` 加载。相关接口约束见 [Vulkan 缓存导出](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html)与[缓存初始化](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateInfo.html)。

实现位于 `nvn_vk_pipeline.inc`，同步创建与后台创建共用同一个描述符工厂。记录完整 PsoKey 的顶点输入、光栅、深度/模板、混合、反馈标记、附件兼容描述和 storage-bias 特化值；进程内 shader 下标在落盘时替换成稳定的 shader key。清单内没有 guest 指针、Vulkan 句柄或 shader 两两穷举组合。

| 层次 | 行为 |
| --- | --- |
| 进程内命中 | 继续复用原来的完整 PSO 哈希表；提交中引用的对象不被驱逐 |
| 已记录的完整管线 | 启动后台预热；若明确指定 Vulkan 格式种子清单，连同本机清单去重加载 |
| 首次出现的管线 | 优先送给编译线程；绘制必须等正确结果就绪，失败后同步重试，仍失败则沿用原来的明确报错 |
| 后台正在预热别的管线 | 新需求在下一条工作选择时优先；不抢占驱动正在执行的编译 |
| 缓存写入 | 后台线程定期快照、fsync 后原子替换，保留之前完整文件；不会等待正常退出才保存 |

工作线程不访问提交状态 S 或获取 G.lock。启动预热使用独立 shader module/layout，省略不参与编译的 C1 buffer 和绑定计划；运行时需求使用主线程已经初始化、进程生命周期内有效的 shader 对象。管线描述与主线程完全相同，shader layout 按同样的定义创建。Vulkan driver cache 的创建和快照用专门的互斥锁保护。

“后台创建”并不意味着首次使用没有等待。没有清单时仍然需要编译；同一编译线程上的保存操作也可能影响后续需求等待。预热有启动 CPU 与内存成本，只有真正取用到的管线才有提前创建的价值。本轮没有照搬 Metal 的提交后续命令预测，也没有在 Android 上猜测读取画面回调来强加 15 秒加载闸。

## 2. 持久化、失效与诊断

私有路径：`files/d2r-mobile/pipeline-cache/{pipeline-cache.bin,pso_user.vkbin}`。文件有版本、种类、长度、CRC32 和兼容标识；兼容标识包含 GPU vendor/device、driver version、pipelineCacheUUID、shader_index CRC、管线键尺寸及 shader 适配选项。驱动缓存的 Vulkan 标准头再次核对。驱动、GPU、shader 索引、键格式或适配选项变化，以及截断、长度不符或损坏，都视为缓存未命中，不替换正确的渲染行为。

最多保留 2,048 条完整管线，驱动缓存载入上限 64 MiB；达到 PSO 表上限后沿用原来的 fence 所有临时管线生命周期。后续修改键语义时必须升级文件 schema，不能仅依赖结构体尺寸。

诊断文件 `diagnostics/pipeline-mode.txt` 在启动时读取，必须重启才改变：

- `0`：原有进程内缓存；不读写本轮磁盘缓存，不启动编译线程。
- `1`：驱动缓存落盘并记录清单，需求仍同步创建，不预热。
- `2`：磁盘缓存与后台预热、需求创建，默认开启。

`DM_PSO_MANIFEST=off` 禁止启动清单预热；如传入路径，必须是本轮 Vulkan 格式。默认种子为 shader 目录下 `pso_manifest.vkbin`，不读取上游 Metal 的 TSV 描述。`DM_DISABLE_PSO_REUSE` 仍保留原有诊断语义，禁用后台领用。没有提供缓存目录的独立夹具默认走模式 0。

创建 `diagnostics/pipeline-ab` 并重启，可记录 `PIPELINE_FRAME` 累计计数。日志将同步建管线、后台建管线、预热领用和需求等待分开。`build_ns / wait_ns / bg_ns` 均为 CPU 墙钟耗时；不能当作 GPU 耗时，也不能将“同步创建为零”解释为“没有阻塞”。

## 3. 已完成的编译和独立验证

- NDK 27.2.12479018，arm64-v8a / API 29 / RelWithDebInfo 编译、链接通过，新增 C 代码经过 `-Wall -Wextra -Werror`。
- 生产缓存文件代码通过 415 项主机检查，普通及 ASan/UBSan 各一轮；覆盖逐位置截断、逐字节损坏、GPU/驱动/UUID/shader/选项失效、错误类型、容量上限、写入失败保留旧文件及尾部多余数据。
- 荣耀 PPG-AN00 的真实 NVN/Vulkan GPU 夹具按模式 0、首次模式 2、重启模式 2 各运行 120 个用例，共 360 个。每轮比较 1,720,320 字节，无差异；三轮基础颜色图 SHA-256 相同。另有 compute、真实写故障恢复和紧凑记录检查通过。
- 重启夹具加载驱动缓存 **11,645 B**，实际领用 **10 条**清单管线。模式 0 的 10 条同步创建合计 25.470 ms；首次模式 2 的需求等待合计 27.654 ms；重启预热后领用等待合计 0.307 ms。夹具规模很小、系统驱动内部缓存未清除，这些数字只验证路径命中，不能代替完整游戏卡顿结果。

对应原始目录：`pipeline-host-20261016`、`pipeline-device-final-20261016`，均在 `D:/AndroidSwitch/build/android-measured-optimizations/` 下。GPU 夹具使用系统 Vulkan 驱动；游戏继续使用原来的私有 Turnip/sysmem 配置，两者分别记录。

## 4. 完整游戏对比方法

测试包 SHA-256：`4e331345cf973081f7aba3fd6d57e46fd5064e46d11826197a71d44cf4ff1154`；native SHA-256：`f8b53be2913600bc6f9c78b16c2963546acc28afde397d533d1a8e6d65cad0e3`。

设备固定为荣耀 PPG-AN00 `AVAY025422000548`。保留 UID 10490、原游戏资源和存档、Turnip 驱动/sysmem、3D scale 1.0、刷新策略及用户选择的“仅清屏合并”。用户操作和观察画面，助手只读日志、场景数据与设备遥测，不发送游戏输入或截屏。

使用同一 APK 依次比较模式 0 的首次/重复进入、模式 2 空私有缓存的首次/重复进入、模式 2 保留缓存并重启后的首次/重复进入。每次启动先在角色选择界面采集至少 120 秒，电池温差 ≤0.5°C、skin 温差 ≤1°C、thermal status 不变后提示进入。每次进入营地停留 60 秒，分别保留进入后的前 30 秒与末尾 30 秒；保存退出后同角色重进。实际区域和位置由固定 build91738 的只读探针核对，不用截图认场景。六次均为玩家 id 1、区域 1、位置定点值 `(318734336, 370507776)`；每次保存退出再进入都会重新建立对局，单位地址随之变化。

入口锚点是玩家路径首次读到区域 1 的采样时刻，**不等于加载画面消失或用户按键的精确时刻**。从提示到入口的准备区间含用户操作等待和菜单绘制，不能将其长度解释成加载时间。原始显示时间戳、逐帧管线计数和区域链都保存，方便单独检查进入前发生的编译。

显示卡顿用 SurfaceFlinger actual-present 间隔，与 CPU 出图日志分开。按实际显示刷新槽数统计跨至少两次刷新的间隔，同时保留 p95、最大间隔及 >100 ms 次数；动态刷新切换和图层缺失必须明确标记。末尾 30 秒再次检查热稳态。未清除驱动内部系统缓存，不宣称整机或系统驱动完全冷启动。

第一次基线预热期间遇到显示图层重建，严格发现层失败，未开始进场采样便退出；失败记录保留在 `run-01-baseline`。采集器补充了短暂图层缺失的记录与重新绑定，再以同 PID 重试，不能把该失败视为游戏崩溃或零掉帧。

三组进程分别为 PID 25306、16717、26747。首次建立游戏缓存前目录为空；保留缓存重启时载入 **1,475,277 B** 驱动数据和 **113 条**清单，后台创建合计 **127.735 ms**，进入营地前已完成。进入营地后的前 30 秒结果如下，单位均为 CPU 墙钟统计：

| 配置 | 进入 | 本窗口创建 / 领用数 | 关键路径统计 | 仍需等待编译数 |
| --- | --- | ---: | ---: | ---: |
| 模式 0，进程内缓存 | 首次 | 同步创建 36 | 创建 3,144.168 ms | 36 |
| 模式 0，进程内缓存 | 重复 | 0 | 0 ms | 0 |
| 模式 2，空私有缓存 | 首次 | 后台需求创建并领用 36 | 领用等待 3,237.623 ms | 36 |
| 模式 2，空私有缓存 | 重复 | 0 | 0 ms | 0 |
| 模式 2，保留缓存后重启 | 首次 | 已预热管线领用 36 | 领用 0.915 ms | 0 |
| 模式 2，保留缓存后重启 | 重复 | 0 | 0 ms | 0 |

基线创建数来自同步 Vulkan 工厂调用，模式 2 的领用计数包含等待、查找和交接时间。它们不包含全部资源读取和帧处理。**空清单首次进入没有改善**，把需求送到后台仍需等待正确管线；本轮验证的有效部分是保留缓存后提前完成已知管线创建。未实现的提交后续命令预测不能作为本轮收益。

以下保留每轮末尾 30 秒的显示结果，同时列出排除原因；只有同时满足热检查和显示覆盖的窗口才用于稳定帧率观察：

| 配置 / 进入 | 显示 FPS | 跨两次刷新占比 | 质量与用途 |
| --- | ---: | ---: | --- |
| 模式 0 / 首次 | 29.907 | 0.446% | 温稳、固定 30 Hz、覆盖完整 |
| 模式 0 / 重复 | 29.973 | 0.223% | 前 120 秒电池温差 1°C，排除热稳态对比 |
| 空缓存模式 2 / 首次 | 30.038 | 0% | 温稳、固定 30 Hz、覆盖完整 |
| 空缓存模式 2 / 重复 | 26.812 | 不计算 | 温差 1°C、刷新变化、图层缺失，排除 |
| 重启预热 / 首次 | 30.037 | 0% | 断线导致热观测跨度不足，排除热稳态对比 |
| 重启预热 / 重复 | 30.069 | 不计算 | 温稳，但刷新变化、图层缺失，排除 |

进入后的前 30 秒，各组均观察到 30/60 Hz 变化，因此掉帧比例保留 null，不将整段间隔套用单一名义周期。最长观测间隔分别为基线首次 4,538.713 ms、空缓存首次 3,662.023 ms、预热首次 7,090.974 ms；基线与预热首次还有空图层轮询，**这些值不能作为准确的入场卡顿改善证据**。预热首次虽已没有管线编译等待，显示长间隔仍存在，需另行调查显示层、刷新切换或其他工作，不能归因到某个未验证原因。完整统计和所有排除项都保留在 JSON。

第二组重复进入在 60 秒后继续等待温稳，但温度在 44–45°C 波动；等待分支还暴露了采样间隔意外加快的问题。该延长段全部排除，采集器已恢复固定 60 秒、按约 500 ms 采样并如实标记热检查失败。原采集在切换进程时退出，`completed:false` 和错误原样保留；汇总器仅在明确的 controlled-stop 记录存在、同 PID 游戏数据连续且足够 60 秒时，恢复已完成的重复进入窗口，不将整次采集改写成成功。恢复窗口截至首次达到 60 秒的观测，未使用之后的高频采样。

第三组在预热温稳完成后发生一次短暂 ADB 断连；恢复时确认 PID 和配置相同，尚未进入场景，接续原预热记录再采首次进入。断连前后观测空隙导致该次首次进入的热覆盖不足，已明确排除；没有把它补成连续温度数据。采集器已补充只读命令的有限断线重试与同进程 pre-entry 接续。

用户对三组明确回复“没有发现画面异常”。本轮助手没有截屏或注入游戏输入。测试结束保留最终 **114 条**实际请求管线清单和缓存，关闭 pipeline-ab / attachment-ab 逐帧诊断并重启；确认模式 2 载入 **1,496,829 B** 驱动数据和 114 条清单，逐帧诊断日志均未出现，原清屏选项保持不变。

完整原始目录：`D:/AndroidSwitch/build/android-measured-optimizations/pipeline-ab-20261016`。结果、精确窗口、质量标记、来源哈希和最终配置见 [机器摘要](ANDROID_VULKAN_PIPELINES_2026-10-16.json)。

## 5. 复现入口

```powershell
& 'C:/Users/dodol/AppData/Local/Android/Sdk/cmake/3.22.1/bin/cmake.exe' --build D:/AndroidSwitch/build/android-measured-optimizations -j 6
wsl -d Ubuntu -- python3 /mnt/d/AndroidSwitch/worktrees/android-measured-optimizations/dm/android/tests/run_pipeline_checks.py --output /mnt/d/AndroidSwitch/build/android-measured-optimizations/pipeline-host-repeat
python -X utf8 dm/android/tests/run_attachment_device.py --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache --build D:/AndroidSwitch/build/android-measured-optimizations --output D:/AndroidSwitch/build/android-measured-optimizations/pipeline-device-repeat --serial AVAY025422000548 --run-device --pipeline-check

# 由操作者停留角色选择界面后开始；首次出现 ready_to_enter 才进入营地。
python -X utf8 dm/android/tests/capture_pipeline_game.py --serial AVAY025422000548 --output D:/AndroidSwitch/build/android-measured-optimizations/pipeline-game-repeat --settle 120 --entry-seconds 60

python -X utf8 dm/android/tests/summarize_pipeline_game.py D:/AndroidSwitch/build/android-measured-optimizations/pipeline-ab-20261016/run-01-baseline-retry D:/AndroidSwitch/build/android-measured-optimizations/pipeline-ab-20261016/run-02-first-cache D:/AndroidSwitch/build/android-measured-optimizations/pipeline-ab-20261016/run-03-warm-resumed --allow-controlled-stop --output D:/AndroidSwitch/build/android-measured-optimizations/pipeline-ab-20261016/game-analysis-repeat.json
```

输出目录需为新目录。GPU 夹具、基础 APK、原游戏文件及私有驱动是已有本地资源，不在仓库中。APK 打包仍复用 `package_worktree_apk.py`，仅替换 native 和重新编译的 classes.dex，校验原有其他条目内容及签名、16 KiB 对齐。
