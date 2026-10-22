# D2R Android 性能优化交接（2026-10-23）

给后续接手的 agent。接着 [ANDROID_PERF_HANDOFF_2026-10-22.zh-CN.md](ANDROID_PERF_HANDOFF_2026-10-22.zh-CN.md) 读：昨天的内容（脏页合并、storage_bias 修复、runs 缓存、Turnip 花屏等）这里不重复。读完这份就能继续，不需要翻聊天记录。

## 1. 当前状态一句话

分支 `port/android-integration-20261021` 已包含今天全部工作（见 §2），**怪物多时的卡顿基本解决**：新区域真实游玩约 5300 帧，平均 53.8 FPS，没有低于 30 FPS 的窗口，进局后只有 6 次孤立卡顿（最长 108 ms）；之前同类场景是连续十几帧 65–150 ms。
下一个大项是**批次槽（多提交槽并行）**，方案已批准，**步骤 0 的诊断代码已提交但还没有在设备上跑出数据**（§6）。

## 2. 今天的提交（都在 `port/android-integration-20261021`）

| 提交 | 内容 |
|---|---|
| `9b89450` | HITCH 卡顿归因日志 + 异步 PSO |
| `cc809de` | Adreno 8xx 自动原生 BC 纹理直传 |
| `20cee9f` | HITCH 增加回放 CPU / 脏页缺页 / mprotect / 读盘 / 上下文切换字段 |
| `df80d24` | PSO 多线程编译（最多 3 个） |
| `7900daf` | 多槽冲突台账（步骤 0，不改行为） |

### 2.1 HITCH 日志

位置：`dm/gfx/nvn/nvn_vk.c` 出图处（`HITCH_DELTA` 宏附近）。相邻两次出图间隔 > 40 ms 且 > 近期均值 2 倍时打一行，每次运行最多 600 行：

```
[nvn-vulkan] HITCH frame=N dt_ms mean_ms pipeline_ms pso_sync pso_wait_ms shader_load_ms tex_decode_ms gpu_wait_ms
             replay_cpu_ms faults mprotect_ms majflt inblock vol_csw invol_csw
```

读法：`pso_wait_ms` 大 = 等管线编译；`tex_decode_ms` 大 = 纹理首次使用的 CPU 解码；`gpu_wait_ms` 大 = GPU 忙；`replay_cpu_ms` 远小于 `dt_ms` 且 GPU 等待也小 = 游戏线程在拖（逻辑或读盘，看 `majflt`/`inblock`）。
计数器每 60 帧清零，所以差值用 `HITCH_DELTA` 处理回绕。

### 2.2 异步 PSO

`dm/gfx/nvn/nvn_vk_pipeline.inc` 的 `pipeline_obtain(..., int *skipped)`：图形 draw 遇到仍在排队/编译的管线，每帧最多等 3 ms（`PW.async_budget_ns`），预算用完就**只跳过 `vkCmdDraw`**（管线/描述符绑定也跳过，其余绘制簿记照常），管线继续在后台编译，后面的帧自然用上。计算管线仍阻塞。
这改变了文件头原来的“从不跳过 draw”原则（注释已改）。代价：新内容出现时物体可能晚几帧才画出来（弹出），**视觉效果还没人主观评价过**。
开关：标记 `disable-pso-async`（→ `DM_DISABLE_PSO_ASYNC`）；预算 `DM_PSO_ASYNC_BUDGET_US` / 标记 `pso-async-budget-us.txt`。
日志：每 60 帧一行 `PSO_ASYNC ... skipped_draws_total=N budget_us=...`。

### 2.3 多线程编译

