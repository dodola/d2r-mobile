# 上游 Android 功能移植 · 2026-10-28

English: [UPSTREAM_ANDROID_PORT_2026-10-28.md](UPSTREAM_ANDROID_PORT_2026-10-28.md)

对照本地 main `766ef032d1a34f51517bf5e6eee8d9b26f81ac64`，吸收 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) main `1e0e7934c9ccdf95494aee5065d1d07ab3cc3918`（Android 2.1.0）的行为。上游旧 android 分支已删除。本次适配本地 C Vulkan 后端，沿用 `.vkmeta/.spv`、资源导入、预热清单、应用及发布签名契约。

## 完成审计

- [x] 共享 UI：全屏地图选择在面板关闭/换区后恢复，不修改玩家配置；过场后恢复小地图；加宽仓库还原；手柄提示清理及八项上限。
- [x] Adreno：三步运行时内联，游戏/预热/工作线程同一路径，750–799 默认启用，标记和环境变量覆盖，着色器 ABI/缓存身份更新。
- [x] 管线诊断：完整稳定状态键，驱动/着色器身份隔离，逐线程持久记号，连续两次遗留启动才隔离，成功/手动恢复和错误分类。
- [x] 绘制完整性：普通需求始终等待，后台预热保留；缺效果降级须显式启用。
- [x] 现有等价功能：保留并验证 R8/RG8 BC4/5、30 FPS、NaN、Android 栈分配和驱动隔离清单。
- [x] 相机：默认原版，肩后/第一人称设置，Vulkan 常量副本与帧间矩阵，输入/名称/血条转换、避墙/高度及剔除例外。
- [x] 相机前端：右侧拖动、转向按钮、可选传感器、屏外怪物和地图方向、前后台/窗口/菜单/弹窗生命周期。
- [x] 构建验证：CPU 回归、i18n/模式/布局门禁、Android 原生链接、完整签名 APK 和可用设备原生检查。
- [x] 双语说明、上游署名及测试证据。完整游戏验收边界见下文。

## 来源及适配

地图来自 `195ae11` / `d1275c7`，仓库来自 `985e5ec`，提示来自 `d93985b`，内联/管线参考 `2428e5b`，相机来自 `dc5778d`。`dm/third_party/spirv-tools` 保留上游源码及许可证。相机纯数学保留上游算法，接入本地不可变 UBO 快照，不改客体内存。相机操作及架构见 [镜头说明](over-the-shoulder-camera.zh-CN.md)。

没有照搬上游 quad/warp32 开关。本地 Qualcomm 链是 compute ballot 的等价 OR 归约和 skin UBO 的 vec4 读取后分量提取，与上游重写不同，保留既有 driver/subgroup 能力筛选。helper 内联在本地适配及 NaN 修正后执行，预热和正常加载一致。本地 BC4/5 已采用一/二字节布局，未引入上游全局 BC 回退及其 mip 尺寸遗漏。

## 开关及诊断

- 内联优先级：缓存目录 `gfx-inline` > `DM_VK_INLINE_HELPERS` > Qualcomm Adreno 750–799 默认值。文件首字节 `0` 关闭，其他值打开。ABI 5、NaN/内联位进入缓存及清单身份；后续改写变化须继续更新 ABI。
- 管线记录为缓存目录 `pipe-guard-<identity>.*`。身份包含设备/驱动 UUID/shader index/选项，键包含完整稳定状态。同一启动多个线程遗留相同键只计一次；中间无相同记号的启动会打断未隔离累计。
- 只有明确返回 `VK_ERROR_INVALID_SHADER_NV` 立即持久隔离。OOM/device lost/未知错误不拉黑。隔离需求默认明确失败；`DM_VK_PIPE_DEGRADE=1` 或 `gfx-pipe-degrade` 内容 `1` 才允许缺绘制/派发。
- 建立 `gfx-pipe-reset` 后下次初始化清空当前身份记录；`DM_VK_PIPE_RETRY=1` 临时重试。驱动/配置变更自动隔离旧记录。

## 实际验证

Windows NDK 27.2 / Android API 29 arm64 Release 构建成功，包含静态 C++ 运行库、Java/JNI 及完整触控覆盖层。APK 使用现有发布签名，验证 204 项载荷。生成资源、游戏内容、签名材料和 APK 均未加入 Git。

| 检查 | 结果 |
|---|---|
| `run_upstream_port_checks.py` | 10 组：地图、仓库、相机状态/数学/接入/触控、管线守卫/等待、BC8、真实内联器；除优化器组外启用 ASan/UBSan |
| 需求等待 | 生产函数：图形/计算工人延迟 30 ms 仍获得管线，同步回退通过 |
| BC8 | 324 例，BC4/5 有/无符号、1–9 像素尺寸、输出守卫 |
| `run_merge_checks.py` | 全部通过；触控 49 例/4474 检查、NaN 20,000 例、脏页/队列/省电/存档恢复等 |
| `run_pipeline_checks.py` | 普通及 ASan/UBSan 各 415 检查 |
| i18n/模式/布局 | literal scan、484 行生成表、mode scan、两项布局检查通过 |
| Honor TNA_AN00 / Adreno 642L | 真实库设置夹具 188 检查；含相机三档应用/丢弃/默认/重读 |
| 同一手机 Vulkan | 合成 subgroup helper 经生产内联器后真实派发，32 个 lane 输出全部为 32 |
| 实际本地 shader 包 | 6,929 个模块中 187 个含 helper，全部成功内联/验证且描述符装饰保持一致 |
| 设备纹理视图 ABI | 6,201 检查通过，真实 NVN 入口与原生 thunk |

Linux/WSL 复现：以 `cmake -S dm/third_party/spirv-tools -B <spirv-build>` 配置并构建 `dm_spirv_opt`，然后 `python3 dm/android/tests/run_upstream_port_checks.py --out <receipt-dir> --spirv-lib <spirv-build>/libdm_spirv_opt.a`。设备目标为 `dm_android_inline_probe` 及 `dm_android_d2r_custom_probe`；后者传入原生库路径。

机器证据在 checkout 外 `analysis/upstream-android-port-20261028/`：`final-cpu/result.json`、`final-merge/result.json`、`final-pipeline/result.json`、`device-custom.log`、`device-inline.log`、`device-view.log`、`local-shaders.json`、`apk-build.log`。上游分析及固定基线见 `analysis/upstream-android-review-20261028/REPORT.zh-CN.md`。

## 验证边界

未验收 Adreno 750、Mali、iOS/Metal；未执行完整手机游戏流程、游戏镜头截图、全区域/战斗/过场、传感器真人操作或原版真值比较。设置夹具使用合成客体和真实库，Vulkan 探针使用合成着色器，均不等于游戏画面通过。手机应用及存档没有被覆盖、清除或卸载。相机默认关闭，完整视觉及玩法验收仍应在发布前完成。
