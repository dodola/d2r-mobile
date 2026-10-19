# Android 性能记录 2026-10-20：提交槽流水线

设备 TNA-AN00（Adreno 642L，60 Hz），系统驱动，100% 画质，角色选择场景，冷机（Thermal Status 0）。

## 基线诊断（单渲染槽）

| 项目 | 数据 |
| --- | --- |
| 帧率 | 11.95 FPS（帧时间约 84 ms） |
| GPU 忙碌 | `PASS_TIME_TOTAL` 约 61.6 ms/帧 |
| CPU 等槽位 | `slot_wait_ms` 约 41 ms/帧 |
| 彩色 pass（rt 70b52d91c） | 约 38 ms/帧，385 次 draw，90% 为片元成本 |
| 纯深度 prepass | 约 4.8 ms/帧，同样 385 次 draw |
| 片元着色器调用 | 彩色 pass 2.34 M/帧，深度 pass 2.24 M/帧（屏幕 0.92 M 像素） |

热点着色器 `5c1ca4e7c3e4657c`（约占彩色 pass 一半，201 draw/帧）为 5568 行 SPIR-V，全 fp32，含 7 次 PCF、
16 次 texel buffer fetch、5 个循环。

### 已验证无效或不可行

| 尝试 | 结果 |
| --- | --- |
| 合并头发/模型 pass | 不存在可合并的两个 pass；纹理 transition（L1502）一次也未触发 |
| 深度 `EQUAL` 改 `GREATER_OR_EQUAL`（启用 LRZ） | 片元调用增加约 30%，说明彩色 pass 有约 25% 片元比 prepass 深度更近，画面会改变，不可用 |
| 纹理 LOD bias +4 | 彩色 pass 仅快约 10%，纹理带宽不是主因 |
| `spirv-opt -O` | 着色器几乎不变，耗时无变化 |
| `--relax-float-ops`（fp16） | 耗时无变化 |
| 去掉 1354 条 `NoContraction` | 耗时无变化 |

Eden 的着色器路径同样没有快速数学开关，最终优化交给驱动编译器，结论一致。

## 根因：渲染段共用一个槽位

所有渲染段都使用 `G.submission_slot`，present 单独使用 `submission_slot2`。第 N+1 段开始录制前必须等第 N 段在 GPU 上
完成并发布，CPU 与 GPU 因此串行（约 42 ms + 42 ms ≈ 84 ms）。

## 修改

- `nvn_vk.c`：渲染段改为环形槽位。数量由 `DM_RENDER_SLOTS` 决定（1 到 4，**默认 1，即原行为**）；额外槽位自行分配命令池和描述符内存。
- 每个提交开头加入一个全局内存屏障（`ALL_COMMANDS` 到 `ALL_COMMANDS`），保证后一帧等待前一帧的 GPU 写入。槽数为 1 时不加。
- `app_jni.c`：`diagnostics/render-slots.txt` 标记文件，内容为槽数；删除后使用默认值。
- 新增诊断（默认关闭）：PASSLOG 中每个 pass 的深度测试/写入/比较函数、模板配置和深度附件 load/store 操作；
  `lod-bias.txt`、`dump-shaders`、`shader-override/`（用离线 SPIR-V 覆盖指定着色器）。

## 结果（交错重复测量）

| 渲染槽数 | FPS |
| ---: | ---: |
| 1（原行为，重复 3 次） | 11.9 / 11.9 / 11.9 |
| 2 | 14.75 |
| 3 | 14.70 |

彩色 pass 耗时不变（约 2299 ms / 60 帧），属于纯流水线收益。2 槽后 GPU 受限（61.6 ms GPU，67.8 ms 帧时间），3 槽没有额外收益。
注意：设备过热时单次测量会偏低（出现过一次 9.4 FPS 的瞬态），对比必须交错重复。

## 更正：2 槽的 +24% 来自数据竞争，不能采用

随后加入页级冲突检测（每个 pool 为每个渲染槽记录 4 KiB/256 B 的读写位图；CPU 写镜像前若与其他在途槽位重叠，等待其 GPU fence，并计数）。

| 粒度 | 每 60 帧冲突数 | 2 槽 FPS |
| --- | ---: | ---: |
| 无检测（旧实现） | 未知 | 14.75 |
| 4 KiB 页 | 约 3200（约 53/帧） | 12.3 |
| 256 B | 约 2200（约 37/帧） | 11.7 |

粒度缩小后大部分冲突仍在，说明是真冲突：游戏在帧 N 的 GPU 仍在读取的同一段缓冲上写入帧 N+1 的数据。加冲突等待后 2 槽没有收益，
所以此前的 14.75 FPS 是靠 CPU 与 GPU 同时访问同一镜像换来的，不安全。这与历史记录“未版本化的双渲染上下文已排除”一致。
要安全重叠需要按帧版本化动态缓冲（每槽独立镜像或 GPU 有序暂存上传），之前的暂存上传尝试因每帧约 750 次上传导致帧率崩溃，改动面很大。

结论：渲染槽保持默认 1，多槽仅在带冲突检测的前提下可用，日志 `RENDER_SLOT_PROFILE conflicts=` 给出冲突计数。

## 渲染比例实测（单槽，系统驱动）

| 渲染比例 | FPS |
| --- | ---: |
| 100% | 约 12.4 |
| 90% | 约 13.4 |
| 75% | 约 15.2 |

75% 的像素数只有 100% 的 56%，帧率仅提高约 22%：单槽时帧时间约等于 CPU（约 42 ms）加 GPU，分辨率只影响 GPU 那一半。
因此 CPU 回放仍是主要杠杆。用户已确认中端设备可以默认使用 0.75 档。