同文件：`pipeline_worker` 现在有最多 `PSO_WORKERS_MAX=4` 个线程，缺省 3（CPU ≥ 6），`DM_PSO_WORKERS` / 标记 `pso-workers.txt` 覆盖，1 = 原单线程。线程安全做法：
- `PW.build_lock` 串行化 `pipeline_shader`（共享 `WarmShader` 链表）和 `pipeline_build` 里 `shader_module` / `vertex_module` 的准备；真正耗时的 `vkCreate*Pipelines` 在锁外并行。
- `PC.cache_lock`（`nvn_vk_pipeline_cache_runtime.inc`）改为读写锁：创建管线取读锁，取缓存快照取写锁。`nvn_vk_depth_copy.h` 同步改。
- 只有 0 号线程负责落盘清单。
冷启动（清清单和驱动缓存）：被跳过的 draw 11749 → 约 2000，最长卡顿 1562 → 746 ms。

### 2.4 原生 BC

`nvn_vk.c`：`DM_BC_PASSTHROUGH` 未设置 = 自动，**仅 Adreno 8xx 开**（设备名 `Adreno (TM) %d` ≥ 800）。PPG 上总解码耗时 1826 → 约 750 ms，画面与 CPU 解码逐像素比较一致（差异只在动画）。强制开 `enable-bc-passthrough`，强制关 `disable-bc-passthrough`。
**未验证**：TNA（Adreno 642L）、Mali（本身无 BC）。不要直接放开到其他 GPU。

试过并还原的：给 `refresh_compressed_cpu` 加“原始 swizzle 字节没变就跳过”，校验 0 次不一致，但一次运行只能跳过约 300 个子资源，测不到收益。

## 3. 关键测量数据（PPG-AN00，电量 5–10%，限频，只作同状态对比）

- 怪物多场景（修复前）：35 条 HITCH，连续十几帧 `pso_wait_ms` 56–154 ms；PSO 按需编译 17–73 ms/条。
- 修复后新区域游玩：见 §1。按需编译平均 48 ms（最长 282 ms），共跳过 345 次绘制，`pso_wait_ms` 最大 3.3 ms。
- 进局阶段的大卡顿仍在（`tex_decode_ms` 140–370 ms、`shader_load_ms` 45–70 ms），发生在读取画面前后，用户看不到。
- 营地 100% 渲染比例仍约 45 FPS（限频），未因今天的改动变化。

## 4. 和 sydeEvans 版本的对比

