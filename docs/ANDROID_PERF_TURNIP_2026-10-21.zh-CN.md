# Android Turnip 性能记录（2026-10-21）

## 基线

- 设备：HONOR TNA-AN00，Adreno 642L。
- 驱动：Turnip Mesa 26.2.3，`git-fbc44a3bcc`，driver ID 18，`TU_DEBUG=sysmem`。
- 场景：同一营地存档，游戏内 `hud.valid=1`、`game=1`。
- 分辨率：800p 出图 1422×800；自动世界预算初始约 1229×692，热态可降至 853×480（0.60）。
- 测量：应用内 Vulkan timestamp、CPU 分段和 Surface 呈现间隔。AGI 的系统镜像没有暴露 Adreno GPU profiling counters；系统 Perfetto trace 可用，但不能读取硬件 GPU busy/idle 计数器。

## 已保留

1. Turnip 测试 APK包含验证过的 `libd2r_turnip.so` 与加载依赖，运行时核验 driver ID。
2. 全局 render-pass 缓存与已有 framebuffer/view 生命周期分离；稳态 render-pass 创建不再随资源缓存 flush 重复。
3. 格式正确的颜色附件覆盖探针：
   - RGBA8、`R32_UINT`、`B10G11R11_UFLOAT_PACK32` 使用各自哨兵；
   - key 包含附件 slot、mip、layer；
   - 每个探针只在所属 pass 结束时回读一次；
   - 新证据使用 `pass-coverage-v2-<identity>.tsv`，旧 green 不会被新版本读取。
4. 覆盖率扫描工具的 `--apply` 正确保留表头，并通过临时文件原子替换。
5. 动态 UBO descriptor：
   - 只对绑定号升序的 kind-2 UBO 启用；
   - descriptor 内容固定为池缓冲、零偏移和实际 UBO 长度；
   - 每 draw 偏移通过 `vkCmdBindDescriptorSets` 提交；
   - Turnip 不能用 `VK_WHOLE_SIZE` 代替实际 UBO 长度，否则 3D 世界会消失。
6. `COPY_VIEW` 热路径日志改为显式诊断开关。同一段日志从约 5.3 MB 降至约 0.42 MB。

## A/B 结论

### 绑定校验

动态 UBO 把 descriptor cache 从约 23,220 hit / 52,212 miss 提升到约 57,240 hit / 18,180 miss（60 帧）。`bind_ms` 只有小幅下降，帧率差异在噪声内。

在动态 UBO 上继续打开“逐绑定精确校验”，binding-plan hit 从约 109,140 提升到 163,380（60 帧），但 bind 时间仍约 7.0–7.1 ms/帧，帧率没有实质变化。精确模式继续默认关闭。

### 查询分流

本地已有跨连续绘制的遮挡查询批处理，因此当前场景只有约 49 次查询/帧。UI 和阴影跳过查询后，组合模式只降到约 47 次/帧；帧率和 GPU 时间均在噪声内。实验开关保留，默认关闭。

### 分辨率预算

同一热态营地：

| 档位 | 输出 | 世界 | FPS | GPU |
| --- | --- | --- | ---: | ---: |
| 800p + 自动预算 | 1422×800 | 1229×692 | 20.91 | 33.0 ms |
| 1080p + 自动预算 | 1920×1080 | 1398×787 | 19.06 | 38.5 ms |

800p 更适合该 Adreno 642L 设备。1080p 可作为画质档，但不建议作为性能默认。

### 附件流量

同一热态营地、60 帧窗口：

| 模式 | load | store | clear | 理论总量 |
| --- | ---: | ---: | ---: | ---: |
| 全部关闭 | 104.5 MB/帧 | 104.5 MB/帧 | 0 | 209 MB/帧 |
| 清屏合并 | 54.1 MB/帧 | 78.6 MB/帧 | 24.6 MB/帧 | 157.3 MB/帧 |
| store 省略 | 104.5 MB/帧 | 102.5 MB/帧 | 0 | 207 MB/帧 |
| 两者 | 50.8 MB/帧 | 77.0 MB/帧 | 26.1 MB/帧 | 153.9 MB/帧 |

清屏合并减少约 25% 的理论附件操作字节，但 GPU 时间无可见变化；store 省略在当前场景只再减少约 2 MB/帧。两者均不能作为帧率收益单独宣传。

### 失败或删除的候选

- 阴影图 1024²→768²：阴影 pass 从约 5.45 ms/帧降到 5.31 ms/帧，变化低于可见收益，且降低画质。实现已删除。
- pool-read 单条 memo：每帧只命中约 4 次，`pool_ms` 和帧率不变。实现已删除。
- linear 呈现过滤：同档位约 19.9 FPS，低于 cubic 的约 20.3 FPS；保留 cubic。

## 复现与边界

- 所有性能数据来自 Turnip，不用于推断系统 Adreno 驱动。
- 附件流量是“附件尺寸 × bpp × load/store 次数”的理论量，不是 GPU 物理带宽计数器。
- 热态为 Android thermal status 1–2；比较使用相同进程或相邻同热态窗口。
- 覆盖率表、深度 store 跳过和查询绕过仍不设为默认，先完成更多场景与反例验证。
