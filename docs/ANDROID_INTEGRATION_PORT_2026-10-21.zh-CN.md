# android-integration 第二轮移植评估（2026-10-21）

参考分支：[vickwv/android-integration@062f14e](https://github.com/vickwv/d2r-mobile/tree/062f14ec7f1f4e1cbe7d5987ce578ce48bc58e61)。
上一轮见 [ANDROID_INTEGRATION_PORT_2026-10-20.zh-CN.md](ANDROID_INTEGRATION_PORT_2026-10-20.zh-CN.md)（基于 3da783c），Turnip 侧见
[ANDROID_PERF_TURNIP_2026-10-21.zh-CN.md](ANDROID_PERF_TURNIP_2026-10-21.zh-CN.md)。
本轮逐项比对 3da783c 之后到 062f14e 的性能提交，只移入对本地有意义且可验证的部分。

## 本轮移入

| 来源 | 本地实现 | 默认行为 |
| --- | --- | --- |
| `0cf4904` 25 ms 出图节拍 | `dm_android_cadence_base_hz()`：仅当窗口实际运行 120 Hz 模式时把基准设为 80 Hz，引导 interval 2 得到 25 ms（恰为 3 个 vsync）；其他模式保持 60 Hz。远端无条件设 80，在 60 Hz 面板上会交替 16.7/33.3 ms。`DM_PRESENT_HZ` 仍优先；`diagnostics/disable-present-cadence` 关闭。 | 自动，仅 120 Hz 窗口 |
| `f09e07f` `find()` 直接映射缓存 | `nvn_dirty.c`：线程私有、只缓存槽位指针，命中时按槽位**当前** live 与 base/end 校验。远端缓存 base/end 副本，槽位复用后会命中过期区间。 | 启用 |

验证：`t_present_policy` 43,672 项（含 ASan/UBSan），`run_merge_checks.py` 23 项 0 失败（含真实缺页/脏页并发 1000 轮），NDK 27.2 Release 构建通过。

## 实机结论（TNA-AN00，Adreno 642L，Turnip sysmem，角色选择界面）

- **节拍在本机不生效**：游戏运行时 `dumpsys display` 的 primary render range 为 (0, 60)，SurfaceFlinger 为 60.00 Hz；`PRESENT_POLICY` 日志 `base_hz=60`。Java 层没有请求任何显示模式，系统策略限制了渲染刷新率。节拍代码只在窗口真的跑 120 Hz 的设备/设置下启用，未在实机上验证 25 ms 路径。
- **帧缓冲缓存无对象**：稳态 `framebuffer_misses=0`、`view_misses=0`、`framebuffer_hits≈3100/60 帧`；创建数 807 是启动期累计。
- **瓶颈在 GPU**：默认配置 49 ms/帧，GPU 忙 30.5 ms/帧；`gpu_wait` 约 21 ms/帧。最大分组为 960×540 主颜色通道，3 pass、369 draw/帧，17.7 ms/帧（58%）；同尺寸仅深度分组 3.4 ms/帧。CPU 侧 bind 7.8 ms、pool 4.3 ms、lookup 2.9 ms 都被 GPU 等待覆盖。
- **分辨率档是画质选项，不是性能杠杆**（交替 A/B，同热态，每组 3 个 60 帧窗口）：

| 配置 | FPS |
| --- | ---: |
| 默认（自动世界预算） | 22.0–22.4 |
| 800p 档 | 17.9–18.1 |
| 1080p 档 | 15.4 |
| 原生档 | 11.9 |

## 已评估、不移入

| 远端项目 | 原因 |
| --- | --- |
| 逐通道覆盖率表默认开、颜色 store 跳过、深度 load 跳过、通道合并（`283e664`…`ce0e887`） | 远端自己写明收益是带宽/功耗，帧率不变；Turnip sysmem 下 load/store 近似空操作，本地此前附件模式 A/B 的 GPU 时间无变化。远端种子表的 key 是整个 `PassKey` 的单向哈希，字段与本地不同，且尺寸来自另一台设备，无法转换。深度 load 的哨兵不能证明 z-test 安全。 |
| 哈希帧缓冲缓存（`b5a9beb`） | 本地稳态 0 miss。 |
| 读出备忘 1024 槽、pool MRU（`284c832`、`ae260aa`） | 远端实测持平；本地同类 memo 已删除。 |
| 查询预重置（`b5a9beb`） | 远端实测 19.2→15.8 FPS；本地已有 `DM_QUERY_RESET_RANGE`，默认关。 |
| UI/阴影遮挡查询跳过（`8107112`、`a4b8959`） | 远端收益来自约 1981 查询/帧；本地已批处理到约 49/帧，A/B 在噪声内。 |
| 主提交上下文双缓冲（`3c8c7dd`） | 远端冷机 +2.3%、热机持平；本地 2 槽有约 37 次/帧缓冲冲突（数据竞争），保持 1 槽。 |
| 分辨率档位 UI 行 | 见上表，所有档位都慢于默认。 |
| 随包 PSO 种子（`afc4de0`） | 种子含设备/驱动/shader 身份，需在目标构建上现录；属发版流程，不是代码移植。 |

## 工具

- AGI 3.3.3（`C:\Program Files (x86)\agi`）可用于查看系统 Perfetto trace；本机默认走内置 Turnip，Adreno 硬件计数器不经系统驱动暴露，GPU 侧继续用应用内 Vulkan 时间戳（`pass-timing` + `profile-detail`）。
- 实机基线脚本：`analysis/perf-runs/run-mobile.sh <label> [marker=value ...]`（`NOPROF=1 NWIN=5`），日志在 `analysis/perf-runs/port-1021/`。