## 验证状态与风险（默认保持 1 槽的原因）

- 角色选择场景：1 槽与 2 槽截图一致，无花屏或闪烁；日志无 FATAL、VK_ERROR 或 DEVICE_LOST；持续数分钟稳定。
- 未验证：进入游戏世界后的战斗、地图切换、存档。adb 注入的点击无法可靠进入游戏世界，需要手动验证。
- **数据竞争风险：** 多槽意味着帧 N+1 的 CPU 录制、pool 镜像刷新与仍在 GPU 上执行的帧 N 重叠。对严格遵守 NVN 栅栏的游戏理论上安全，但历史记录里“未做版本化的双渲染上下文”曾被排除，这里没有证据证明它在战斗、地图切换、存档中安全，因此默认不启用。
- 启用方式（需先在游戏内验证）：`adb shell run-as org.d2r.newprobe sh -c "printf 2 > files/d2r-mobile/diagnostics/render-slots.txt"`，退回删除该文件。
- 如果游戏内出现闪烁、贴图错乱或闪退，先写入 `render-slots.txt` 内容 `1` 退回原行为，再对比。

## 渲染正确性：系统驱动的雾气伪影（未解决，已定位到 pass 008）

角色选择场景里，系统 Adreno 驱动会画出块状白色马赛克（雾气区域），Turnip 上没有。量化指标：裁剪区内亮度大于 120 的像素占比，系统驱动 5–13%，Turnip 约 0.55%。

**排除项**（开关打开后伪影仍在）：存储省略、清除合并、`subres`、采样拷贝缓存、资源缓存、描述符缓存、屏障省略、`storage-bias`、`temp-pool`、`wait-elision`、`wb-stage`、`async-commit`、`dirty-uploads`、`cpu-uniform-snapshot`、`command-state-cache`、`driver-table-reuse`、`control-cache`；跳过 12 个热点像素着色器；`precise-sync` 关闭时变成随机彩色噪点（说明读到未定义数据）。深度比较采样器对 D16 阴影图改用最近邻也没有效果（该改动已撤回）。

**定位方法**：`diagnostics/dump-frame.txt` 写入帧号 N，每个颜色/深度 pass 结束后把第一个附件拷贝到诊断目录 `pass-<序号>-<宽>x<高>-fmt<格式>-bpp<n>-s<serial>.bin`。两种驱动各抓同一帧，用 `dm/tools/pass_dump_diff.py` 逐 pass 对比。

**结果**：54 个 pass 里，所有深度 pass（包括主深度缓冲 serial 29）两边完全一致；**第一个分歧的是 pass 008（640×360，R16F，serial 58，像素着色器 `60f3906f2280721c`）**，输入的主深度相同，输出在角色区域不同（系统驱动上角色轮廓是纯白，Turnip 上有明暗层次）。该着色器采样 4 张纹理：serial 29（主深度）、46（256×256 D16，4 层，级联阴影图，带比较）、56 和 57（3D），另外有 46 次 texel buffer fetch（常量数据，经过“无采样器 buffer 适配”和 `STORAGE_BIAS_MODULE` 改写）。下游的 pass 024、033、035（RGBA16F，serial 28）把差异放大，最终形成雾气伪影。

**下一步**：对 pass 008 的输入逐个替换/检查（texel buffer 的视图范围与对齐、3D 纹理的格式与视图、4 层阴影图的数组视图），并用 `shader-override` 逐段裁剪该着色器确认是编译器缺陷还是我们的适配问题。

## 驱动策略：已验证的 SoC 自动使用 Turnip（带崩溃回退）

由于系统驱动在 Adreno 642L 上存在渲染错误，`GraphicsDriverPolicy` 的自动模式改为：`SM7325`（API 31 及以上）使用内置 Turnip，其他 SoC 仍用系统驱动；显式选择始终优先，ADB 的 `private-vulkan*` 标记优先级更高。

自动选择 Turnip 之前先写 `files/d2r-mobile/driver-auto-pending`；首帧呈现或 Activity 正常销毁时删除。若下次启动时它仍在，说明上次没有走到首帧（崩溃或被划掉），记一次失败；**连续两次**才回退到系统驱动并持久化（`auto_turnip_failed`）。用户重新保存驱动选择会清除该状态。日志 `D2RDriver` 输出 `selection=… resolved=…`，回退时附带 `auto_fallback=1`。

| 驱动（Adreno 642L，角色选择） | 100% | 0.75 | 画面 |
| --- | ---: | ---: | --- |
| 系统驱动 | 约 12.4 FPS | 约 15.2 FPS | 雾气块状伪影 |
| Turnip（自动选择） | 约 11.0 FPS | 约 13.0 FPS | 干净 |

已验证：自动选择、首帧后清除标记、注入遗留标记后第 1 次仍用 Turnip、第 2 次回退。尚未验证：其他 SoC 的白名单（目前只有 SM7325 实测过）、在设备上真实崩溃的路径（只用 force-stop 和注入标记模拟）。

## 后续

- 剩余 GPU 空闲约 6 ms/帧，下一个瓶颈是彩色 pass 本身（片元受限）。
- 参考 Eden：Adreno 上用 libadrenotools 的 `adrenotools_patch_bcn` 原生支持 BCn，避免 CPU 解码成 RGBA8（需要 API 28+）；
  按 `driverID` 建立 quirk 表（Qualcomm 专有驱动的标志清单）。
- 单槽时 CPU 自身约 42 ms/帧，GPU 优化后需要继续降低 CPU 回放成本。
