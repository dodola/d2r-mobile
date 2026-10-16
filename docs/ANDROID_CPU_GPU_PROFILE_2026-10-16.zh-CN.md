# Android 提交等待与 CPU 缓存对照（2026-10-16）

本批在 main 上修正性能诊断，并验证现有命令状态缓存、PSO last-cache 的实际行为。
它延续兼容性与性能优化目标，不代表驱动矩阵、营地/战斗或全部格式支持已经完成。

## 设备和构建

- ADB `A6BFUT1C07005372`，TNA-AN00，Android 14 / API 34，Adreno 642L。
- 使用 Qualcomm 系统驱动，driver ID 8，build `923a446bf8, I23d7d7df15, 1725536844`。
- 原生库 SHA-256：`a19c874c25d46961e1d0af012c2530c871ae8fc4f4fbe42a5530dff5af72f5ba`。
- 测量 APK 仅替换原安装壳中的原生库，签名和 16 KiB 对齐校验通过；没有替换
  Java、资源、游戏模块或驱动。应用 UID 为 10200，原资源镜像为 8,973,693,207 字节。
- 角色选择场景为现有离线野蛮人 KKK；截图人工确认场景和人物外观，未将标签字符串
  当作场景探针验证。采集期间不发送输入。

本机证据目录为 `D:/AndroidSwitch/analysis/compat-performance-20261016/`。
构建证据是 `submission-profile-native.json`、`D2R-CPU-Profile.build.json`。
安装前另存了 `cpu-profile-device/saves-before.tar` 和诊断标记状态。

## 修正的诊断

原来的 `RENDER_SUBMISSION_PROFILE` 将 `contexts=1 async=0` 写死，不能表示运行状态。
现在从实际 submission slot 和 `G.async_enabled` 读取；本机正常运行报告
`contexts=2 async=1`。原 PSO 日志写成 `byte_fnv1a`，实际已使用四路 word 混合，
现改为 `word4_mix`，避免对错误的算法描述做性能判断。

新增等待统计：

- `slot_wait_ms`：回放线程等待 submission slot 可复用的墙钟时间。
- `drain_wait_ms`：调用者等待完成队列排空的墙钟时间。
- `completion_fence_wait_ms` / `completion_waits`：完成线程实际等待 Fence 的累计时间和次数。

这些时间来自并发线程，会重叠，不能相加作为一帧总耗时。
slot/drain 的条件变量等待也进入现有 ADPF Fence 等待扣除，避免将异步 GPU 等待
当成 CPU 工作。完成线程的等待不并入回放线程的 CPU 工作。
本轮 ADPF hint session 仍默认关闭；没有把这项计时修复宣称为已测得的调频收益。

内存字段分别报告单个 context 结构、descriptor arena 和 descriptor entry 数组的
字节数；它们不是完整 Vulkan 内存占用，不能据此推算驱动内部 descriptor/query 分配。

## 正确性验证

`run_attachment_device.py --command-state-checks` 在本工作树编译真实 NVN 后端，
使用已有本地原始 Shader 夹具，分别运行关闭、状态缓存、PSO last-cache、两者同时开启。
它不会安装 APK 或修改游戏数据。

- 四种组合均通过 mask、blend、动态常量、cull、topology、动态 scissor 和 sRGB 格式测试。
- 每个进程含 64 次 indexed draw、8 次事件切段和 8 次 Compute/Graphics 切换。
- 每种组合读回 131,072 字节；SHA-256 完全一致。
- 每个进程还通过 120 项附件测试、1,720,320 字节比较；四个进程均为系统驱动 ID 8。
- 命令缓存开启时，七类状态命令均实际发生省略；PSO last-cache 开启时，224 次查询中
  有 162 次 last hit，hash 调用从 224 次降至 62 次。
- 主机差分状态测试在优化构建及 ASan/UBSan 下各完成 200,753 次状态比较。
- CPU 翻译队列及适配器在普通和 ASan/UBSan 构建下通过，覆盖 worker/sync 与暂停后 drain。

原始结果：`cpu-cache-gpu-fixed/result.json`、`cpu-cache-host/result.json`、
`submission-translation/result.json`。早期夹具组合未恢复 texture builder 的 sRGB 格式，
触发明确的格式拒绝；已恢复格式和原始像素后重新验证。失败日志保留在 `cpu-cache-gpu/`。

## 完整游戏对照

新增 `dm/android/tests/capture_present_profile.py`，只读采集 SurfaceFlinger latency、
增量渲染日志、电池温度和 thermal status，不清除显示统计、不重启或修改应用设置。
使用第二列 actual-present 时间戳去重，排除 0 和 INT64_MAX，检测显示历史是否在轮询间
覆盖，进程改变或样本不足时拒绝结果。报告区分实际呈现 FPS 与应用内部 FPS。

初始无详细诊断的 60 秒采集为 15.077 FPS，电池 38°C、thermal status 1。
它与后面的详细诊断测试温度、计时开销不同，不能直接作为改进幅度的基线。

