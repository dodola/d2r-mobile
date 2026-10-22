# D2R Android 性能优化交接（2026-10-22）

给后续接手的 agent。读完这份就能继续，不需要翻聊天记录。细节在各专题文档里，这里只给结论、状态、入口和坑。

## 1. 当前状态一句话

`main`（= `origin/main` = 分支 `port/android-integration-20261021`，提交 `526ae46`）已包含全部优化。
PPG-AN00（骁龙 8 Elite / Adreno 830）营地场景 100% 渲染比例：**36 → 约 45 FPS，60 FPS 目标没达到**。
**但所有 PPG 数据都是在低电量、被系统限频的状态下测的**，满频能到多少还不知道。TNA（Turnip）从未回归验证过这批改动。

## 2. 还没做的验证（优先级从高到低）

1. **满频基准**：PPG 电量 ≥60% 且 `cpu0 scaling_max_freq ≥ 3000000` 时跑 `bench-all.sh` 3 轮。先读 `/sys/devices/system/cpu/cpu{0,6}/cpufreq/scaling_max_freq` 和 `dumpsys battery`，不满足就不要测，数据不可比。限频时的粗估：中大核满频能把串行的约 23 ms 压到约 15 ms，接近 60 FPS，**这只是估算**。
2. **TNA（Turnip，SM7325，serial 未知）回归**：`ANDROID_SERIAL=<serial> analysis/perf-runs/ppg-1022/tna-regress.sh 2`（仓库副本在 `tools/android/perf_regress.sh`）。对每个带开关的优化做"全开 vs 单项关闭"，看营地 FPS、日志是否干净、截图与全关基线的差异（PPG 上噪声 0.45–0.56，高于 1.0 要看）。需要游戏已装好并停在角色选择界面。
3. **发布版 APK 未安装**：`analysis/d2r-mobile-port/release-94012/release-perf-20261022/D2R-Mobile.apk`（sha256 `8c8166ff…`，1.1.0-94012-perf，versionCode 3）。装上去会让 `run-as` 失效，标记文件和日志都用不了，**先问用户**。
4. 测完把结果写进 `docs/ANDROID_PPG_CPU_OPTIMIZATION_2026-10-22.zh-CN.md` 的"下一步"小节。

定时任务 `833f5211`（session-only，7 天过期）原本会自动检查设备，会话结束就没了。最近一次检查时只连着一台 ROD-W09 平板（MT8795Z，没装游戏），PPG 已掉线。

## 3. 已做的优化（全部已合入 main）

| 项 | 位置 | 效果/说明 |
|---|---|---|
| 脏页缺页合并 + 保护合并（fault-around / protect-around，默认 64 KB） | `dm/gfx/nvn/nvn_dirty.c` | **最大单项，36.7 → 45.4 FPS（+24%）**。营地每帧约 420 次写缺页 SIGSEGV + 840 次 mprotect ≈ 9 ms，两个线程还争抢进程 mmap 锁。≥128 KB 窗口因过度标脏反而更慢 |
| GPU 写过的页保持逐页精确 | `nvn_dirty_mark_gpu_written()`，`nvn_vk.c` 的 `mark_write()` 调用 | 代码审查发现：合并会把 GPU 产出的数据当作脏页用陈旧的 guest 内存覆盖 |
| storage_bias 误判修复 | `dm/gfx/nvn/nvn_vk_storage_bias.h` | OpCapability 的字面量恰好等于 SSBO 变量 ID，导致被"距离修复"的大顶点着色器不能走改写，每个 draw 拷贝 19.5 MB。扫描时跳过只含字面量的操作码 |
| runs 表缓存 | `dm/android/d2r_save_adapter.c` `live_runs_frame_cached()` | 冻结钩子每次 `process_vm_readv` 8 KB；现在按帧键 + 50 ms 上限缓存 |
| `shader_get` 直映缓存 + 着色器代码脏页戳 | `nvn_vk.c` | 减少每 draw 的查找和哈希 |
| 描述符集回收 | `nvn_vk.c` `descriptor_set_acquire/rewind` | 按布局分桶；某个绑定被跳过（驱动无 nullDescriptor）时**不回收**，否则会留着上一次的描述符 |
| 显示率感知出图节拍 + 默认固定 60 出图 | `dm/android/nvn_platform_present.inc`、`nvn_present_policy.h` | eco_fps 关闭时默认 60；只有窗口真跑 120 Hz 才用 25 ms 节拍。TNA 渲染率被系统限到 60，不走节拍 |
| find() 进程级原子缓存 | `nvn_dirty.c` `g_find_cache` | `_Thread_local` 版本因动态 TLS 解析反而慢，已换掉 |
| SM8750 保持系统驱动 | `GraphicsDriverPolicy.TURNIP_PRESENT_BUG_SOCS={"SM8750"}` | Turnip 在 SM8750 上旋转后出现转置/镜像花屏，系统驱动 0/1802 帧，Turnip 122–135/1802。根因在 Turnip WSI 内部，没找到；真修需要应用侧预旋转 |