用户手动对比：sydeEvans 的版本在高通设备上两个驱动帧数都更高、怪物多时不卡。仓库 `github.com/sydeEvans/d2r-android`（`org.d2r.android.port`），克隆在 `D:\AndroidSwitch\third-party\sydeEvans-d2r-android\`。

核心差别（来自代码调查，**部分是推断**）：它的 GPU 从不直接读 guest 内存镜像，UBO/顶点/索引/SSBO 在录制时拷进每批自己的缓冲（UBO arena + 带缓存的快照缓冲），guest 内存只在 fence 之后由一个先进先出的发布线程写回，所以能同时放 3 个批次；回放直接在游戏线程里做；录满 2 ms / 32 个 draw 就提前提交；UBO 用动态偏移加内容去重；大只读 SSBO 做脏页增量快照。
我们是 GPU 直接绑定每个 guest pool 的单一镜像（`VPool.v`），CPU 在录制时往同一块镜像写下一帧数据，所以多槽有数据竞争。

在设备上跑它：
- 数据包必须按设备 GPU 的 **subgroup 大小**翻译着色器：Adreno 用 64（`build_shaders.py --host-subgroup-size 64`），**Mali（subgroup 16）它不支持，启动即 `abort`**（`shader_profile_check`，`dm/android/native/nvn_vulkan_shader.inc:60`）。
- WSL Ubuntu 里构建的脚本在 `third-party/sydeEvans-d2r-android/`：`patch.sh`（改写 NSO）、`shaders.sh` / `build64.sh`（翻译着色器，约 2 分钟，输出 `reuse64`）、`assemble.sh`（汇编 8.88 GiB 数据包，约 7 分钟）。venv `~/sye-venv`，.NET 10 在 `~/dm-work/tools/dotnet`。输入复用 `analysis/parity-20261019/d2r-new` 的 ExeFS/RomFS。
- 推送 `adb push` 到 `/sdcard/Download/d2r-package.zip` 约 7 分钟，然后在它的启动器里用文件选择器导入（搜索框一个字符一个字符地输入，一次输入整串会乱序）；导入完会自动启动游戏。它的启动器里“驱动”一行点一下会在系统驱动和内置 Turnip 之间切换。
- PPG 上它能跑，营地约 42.5 FPS（同样 10% 电量限频）。

## 5. 设备

| 设备 | 说明 |
|---|---|
| PPG-AN00 `AVAY025422000548` | SM8750 / Adreno 830，Android 17。装有 `org.d2r.mobile`（debuggable，当前是 `apk-port-1023h`，带多线程编译）和 sydeEvans 的 `org.d2r.android.port`。电量常在 5–10%，USB 供电充不进去。 |
| ROD-W09 `AJTUUN3118H45415` | MT8795Z / Mali-G610，**无 BC、subgroup 16**。我们的版本角色选择场景约 19–22 FPS，没有角色进不了世界。sydeEvans 版本在它上面不能用。 |
| TNA-AN00 `A6BFUT1C07005372` | SM7325 / Adreno 642L。**今天的改动（异步 PSO、多线程、BC 自动）都没有在它上面回归过。** 当前未连接；曾导入 sydeEvans 的数据包，结果未知。 |

`/sdcard/Download/d2r-package.zip`（约 9.5 GB）在 PPG 和 TNA 上都还留着，导入成功后可以删。

## 6. 下一步：批次槽（方案已批准）

完整方案在 `C:\Users\dodol\.claude\plans\moonlit-greeting-liskov.md`（仓库外，下面是摘要）。目标：让第二个渲染槽与前一帧 GPU 重叠，消掉重放线程每帧约 7.5 ms 的等待，预期营地 45 → 55–60 FPS（**估算，需实测**）。

**步骤 0（已提交 `7900daf`，未出数据）**：`pool_conflict_wait` 的冲突台账。用 `render-slots.txt=2` 在 PPG 上跑一轮，读 `SLOT_CONFLICT pool=.. kind=.. n=.. avg_range_bytes=.. hit_pages_of_range=.. wait_ms=..`（每 60 帧随 `RENDER_SLOT_PROFILE` 打印）。门槛：冲突是否集中在少数小范围（文档称 pool 5 的 64 KiB 桶约 1.9 MB/帧），以及 `hit_pages_of_range` 是否显示大量页粒度假冲突（`SLOT_PAGE_SHIFT` 实际是 8，即 256 B）。若是假冲突，先收紧标记，可能不用拷贝就够。
**步骤 1**：录制期间不再整段持有 `G.lock`，发布线程在录制中也能 `submission_publish`；守卫改为等“发布完成”；计数器报告不再在完成期写镜像；`memory_snapshot_ownership` 和 trim 覆盖全部槽。门槛：单槽行为位对位不变。
**步骤 2**：UBO 版本化（每槽 UBO arena + `UNIFORM_BUFFER_DYNAMIC` 动态偏移 + 同批内容去重）。风险：Adreno 642L 上曾有 Qualcomm 对 UBO 动态分量索引的编译器问题。
**步骤 3**：顶点/索引/SSBO/拷贝源走每提交快照（复用 `cached_snapshot` / `temporary_named`），大只读 SSBO 沿用 `storage_bias` 或脏页增量。
**步骤 4**：先在 SM8750 默认 2 槽，其余保持 1；提前分块提交作为独立后续项。
失败判据：任一步 FPS 相对单槽同状态 A/B 下降，或画面与基线不符，立即回退该步开关。历史上两次失败（无保护的 2 槽、每 draw 私有拷贝掉到 7–11 FPS），按块批量是没试过的差别。

关键事实（行号为 `nvn_vk.c`）：镜像 CPU 写入点只有 `pool_upload_guest_span`（`:960`）、`OP_UPDATE_UB`（`:4276`）、ZCull 保存（`:4224`）、缓冲拷贝（`:1533`、`:2453`），完成期有计数器报告（`:1454`）；上传缓存 `G.pool_uploads` 是全局的（键 `(pool_serial, addr, len)`）；`G.lock` 在整个录制期持有（`sub_begin_ctx` 到 commit），完成线程发布会被卡住。

其他未做的想法（按价值）：多线程编译已做，进局阶段纹理解码移到后台线程、着色器加载后台预取、原生 BC 扩到更多 GPU（需逐机型验证）。

## 7. 坑（今天吃过亏）

- **工作区里有别的会话在同时改文件**（多游戏外壳 `dm/android/multigame/`、`gamepack.py`、`game_profile.*`、`app_jni.c`、`GameActivity.java`、`CMakeLists.txt`、`loader.c`、`patch_nso.py`）。**不要 `git add -A`**：我曾经一次把它们都提交进了我的提交，发现后用 `git reset --soft HEAD~1` 重新只提交了自己的文件。对 `app_jni.c` 只提交自己的 hunk 时，用 `git diff` 抽出 hunk 再 `git apply --cached`。我在那之后编译的包也包含了对方 `app_jni.c` 的未提交改动。
- 在 Bash 工具里用 Python heredoc 写 C 字符串里的 `\n` 会被吃成真换行。解决：用 `chr(92)` 拼，或写完后用 `re.sub` 修，别直接在源里写 `\\n`。文件是 CRLF，Python 改完要保持。
- Git Bash 会改写 `/mnt/d/...`、`/sdcard/...` 路径：调 `wsl` / `adb` 前设 `MSYS_NO_PATHCONV=1`；Python 在 Windows 侧看不到 `/tmp`，临时文件放到 `D:/AndroidSwitch/analysis/perf-runs/`。
- `perf_run_world.sh`（`analysis/perf-runs/ppg-1022/run-world.sh` 的仓库副本在 `tools/android/`）会清掉已知标记，但**不会清** `pso-workers.txt`、`verify-*`、以及你新加的标记；测完要手动 `rm`，否则影响下一轮。
- 低电量会让 CPU 被系统限频，**只有同状态成对 A/B 的数字可信**；每次测之前读 `scaling_max_freq` 和电量。
- 读日志要确认是哪个进程：`game-startup.log` 在每次启动时轮转为 `.previous.log`；用户在我留着运行的进程里继续玩，日志会接着长。判断“有没有怪物”看 `RENDER_PROFILE` 的 draw 数是否变化，不要只看有没有 HITCH。
- 自动模式的分类器曾拦下“打包 + 装机 + 跑设备”的组合命令（和之前反复做过的相同），之后又暂时不可用；遇到就拆成更小的命令，或让用户在终端里执行，不要绕过。
- PPG 不要用 `input keyevent BUTTON_START` 导航，不要 `KEYCODE_SLEEP`（安全锁，无法解锁）。
- 测试：WSL Ubuntu 里 `python3 dm/android/tests/run_merge_checks.py --output /tmp/x` 应为 `PASS 48/48`；native 构建 `cmake --build D:/AndroidSwitch/build/android-perf-port-20261021 -j8`（cmake 在 `$LOCALAPPDATA/Android/Sdk/cmake/3.22.1/bin`）；打包安装 `analysis/perf-runs/ppg-1022/pack.sh <名字>`。
- 提交带 `Co-Authored-By` 尾注；CONTRIBUTING 要求注释中英双语。

## 8. 还没验证

1. **TNA（Adreno 642L）对今天全部改动的回归**（异步 PSO、多线程编译、HITCH 日志的开销、BC 自动在它上面保持关闭）。
2. **PPG 充满电满频后的绝对数字**，以及批次槽的收益。
3. 异步 PSO 的**视觉弹出**主观感受。
4. 多线程编译在 CPU 核数较少的机型上的表现（缺省 CPU ≥ 6 才用 3 个线程）。