下面每轮采集 45 秒，同一原生库和渲染设置，开启详细 CPU/GPU 计时，管线编译为零。
每 60 帧约 58,140～58,200 次绘制，细小变化来自菜单动态元素；没有跳过绘制。
CPU 列为各轮完整窗口的中位数，GPU busy 是 chunk 顶/底时间戳范围，不是硬件利用率。

| 顺序 | 状态缓存 | PSO last | 实际呈现 FPS | CPU replay ms/帧 | PSO lookup ms/帧 | GPU busy ms/帧 | 电池 / thermal |
|---|---|---|---:|---:|---:|---:|---|
| 0 | 关 | 关 | 14.627 | 23.047 | 0.573 | 51.308 | 40°C / 1 |
| 1 | 开 | 关 | 14.754 | 22.708 | 0.570 | 51.168 | 40°C / 1 |
| 2 | 关 | 开 | 14.603 | 23.397 | 0.357 | 51.232 | 40°C / 1 |
| 3 | 开 | 开 | 14.749 | 22.765 | 0.363 | 51.227 | 40°C / 1 |
| 4 | 关 | 关 | 14.514 | 24.118 | 0.589 | 51.313 | 40～41°C / 2 |
| 5 | 开 | 开 | 14.642 | 23.712 | 0.369 | 51.195 | 40～41°C / 2 |

状态缓存将每帧实际发出的七类状态命令从约 6,701 次降到约 2,196～2,223 次。
PSO last-cache 减少了查询成本，但整体 CPU 和实际呈现没有一致同步改善。
两组重复对照的 FPS 差异均不足 1%；未控制 GPU 频率，后两轮进入更高温控等级，
不能证明普遍或显著的 FPS 收益。因此这两项仍保持显式实验开关，不修改默认值。

一次重启后的重复基线包含 3.190 ms 新管线编译，未纳入上表；延长预热后重测。
启动脚本还修正了新进程短暂读取上次启动日志的问题。无效采集保留用于审计，
未将它们混入有效的六轮结果。

证据：`cpu-menu-ab-result.json`、`cpu-menu-ab-summary.json`，对应各轮的
`display-samples.jsonl`、`state-samples.jsonl`、`interval.log` 和截图。
本轮没有完成营地/战斗、其它设备或其它驱动的完整游戏验证。

## GPU Pass 定位和恢复验证

另一次仅用于定位的 `profile-detail` + `pass-timing` 采集，得到所有已计时 Pass 合计
约 47.293 ms/帧。最大的组为 `rt_key=70b52d91c`：1280×720，颜色纹理 serial 23
（NVN R11G11B10F / Vulkan B10G11R11_UFLOAT_PACK32），深度纹理 serial 29
（D24_UNORM_S8_UINT），每帧两个 Pass、合计约 374 次绘制，约 25.194 ms/帧。
同一深度目标的无颜色 Pass 组约 3.907 ms/帧。组内时长包含 Pass 周围已插入的同步，
不能直接解释为某个片元 Shader 的纯执行时间。

高频片元 Shader 包括 `38f344adf801b153`、`5c1ca4e7c3e4657c`、
`b3d4dd57cd4d630c`；高频统计覆盖所有单颜色 1280×720 目标，不专属于上述最重的组，
也不等于逐 Shader GPU 耗时。下一步需要关联具体绘制、资源与 Shader 成本。
原始证据为 `gpu-pass-menu/`，摘要为其中的 `pass-summary.json`。

清除本轮新增的诊断标记并重启后，普通模式 60 秒实际呈现为 15.021 FPS，
电池温度 40～41°C。这是当前运行观察，不是相对初始 38°C 基线的性能提升。
原始记录为 `normal-menu-final/`。
最终交付检查 `cpu-optimization-validation.json` 还确认实际映射的原生库 SHA-256 与构建
一致，编译输入哈希与当前工作树一致，UID/资源大小未变，14 个存档及设置文件内容
哈希全部未变；诊断标记恢复，普通模式两个 CPU 缓存仍关闭。

只读采集入口示例（输出目录必须尚不存在）：

```powershell
python dm/android/tests/capture_present_profile.py --serial A6BFUT1C07005372 --scene character-menu --seconds 60 --output D:/AndroidSwitch/analysis/present-new-run
```

真实后端对照入口（需要本地原始 Shader 夹具及已配置的 CMake core 对象）：

```powershell
python dm/android/tests/run_attachment_device.py --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache --build D:/AndroidSwitch/build/d2r-system-driver-main --output D:/AndroidSwitch/analysis/cpu-cache-new-run --command-state-checks --run-device --serial A6BFUT1C07005372
```

## 下一步

当前菜单持续运行的主要成本在 GPU；CPU 缓存不能代替 GPU Pass、反馈复制和 Shader
成本分析。继续使用 pass 时间戳定位具体目标，再做同场景正确性与性能对照。
深度复制、顶点格式替代路径和驱动恢复仍属于总目标的未完成工作。