回退开关（marker 文件放 `files/d2r-mobile/diagnostics/`，或设对应环境变量）：`disable-fault-around`、`disable-runs-cache`、`disable-shader-stamp`、`disable-shader-get-cache`、`disable-set-recycle`、`disable-storage-bias`、`disable-present-60fps`、`disable-present-cadence`、`enable-dynamic-ubo`。窗口大小：`fault-around.txt`、`protect-around.txt`。

## 4. 已排除的方向（没有新证据不要重提）

- **哈希帧缓冲缓存、覆盖率表默认开、color store skip、depth load skip、pass 合并**：只省带宽，sysmem 下 load/store 几乎免费；远端种子用单向 `PassKey` 哈希，转不过来。
- **分辨率档位 UI 行**：所有档位都更慢，只是画质选项。
- **跳过 GPU 回写**（每帧 2.4 MB、约 600 个 host_prep 页）：见 `ANDROID_PPG_CPU_OPTIMIZATION_2026-10-22.zh-CN.md`，没有安全收益。
- **swapchain 回收**、**关闭 HWC 叠加层**：都不是 Turnip 花屏的原因。
- **线程亲和、ADPF 提示**：对 PPG 无明显帮助。重叠模式（`finalize-wait=none`）修完缺页开销后只是追平串行，不更快。
- **Adreno 642L 上的 shader 微调**：无效；多槽重叠是数据竞争（带保护的 2 槽每帧约 510 次冲突，更慢；双缓冲主提交上下文每帧约 37 次冲突）。
- TNA 营地 GPU 侧：逐 draw 计时显示两个 PS（`5c1ca4e7c3e4657c` 8.2 ms，`38f344adf801b153` 6.8 ms）占一半，是 ALU/寄存器重的 5568 行 fp32 着色器；LOD bias、`--relax-float-ops` 都无收益，剩下只有削画质或降分辨率。详见 `ANDROID_GPU_DRAW_TIMING_2026-10-22.zh-CN.md`。TNA 的优化已被用户宣布结束。

## 5. 还开着的想法（都偏大或偏险）

- **第二个渲染提交槽 + 带版本的池拷贝**：PPG 上重放线程每帧约等 GPU 7.5 ms，因为只有一个渲染提交槽。直接加槽会引入冲突，必须配版本化的池拷贝。
- **深度预通道省略**：约 3.6 ms/帧（TNA 数据），需要保证画面一致。
- **SM8750 默认开 dynamic UBO**：只有约 +1.5%，且 Adreno 642L 上出过 Qualcomm 对动态 UBO 分量的 miscompile（片元着色器也受影响），不建议。
- **描述符 update template**：预计约 +2%。
- **应用侧预旋转**：真正解决 Turnip 在 SM8750 上的花屏，才能让 Turnip 在该机型重新成为可选。

## 6. 本地环境与入口

