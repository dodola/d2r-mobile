# PPG-AN00 CPU 侧优化与 60 FPS 调查（2026-10-22）

设备：PPG-AN00（骁龙 8 Elite / Adreno 830，Android 17），系统驱动，营地场景，渲染比例 1.0（100% 画面），已解锁出图节拍（`present-60fps`）。
**测量条件的重要说明**：全部测量时电量 12%–45%，系统把 CPU 频率封顶：中大核 `scaling_max=2.23 GHz`（硬件 3.53 GHz），超大核 cpu6/7 被锁死在 1.02 GHz（硬件 4.47 GHz）。绝对 FPS 在满电、低温时会更高，下面的**相对提升**才是可迁移的结论。

## 结论

营地场景 36 → 约 43 FPS（+19%），画面不变，没有降低任何画质。离 60 FPS 还差约 28%，在降频状态下做不到；在满频下可能做到（见最后一节）。

## 瓶颈在哪里

- 不是 GPU：渲染比例 1.0 与 0.70 帧率相同，GPU 每帧只忙约 13 ms。
- 是 CPU，而且是两个线程**串行**工作：游戏每帧提交后连续调用 21 次 `nvnDeviceFinalizeCommandHandle`，每次都排空翻译队列，所以帧时间 ≈ 游戏线程（约 9.7 ms）+ 回放线程 `nvn-translate`（约 13 ms）。游戏线程 62% 的时间在 futex 里等回放线程。
- 不排空、让两边重叠（`finalize-wait.txt=none`、`acquire-sync.txt=early`）反而慢约 7%：游戏线程能提前最多三帧，但回放线程的墙钟时间涨到约 20 ms，周期数不变（≈ 31.7 Mcycles/帧），等价于运行频率更低。绑核（回放线程 cpu7、游戏线程 cpu6）没有改善，ADPF 提示也没有。因此默认保持串行。

## 已落地（均有 A/B，默认开启，各带关闭开关）

| 改动 | 实测 | 关闭方式 |
| --- | ---: | --- |
| 存储索引补偿（`storage_bias`）对带距离能力修复的大顶点着色器生效。`OpCapability` 的枚举值恰好等于 SSBO 的变量 ID，被误判成"不支持的使用"，于是 19.5 MB 的只读顶点缓冲每个 draw 都被整块拷贝 | 34.6 → 38.0（+10%），快照 14.4 → 1.4 MB/帧 | `disable-storage-bias` |
| D2R 存档适配器的冻结钩子每次调用都 `process_vm_readv` 读 8 KB 运行表并哈希，改为每个呈现帧读一次 | 39.4 → 41.2（+4.5%） | `disable-runs-cache` |
| 着色器索引条目 → 已加载着色器的直接映射缓存，去掉每 draw 两次对约 130 个着色器的 `strcmp` 扫描 | 39.2 → 40.2（+2.5%） | `disable-shader-get-cache` |
| 着色器代码比较在代码页未被写过时跳过（脏页戳；池序列号防止复用） | 38.1 → 38.9（+2%） | `disable-shader-stamp` |
| 描述符集按布局复用，不再每个 miss 都 `vkAllocateDescriptorSets` | 41.5 → 42.2（+1.5%） | `disable-set-recycle` |
| 默认 60 FPS 呈现（省电模式关闭时），之前只会被游戏自己的 interval 封在 30 | 30.0 → 33–37 | `disable-present-60fps` |
| 其余：脏页范围查找缓存（去掉 TLS，用进程级原子指针）、`nvn_dirty_protect` 先读状态再 CAS | 噪声内 | — |

## 试过、不采用

- `finalize-wait=none` / `ticket`、`acquire-sync=early`：见上，更慢。
- ADPF 性能提示（目标 16.7 ms）、回放线程绑超大核：无变化。
- 动态 UBO 在系统驱动上（`enable-dynamic-ubo`）：描述符命中率 30% → 75%（再放宽非升序绑定到 89%），但回放线程总 CPU 不变，FPS 不变；保持关闭，仅留开关。
- 控制块的脏页戳：控制块不在被追踪的池里（0 / 200000），且需保留字节校验。
- 跳过全部 UI 方块（664 draw/帧）上界只有 +8%：UI 快路径每个 draw 约 2.7 µs，已经很便宜。
- 逐 draw GPU 时间戳在这个驱动上粒度太粗（约 1 µs），PPG 上不可用于排序。

## 稳定性

120 秒（7210 帧）营地走动 + 开关菜单：0 个异常帧、0 个错误、进程存活。TNA（Turnip）**没有重测**这批改动，下次接上 TNA 需要回归（`storage_bias` 修复、描述符复用、着色器缓存对所有驱动生效）。

## 诊断工具

`guest-wait-diag[=trace]`：游戏线程等待分解、`THREAD_RUSAGE`、`NVN_CALLS`、`FINALIZE_HANDLE`、`SYNC_TRACE`；`desc-diag`：描述符 miss 归因；`draw-timing`；`wsi-diag`。`analysis/perf-runs/ppg-1022/run-world.sh <label> [标记...]` 一键进入世界并输出最后几个 60 帧窗口。

## 下一步

先让手机充电并保持低温再测一遍。当前 CPU 被限制在约 63%（中大核）和 23%（超大核）的最高频率；仅按中大核频率比例估算，满频能把串行的 ≈23 ms 压到约 15 ms，也就是接近 60 FPS。需要实测确认，这只是估算。
