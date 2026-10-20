# Android 性能记录 2026-10-20（二）：94012 着色器查找缓存与 Turnip 对照

设备 TNA-AN00（Adreno 642L，Android 14，60 Hz），冷机（Thermal Status 0），选角画面，`NOPROF=1`（不开诊断计时），
`org.d2r.mobile` 1.0.0-94012 的可调试版（与发布版同包名、同签名，`adb install -r` 保留数据）。

## 结论

| 项目 | 结论 |
| --- | --- |
| 最大 CPU 热点 | 94012（着色器索引 v2，完整代码标识）下 `shader_entry` 占 `nvn-translate` 线程 **50.8%**（系统驱动下 32.7%） |
| 原因 | 每次 draw 都对 2176 字节 control 和整段着色器代码做逐字节 FNV 哈希；为 91738 写的 `control_cache` 在 v2 路径里被提前 `return`，从未生效 |
| 修复 | `ShaderLookup` 查找缓存（提交 `b621728`），命中后仍做同样的两次 `memcmp`，语义不变 |
| 效果 | Turnip sysmem、100% 画质：**12.4 → 17.1 FPS（+37%）**，帧时间约 80 → 58 ms；命中率 99.5% |
| Turnip 驱动选项 | `gmem`、`nolrz`、`noubwc`、`forcebin` 均无收益，默认 `sysmem` 最优 |
| 驱动选择 | TNA：Turnip 比系统驱动快约 10%；PPG-AN00：Turnip 约 20 FPS，系统驱动 10–13 FPS（见下） |
| 下一个瓶颈 | 修复后 58 ms 里 GPU 等待约 32 ms，GPU pass 成为更大的一块；CPU 热点已很平 |

## 根因细节

`program_stage()`（`nvn_vk.c`，draw 路径）每次 draw 对顶点、像素阶段各查一次着色器，一帧约 940 次 draw，即约 **1870 次查找/帧**。
索引 v2 路径（`G.shader_code_identity || G.shader_index_version==2`）在 `program_stage_optional_impl` 里直接调用 `shader_entry()`，
绕过了其后的 `control_cache`。`shader_entry()` 在 v2 下的开销：

1. `control_hash()`：对 0x880 字节逐字节 FNV；
2. `shader_code_hash()`：对整段着色器代码逐字节 FNV（FNV 乘法链无法向量化，代码越大越贵）；
3. 命中候选后再各做一次 `memcmp`（control、代码）。

## 修改

`dm/gfx/nvn/nvn_vk.c`：新增 `ShaderLookup`（2048 项，直接映射），键为（control 指针、阶段、代码地址、代码大小），值为索引条目。

- 命中时先检查键，再对该索引条目重新做 `memcmp(control, 0x880)` 与 `memcmp(代码, code_size)`，与原哈希查找之后的校验完全相同。
  游戏在同一地址改写着色器内容 → `memcmp` 失败 → 回退完整哈希查找并更新缓存，不会返回过期条目。
- 首次匹配语义不变：缓存存的是完整查找返回过的条目；相同字节必然得到相同哈希和探测序列。
- 不缓存未命中（NULL）结果；91738 的 v1 路径不变。
- 命中/未命中计入已有的 `RENDER_CACHE_PROFILE control_hits/control_misses`（v2 下此前恒为 0）。

## 测量参数

- 脚本：`analysis/perf-runs/run-mobile.sh <标签> render-scale.txt=<比例> private-vulkan-sysmem`（由 `run.sh` 改包名而来），
  `NOPROF=1 NWIN=3 TIMEOUT=100`。每次先清除全部诊断标记，再写入请求的标记，冷启动后取稳定窗口（约 56200 draw/60 帧）。
- 驱动标记：`private-vulkan-sysmem` = Turnip + `TU_DEBUG=sysmem`；`private-vulkan` + `turnip-debug.txt=<选项>` 完全控制 `TU_DEBUG`；
  无标记 = 系统驱动。
- 比例：`render-scale.txt` 写 `1.0` 或 `0.75`。
- 对比必须交错重复；每组的第一个窗口是过渡窗口（场景加载中），只看后面的稳定窗口。
- 采样：`$NDK/simpleperf/app_profiler.py -p org.d2r.mobile -r "-e cpu-clock -f 2000 -g --duration 12" --disable_adb_root -nb`。

