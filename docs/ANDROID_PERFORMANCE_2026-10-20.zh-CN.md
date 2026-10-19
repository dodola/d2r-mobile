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

- `nvn_vk.c`：渲染段改为环形槽位。数量由 `DM_RENDER_SLOTS` 决定（1 到 4，默认 2）；额外槽位自行分配命令池和描述符内存。
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

## 验证状态

- 角色选择场景：1 槽与 2 槽截图一致，无花屏或闪烁；日志无 FATAL、VK_ERROR 或 DEVICE_LOST；持续数分钟稳定。
- 未验证：进入游戏世界后的战斗、地图切换、存档。adb 注入的点击无法可靠进入游戏世界，需要手动验证。
- 如果游戏内出现闪烁、贴图错乱或闪退，先写入 `render-slots.txt` 内容 `1` 退回原行为，再对比。

## 后续

- 剩余 GPU 空闲约 6 ms/帧，下一个瓶颈是彩色 pass 本身（片元受限）。
- 参考 Eden：Adreno 上用 libadrenotools 的 `adrenotools_patch_bcn` 原生支持 BCn，避免 CPU 解码成 RGBA8（需要 API 28+）；
  按 `driverID` 建立 quirk 表（Qualcomm 专有驱动的标志清单）。
- 单槽时 CPU 自身约 42 ms/帧，GPU 优化后需要继续降低 CPU 回放成本。
