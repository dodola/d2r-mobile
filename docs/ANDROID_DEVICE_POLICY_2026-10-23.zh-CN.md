# Android 机型策略与上游修复验收（2026-10-23）

English: [ANDROID_DEVICE_POLICY_2026-10-23.md](ANDROID_DEVICE_POLICY_2026-10-23.md)

范围：`codex/ppg-render-slots-20261023` 隔离工作树；不代表共享集成目录已经合并。本次没有安装上游 APK，没有修改 TNA。用户自行验收战斗。

## 是否全部放开

结论：通用优化可以统一默认，但不要删除硬件能力检查、驱动错误修正和实验守卫。双槽已经对全部设备/驱动默认开启；不是仍只允许 SM8750。

| 项目 | 当前实际条件 | 建议 |
|---|---|---|
| 双槽、FIFO 发布、UBO 与非 uniform 版本化 | 所有机型、所有驱动；独立禁用标记或显式槽数可回退 | 已放开，保持 |
| NaN min/max/clamp 语义 | 所有驱动默认启用；`disable-nan-minmax` 回退；缓存身份隔离 | 通用语义修正，保持；没有解决系统驱动的局部黑块 |
| 启动器自动选择 Turnip | Android API ≥31、`SOC_MANUFACTURER=QTI`；SM8750 被旧闪屏名单排除。显式选择 Turnip 始终优先 | 其他高通机型本来就未受白名单限制。SM8750 例外可在预旋转战斗/生命周期验收后移除；不能把 Turnip 推给 Mali 等非 Adreno GPU |
| `VERIFIED_TURNIP_SOCS` | SM7325、SM8750 | 只是已测记录，不是 Turnip 的允许列表 |
| Turnip 预旋转 | 本次新增：SM8750 且实际 Vulkan driver ID 为 Mesa Turnip 自动启用；`present-prerotate.txt=0/1` 强制；检查旋转类型和交换链颜色附件能力 | 当前机型已测。其他机型可测试显式启用，尚不建议无验证扩大到全部驱动。最终可按驱动和窗口能力统一策略 |
| 原生 BC 纹理直传 | GPU 名称解析为 Adreno 编号 ≥800 时自动请求；还必须支持 `textureCompressionBC`、具体格式的采样/线性过滤/传输及图像尺寸。存储图像等仍解码 | 机型名称门槛是可推广候选；应以能力检测加代表设备对照取代，不能删除能力检查。TNA/其他 GPU 本次未验收 |
| 高通系统驱动的 UBO / ballot 改写 | 实际 driver ID 为 Qualcomm proprietary；ballot 还要求计算阶段 subgroup arithmetic | 编译器兼容修正，保留。Turnip/Mali 不应盲目套用 |
| 动态 UBO | Turnip、显式启用或 UBO 版本化开启，且 binding 有序；可禁用 | 默认双槽开启 UBO 版本化后已覆盖其他驱动，不是仍锁高通某机型 |
| Mali 深度输入实验 | 精确 Mali-G610 MC6 vendor/device/driver/version/info、精度位、扩展能力匹配，并需显式实验标记 | 默认关闭，强烈保留；校准边界不能外推到所有 Mali |
| Turnip `sysmem` | 启动器正常 Turnip 路径统一设置；历史依据来自 Adreno 642L。诊断 `private-vulkan` 不强制它 | 属于驱动运行模式而非机型白名单；应单项测，不能把本次普通 Turnip 结果当作 sysmem 验收 |
| PSO 编译线程 | 按在线 CPU 核数：≥6 为3，≥4 为2，否则1；可覆盖、最多4 | 通用能力策略，保持 |

两次自动启动未呈现画面的驱动回退、subgroup 能力检查、采样器/格式限制和 GPU/驱动缓存身份均应保留。它们不是需要取消的性能锁。

主要代码：[`GraphicsDriverPolicy.java`](../dm/android/src/org/d2r/binaryprobe/GraphicsDriverPolicy.java)、[`GraphicsDriverSettings.java`](../dm/android/src/org/d2r/binaryprobe/GraphicsDriverSettings.java)、[`nvn_vk_slot_policy.h`](../dm/gfx/nvn/nvn_vk_slot_policy.h)、[`nvn_vk_profile.h`](../dm/gfx/nvn/nvn_vk_profile.h)、[`nvn_vk.c`](../dm/gfx/nvn/nvn_vk.c)、[`nvn_vk_texture_upload.inc`](../dm/gfx/nvn/nvn_vk_texture_upload.inc)、[`nvn_vk_pipeline.inc`](../dm/gfx/nvn/nvn_vk_pipeline.inc)。

## 本轮修复与证据

上游基线 `88b322158ae3d6100293de0470d4c43277710269`。