## 基线（修复前，稳定窗口）

| 驱动 | 100% | 0.75 |
| --- | ---: | ---: |
| 系统驱动 | 11.6 / 11.6 | 12.8 / 12.9 |
| Turnip（sysmem） | 12.4 / 12.5 | 14.2 / 14.4 |

与 [ANDROID_PERFORMANCE_2026-10-20.zh-CN.md](ANDROID_PERFORMANCE_2026-10-20.zh-CN.md) 的 100% 基线（系统 12.4、Turnip 11.0，单槽约 84 ms/帧）一致。

## Turnip `TU_DEBUG` 对照（100%，稳定窗口）

| 选项 | FPS |
| --- | ---: |
| `sysmem`（基线，三次） | 12.5 / 12.5 / 12.3 |
| `gmem` | 11.7（-6%） |
| `sysmem,nolrz` | 12.6（持平） |
| `sysmem,noubwc` | 10.9（-12%） |
| `forcebin` | 12.5（持平） |

## 修复前后（Turnip sysmem，100%，三次运行）

| | 稳定窗口 FPS | 每帧 | gpu_wait/60 帧 |
| --- | ---: | ---: | ---: |
| 修复前 | 12.3 – 12.5 | 约 80 ms | 约 1.8 s |
| 修复后 | 16.97 / 17.15 / 17.15 | 约 58 ms | 约 1.93 s |

simpleperf（`nvn-translate` 线程）：修复前 `shader_entry` 50.8%、`memmove` 5.7%、`memcmp` 4.1%；修复后 `shader_entry` 不再出现，
`memmove` 11.6%、`memcmp` 11.4%（含缓存命中后的校验）、`bind_shader` 6.6%、`pool_read` 3.7%；该线程占全部采样从 39% 降到 31%。

验证：日志无 `STOP`/`Fatal signal`/`VK_ERROR`/`DEVICE_LOST`/`PROGRAM_MISSING`，崩溃缓冲区无新增；选角画面（含已有的 1 级术士）渲染正常。
**未覆盖**：进入游戏世界后的战斗、区域切换、存档（改动不改变校验语义，风险低，但尚未在世界内验证）。

## 其他设备：PPG-AN00（Adreno 830，Android 17）

同一发布包、选角画面（修复前）：

| 驱动 | FPS |
| --- | ---: |
| 系统驱动（自动推荐的实际选择） | 10.0 – 12.8 |
| 内置 Turnip | 19.7 – 20.7 |

此前记录里这台设备在 Turnip sysmem 下稳定 30 FPS（受游戏帧间隔限制）、解除限制后约 52 FPS，因此还有约三分之一差距未解释；
自动驱动策略现为"所有 SoC 用系统驱动"（`VERIFIED_TURNIP_SOCS` 为空），在该 SoC 上不是最优，是否加入白名单需要决定。
本次查找缓存修复未在 PPG 上复测。

已排除（PPG/TNA 的变慢原因）：清屏合并/省略写回开关（`attachment-mode.txt`）、Vulkan 验证层、设备全局 `gpu_debug_*` 设置、管线缓存预热（连续启动 5 次无变化）。

## 已知问题

- **息屏/锁屏崩溃（未定位）**：PPG 上游戏运行中设备息屏锁屏后，进程在 `nvn-translate` 线程 SIGABRT，栈为 `die ← mtl_present ← android_xrun ← translate`。
  推测与 Surface 在息屏时被销毁有关，尚无日志证实；发布版不可调试，需用 `--debuggable` 同签名版复现并读取 `game-startup.log` 的 `STOP` 行。
- 可调试版的 Java 主线程在 ART 解释器里占约 18–28% 采样（CheckJNI 等调试开销），该数字不代表发布版。

## 后续候选

1. 省掉命中后的着色器代码 `memcmp`（按 pool 写入代数判断代码区是否变化）。
2. 查 `memmove`（11.6%）的调用来源。
3. 修复后 GPU 等待约 32 ms/帧，下一步需要看 GPU pass（此前记录的每帧约 386 次模板 draw）。
4. 在世界内验证（战斗、区域切换、存档），再用此优化重新构建发布包。
