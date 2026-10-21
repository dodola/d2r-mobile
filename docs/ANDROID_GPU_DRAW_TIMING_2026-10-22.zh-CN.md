# Android 逐 draw GPU 计时与热点实测（2026-10-22）

设备：TNA-AN00（Adreno 642L），内置 Turnip（driver id 18，sysmem），角色选择界面，自动世界预算（主 pass 960×540），无任何覆盖。
基线 22.0–22.4 FPS，约 45 ms/帧。

## 新增诊断：逐 draw 计时

`diagnostics/draw-timing` 标记（`DM_DRAW_TIMING`，默认关）。每个 draw 之后写一个 BOTTOM_OF_PIPE 时间戳，每个 pass 开始写一个；
一个 draw 的耗时 = 与同一 pass 内上一个时间戳的差，所以 pass 之间的屏障/拷贝不会算到 draw 头上。需同时开 `profile-detail`
（即不设 `NOPROF`）才会按 60 帧窗口输出：

- `DRAW_TIME_TOTAL`：窗口内 pass 数、draw 数、归因总毫秒、被丢弃数（`DT_CAP`=4096 条/提交）。
- `DRAW_TIME_GROUP`：按 (VS, PS, pass 尺寸, 颜色附件数, 深度, 混合) 聚合，前 30 组。
- `DRAW_TIME_PS`：只按像素着色器聚合，前 20。
- `DRAW_TIME_TOP`：窗口内最贵的 24 个单独 draw（含顶点数、视口、ztest/zwrite）。

实现：`dm/gfx/nvn/nvn_vk_draw_timing.inc`，在 `nvn_vk.c` 里 5 处挂接。时间戳会把本来可重叠的相邻 draw 串行化：开启后 FPS 约 22→18.9，
pass 总 GPU 时间 1832→2021 ms（约 +10%）。**只用于排序，不当绝对值**。

## 一帧里的 GPU 时间（归因合计 31.7 ms）

| 类别 | ms/帧 | draw/帧 | 占比 |
| --- | ---: | ---: | ---: |
| 彩色几何（960×540，ztest、无 zwrite） | 20.9 | 373 | 66% |
| 全屏后处理（顶点数 ≤4） | 5.0 | 36 | 16% |
| 仅深度预通道 | 3.6 | 360 | 11% |
| 其余 46 个小组 | 2.3 | — | 7% |

热点集中在两个像素着色器，合计约占 GPU 一半：

| PS | ms/帧 | draw/帧 | 每 draw |
| --- | ---: | ---: | ---: |
| `5c1ca4e7c3e4657c`（VS `54f730a6…`） | 8.2 | 140 | 58.5 µs |
| `38f344adf801b153` | 6.8 | 154 | 44 µs |
| 深度预通道 `f57adeb0…` + `94949567…` | 3.4 | — | 13 / 6 µs |

单个最贵的 draw：约 300 顶点、铺满整个 960×540 的一次绘制，约 3.3 ms/帧（≈6.4 ns/像素），说明是逐像素着色成本，不是顶点成本。
后处理里最贵的一个是 480×270 的降采样，815 µs。

## 上界与杠杆实验（交替 A/B，同热态，每组 3 个 60 帧窗口）

| 实验 | FPS | 结论 |
| --- | ---: | --- |
| 基线（前后两次） | 22.2 / 22.3 | |
| 跳过 `5c1c…` 的全部 draw | 26.1 | 免费时上界 +3.9 |
| 跳过 `38f3…` | 25.6 | 上界 +3.4 |
| 跳过 `f57a…`（深度预通道） | 24.2 | 上界 +2.0 |
| 纹理 LOD 偏置 +2 / +4 | 23.9 / 24.0 | 纹理带宽至多占约 8%，主要不是带宽受限 |
| `spirv-opt --relax-float-ops`（RelaxedPrecision）覆盖 `5c1c`+`38f3` | 21.9–22.2 | 无收益；Turnip 与此前系统驱动结论一致 |

## 其他观察

- 每 60 帧：GPU 忙 2144 ms（35.7 ms/帧），GPU 空转 654 ms（10.9 ms/帧），GPU 区间 46.6 ms/帧，帧时间 52.8 ms（计时开启时）。
  单帧 5 个分块：0.8 / 6.0 / **19.1** / 5.8 / 2.4 ms，主彩色分块（3 pass、369 draw）前有 5.5 ms 空转，其上一块前 4.1 ms。
- 线程都落在大核（cpu4–7，2.4–2.5 GHz 上限）；`nvn-translate` 主线程只忙约 34%。CPU 录制不是瓶颈。
- 因此剩下的帧率空间要么是让这两个着色器做更少的事（改变画质：减光源/特性），要么是降分辨率（自动预算已在 0.60 下限）。

## 复现

```sh
# 设备上先写标记，再用 run-mobile.sh 冷启动；不设 NOPROF 才会输出窗口报告
NWIN=4 TIMEOUT=120 bash analysis/perf-runs/run-mobile.sh dt-1 draw-timing
# 上界实验：skip-ps.txt 内容为 16 位 PS key
NOPROF=1 NWIN=5 bash analysis/perf-runs/run-mobile.sh sk-5c1c skip-ps.txt=5c1ca4e7c3e4657c
```

注意 `run-mobile.sh` 不会清掉 `draw-timing`、`resolution-tier.txt`、`render-scale.txt`、`present-60fps`，需要手动删。