- 仓库：`D:\AndroidSwitch\runtime\d2r-mobile`；远端 `origin`=dodola/d2r-mobile，`upstream`=jhaizhou-ops，`vickwv` 若丢失用 `git remote add vickwv https://github.com/vickwv/d2r-mobile.git`。`gh` 未登录。
- 构建（native）：`cmake --build D:/AndroidSwitch/build/android-perf-port-20261021 -j8`，cmake 在 `$LOCALAPPDATA/Android/Sdk/cmake/3.22.1/bin`。
- 主机测试（WSL Ubuntu，gcc 13）：`python3 dm/android/tests/run_merge_checks.py --output /tmp/x`。该脚本把脏页窗口强制为 1，以保持精确语义断言；新增 `t_dirty_around.c`、`t_storage_bias_literals.c`、`t_present_policy.c`。
- 打包安装到 PPG：`analysis/perf-runs/ppg-1022/pack.sh <apk目录>`（debuggable APK + 叠加层 `analysis/hud-custom-20261022`）。叠加层不匹配会丢"自定义设置"和 FPS 显示，需用 WSL 里的 `generate_overlay.py` 重新生成。最近装在 PPG 的是 `apk-port-1022ao`/`ap`。
- 测量脚本：`analysis/perf-runs/ppg-1022/` 下 `run-world.sh <label> [标记]`（进世界、打印最后的 RENDER_PROFILE 窗口，并清理已知标记）、`bench-all.sh`、`tna-regress.sh`、`rot/flip_test.sh` + `glitch_scan.py`。仓库内副本：`tools/android/perf_run_world.sh`、`perf_bench_all.sh`、`perf_regress.sh`、`perf_glitch_scan.py`。
- 诊断日志：`guest-wait-diag` 标记会打印 `GUEST_WAIT`、`THREAD_RUSAGE`、`DIRTY_COST`、`NVN_CALLS`、`SYNC_TRACE`；`draw-timing` 标记打印逐 draw GPU 时间。
- 设备：PPG-AN00 serial `AVAY025422000548`（SM8750，Android 17，显示最高 144 Hz，游戏运行时系统把渲染范围压到 60）；TNA-AN00（SM7325，Adreno 642L，Turnip）serial 未知，另一台旧机 `A6BFUT1C07005372`。

## 7. 测量的坑（吃过亏）

- **`cpu-cycles:u` 看不到内核时间**：缺页/mprotect 的大头完全藏在里面。用 `getrusage`、`DIRTY_COST`、`vol_csw` 才看得到。simpleperf 在这些机器上只支持 `cpu-cycles:u`。
- **电量/温度直接决定 CPU 上限**。12% → 3–7% 时中大核 2.23 → 1.36–1.79 GHz，超大核被锁在 1.02 GHz（满频 4.47）。只有"同状态成对 A/B"的数字可信。USB 500 mA 在负载下充不进电，要墙充。
- 每次测之前先读 `scaling_max_freq` 和电量。
- 帧时间是游戏线程（约 9.7 ms）+ `nvn-translate`（约 13 ms）**串行锁步**（每帧 21 次 `DeviceFinalizeCommandHandle` 排空），不是并行。
- PPG 上**不要**用 `input keyevent BUTTON_START` 导航菜单（会切到手柄 HUD），**不要** `KEYCODE_SLEEP`（安全锁，无法解锁）。从触摸界面进游戏用比例点击。
- 改 C 源码时用 Edit 工具；用 Python heredoc 改会把 C 字符串里的 `\n` 变成真换行。文件可能是 CRLF，脚本里要保留。
- 编译开着 `-Werror=missing-field-initializers`；声明顺序（如 `descriptor_pool_create`、`gw`）会引起"未声明"错误。
- CONTRIBUTING.md 要求注释中英双语（中文 | English）。
- 提交带尾注 `Co-Authored-By`；没被明确要求不要推送。

## 8. 相关文档

- `ANDROID_PPG_CPU_OPTIMIZATION_2026-10-22.zh-CN.md`：PPG CPU 优化的数据和被否决的想法。
- `ANDROID_GPU_DRAW_TIMING_2026-10-22.zh-CN.md`：逐 draw GPU 计时方法与 TNA 结果。
- `ANDROID_INTEGRATION_PORT_2026-10-21.zh-CN.md`：从 vickwv/android-integration 移植了什么、没移植什么、为什么。
- `ANDROID_PERF_TURNIP_2026-10-21.zh-CN.md`：Turnip 相关性能。