1. 移植 [3970f8b](https://github.com/jhaizhou-ops/d2r-mobile/commit/3970f8b07bcc09ae0cd97504431008b18d4efed7) 的 NaN 语义处理。增加指令边界、扩展集隔离、参数个数验证、失败原子性和开关缓存隔离。统一处理预热和正常加载；实际 dump 位于转换之后。
2. 移植 [43a14d3](https://github.com/jhaizhou-ops/d2r-mobile/commit/43a14d3bfdfd78c4a0344687e381af5a33741664) 的 acquire 等待/布局转换同步修复。原来的 `TOP_OF_PIPE` 不能衔接提交在 `TRANSFER` 的获取信号量等待。绘制预旋转路径使用 `COLOR_ATTACHMENT_OUTPUT` 对齐两者。
3. 独立实现应用侧预旋转。交换链选择窗口 `currentTransform`，90/270 度交换宽高；自编全屏三角形反求采样坐标，保留裁剪及客体 Y 原点。当前使用线性采样，原 blit 路径可能使用 cubic，因此不要据此声称输出逐像素相同。
4. `SUBOPTIMAL` 后在下一次录制前检查方向，弥补 180 度切换不触发 Surface 尺寸通知；重建前排空 GPU，清理 framebuffer、image view、描述符及桥接图像缓存。呈现描述符只在专用呈现槽的前次 fence 完成后更新。

PPG：`AVAY025422000548`，SM8750 / Adreno830，Android17，普通 Turnip Mesa26.2.3，双槽、固定 1.0 比例。未自动移动或攻击；用户后续有手动观察/移动，相关样本不作性能 A/B。

| 实验 | 结果 |
|---|---|
| 系统驱动 NaN 关/开/关/开 | 右上区域黑帧 522/961、515/960、402/960、450/960；未建立改善 |
| 实际最终 SPIR-V | 94 个导出模块全部通过 Vulkan1.1 校验；没有旧 FMin/FMax/FClamp；包含 NMin309 / NMax856 / NClamp719 |
| 上游 SHFL 候选，单独诊断覆盖8模块 | 504/961 →445/961 黑帧，未解决；已撤下，不进入默认实现 |
| Turnip 旧版全画面闪屏 | 已核对异常帧为短暂旋转/拉伸。静止样本跳过前120帧后：NaN开21帧、NaN关21帧、用户报障录像64帧 |
| 只修 acquire 屏障 | 仍有44帧短暂转置；不能标为闪屏修复 |
| 预旋转首轮 | 962帧录像，跳过前120帧后异常0；用户确认“目前没有再闪” |
| 反向横屏、默认启用及恢复方向 | 各段稳定录像异常0。发现并补上无尺寸变化的方向重建；随后切换只新增1次 SUBOPTIMAL，稳态不再增长 |
| 最终 r2 缓存清理版 | 961帧录像，跳过前120帧后短暂转置检出0；目标区域黑帧0；采样日志错误0。另用日志验证90→270→90度重建，两个方向采样均无错误，SUBOPTIMAL总计2次后稳定 |

全画面检测采用384×174 RGB均值差，与前后6帧均差>5且两侧互差<6；阈值由真实转置帧校准。旧阈值20会漏报，右上局部黑块统计也不能当作整屏闪烁验收。原始数据保留，未改写旧结果。有限营地样本和用户观察不替代长时间战斗验收。

## 验证与交付

- Android native Release 构建、APK原签名与16K对齐通过；只替换已知调试包的native库，Java/资源逐字一致。
- 合并主机回归通过：48/48 cases、4463/4463 checks，线程200、存档恢复73；新增 NaN 畸形输入/幂等/扩展集测试及20000随机case通过，ASan/UBSan独立检查通过。
- 本地6929模块审计通过，5573模块仅扩展opcode改变；164个候选转换后SPIR-V校验通过。设备最终94模块另行校验通过。
- 自编呈现 GLSL 经固定 NDK glslc 生成、spirv-val校验及重生成一致性检查通过。i18n表406行通过，literal扫描结果与修改前一致；`git diff --check`通过。
- 本轮没有在PPG开启同步验证层，不把上游Redmi的验证层结果当成本机证据；Apple/Metal测试未运行。
- 保留所有设备默认双槽、TNA跳过最终验收。系统驱动局部黑块根因仍未解决；Turnip战斗由用户继续验证。

本地证据：`D:/AndroidSwitch/analysis/upstream-android-20261023/`（审查、着色器审计、主机测试），`D:/AndroidSwitch/analysis/perf-runs/ppg-slots-20261023/`（录像、日志、APK与构建收据）。最终包目录 `apk-turnip-prerotate-r2`，native SHA256 `0ae893f7bde7098ecdafe3cd8cba77898b4e0e962be5ac87b9e679b191edab4c`。启动器的SM8750自动驱动例外未修改；本次设备通过 `private-vulkan` 使用Turnip，预旋转无需强制标记。
