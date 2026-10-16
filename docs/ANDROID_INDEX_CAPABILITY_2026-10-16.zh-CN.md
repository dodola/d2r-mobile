# Android 系统 Vulkan：完整 32 位索引能力与 GPU 生产者验证

本批从 main `d075eea` 开始。完整兼容性与性能目标仍进行中，本文只记录这一批。

## 发现与修正

NVN 后端已经接收 32 位索引，并将 guest 的 `base_vertex` 传给 `vkCmdDrawIndexed`，
但创建 Vulkan 逻辑设备时没有启用已支持的 `fullDrawIndexUint32`。
较大的原始索引可以通过负 base vertex 对应到很小的有效顶点范围；不能用顶点缓冲大小推断原始索引只使用低位。

现在仅在设备支持时启用这项核心 feature，记录支持／启用状态及物理设备的最大索引值。
启用状态加入现有管线缓存身份的 bit 14，与旧缓存隔离；原有 bit 0–13 含义不变。
不增加逐索引 CPU 扫描、GPU 读回等待或每次绘制的新命令。

这是一项能力协商修正。没有证据表明此前真实游戏的特定错误由高位索引引起，也不据此声称 FPS 提升。
不支持完整 32 位索引的设备仍需后续实现适当回退；本批没有增加这样的回退。

## GPU 生成索引的完整边界仍未解决

本批研究了核心 `robustBufferAccess` 与 `vkCmdBindVertexBuffers2EXT(pSizes)`：

- 基础 robustness 的越界顶点读允许返回底层缓冲所绑定内存范围内的其他值，并不保证返回零。
- 顶点检查范围还允许向下舍入到 stride 的整数倍，需要特别处理紧凑末尾顶点。
- robustness2 才规定越界顶点读取返回零并填充缺失组件；当前手机未公布该扩展。
- 因而不能把“启用基础 robustness＋传入大小”当成 guest 逻辑子范围的完整隔离方案。
- CPU 副本可能早于 Compute 写入，不能扫描这个副本来替代真实 GPU 索引检查。

本批没有启用基础 robustness，也没有把越界输入送到未提供相应保证的 GPU。
测试中的有效顶点访问全部保持在绑定范围内；高位原始索引测试不等于完整的越界保护验收。

规范参考：

- [fullDrawIndexUint32](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#features-fullDrawIndexUint32)
- [Robust Buffer Access](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#shaders-robust-buffer-access)
- [vkCmdBindVertexBuffers2](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#vkCmdBindVertexBuffers2)

## 本机实际后端测试

设备：A6BFUT1C07005372，TNA-AN00，Android 14，Adreno 642L。
使用 Qualcomm 系统驱动 ID 8，构建 `923a446bf8, I23d7d7df15, 1725536844`。

运行：

```powershell
python -X utf8 dm/android/tests/run_attachment_device.py `
  --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port `
  --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache `
  --build D:/AndroidSwitch/build/d2r-system-driver-main `
  --output <新的输出目录> --index-checks `
  --run-device --serial A6BFUT1C07005372
```

新增 fixture 使用自行编写的 Shader，经 NVN recorder、实际 Vulkan 后端、系统驱动执行。
分别使用 CPU 上传和 Compute 写入的索引，GPU 生产者开始前把 CPU 索引副本写为退化三角形数据。
覆盖：

| 索引类型 | 原始首索引 | base vertex | stride |
|---|---:|---:|---:|
| uint16 | 0 | 0 | 16 |
| uint16 | 65,533 | −65,533 | 16 |
| uint32 | 0 | 0 | 16 |
| uint32 | 0x00FFFFFE | −0x00FFFFFE | 16 |
| uint32 | 0x7FFFFFFE | −0x7FFFFFFE | 16 |
| uint32 | 0x80000000 | INT32_MIN | 16 |
| uint32 | 0xFFFFFFFD | INT32_MIN | 0 |

每例连续使用三个索引。最后一例因此包含 `0xFFFFFFFF`，primitive restart 关闭；
使用 stride 0 和程序生成的位置，所有实际属性读取仍只访问一个合法元素。
高位索引的 VertexIndex 同时参与几何位置计算，不仅检查最后的 CPU 内存副本。

每种命令缓存状态在同进程运行分块阈值 0／16／48，两个目标交替绘制以产生实际 Pass 边界。
每例 64 次绘制，观察到提前提交分别为 0／3／1 次。
每个目标独立检查全部像素的预期颜色，再与控制组逐字节比较。
GPU 写回的索引、范围内尾部填充及两侧 canary 也检查一致。

最终证据目录：
`D:/AndroidSwitch/analysis/compat-performance-20261016/vertex-bounds/full-index/gpu-2/`。

- 两种缓存状态共 **5,376 次索引绘制、42 次 Compute**。
- 共 **2,752,512 字节**目标读回，两种状态分别为 1,376,256 字节。
- 每组 SHA-256 均为 `294c1436d8298c08d99ce5d4f1d3317d0dadcdfeeeb0e242342be61dd4de4024`。
- 两种状态也各通过既有 120 个附件案例及基础 Compute／绘制／真实页写入故障／紧凑命令检查。
- 启用和关闭命令缓存均由运行日志验证，索引绑定省略路径确实触发。
- 测试专用能力读取入口仅在 `DM_VK_INDEX_TEST` 构建中存在。

第一次夹具 `gpu-1` 已通过所有阈值 0 的高位索引，但在阈值 16 的计数断言停止：
连续绘制同一目标没有触发允许分块的 Pass 边界。夹具改成目标交替后通过。
保留失败记录；这是夹具的计数前提错误，不把它描述成驱动崩溃或渲染错误。

## 真实游戏交付与剩余验收

原生更新产物、构建来源、安装前存档快照及设备状态保存在：
`D:/AndroidSwitch/analysis/compat-performance-20261016/vertex-bounds/full-index/`。

仅覆盖安装原 APK 的原生库，保持 Java、资源与签名体系，未卸载、未清除数据、未更换驱动。
启动日志已确认系统驱动 ID 8、完整索引 feature 启用、Pass 统计关闭。
真实游戏营地回归及部署核对结果以该目录的 `delivery-validation.json` 为准。

本机覆盖更新后 PID 5999，角色 KKK，区域 1；13 次场景采样确认位置保持不变。
实际呈现 1,000 帧／60.241637 秒，平均 **16.583215 FPS**，帧间隔中位数 58.098 ms，
P95 66.508 ms。16 个渲染统计窗口均无管线创建。
电池温度 40–41°C，thermal status 2；未证明热状态稳定，也没有受控的旧版／新版交错性能对照，
因此只作为回归数据，不声称提速或退化。截图中的人物没有明显拉伸，不能代替战斗与长期验证。

部署校验确认 UID 10200、ROMFS 8,973,693,207 字节、15 个存档／设置文件保留；
仅 `KKK.d2s.meta` 在正常游戏过程中改变。任务相关诊断／自定义驱动标记均未启用，
三个主 Shader 的压缩程序及元数据与原始备份一致。
已加载原生库 SHA-256 为 `8c9f472888c03743abb0ba86612ccef0f58756964f4cda9d963d9381bcf22e9f`，
与源码构建、签名与 16 KiB 对齐检查后的 APK 一致。

APK：`D2R-Full-Index.apk`，SHA-256：
`27cc689c8f48f85d14301703e86ed84e6563460df88c34792bc0ae76f80202b6`。

GPU 生成索引的完整顶点边界、未支持 feature 的设备回退、主要重型 Shader 性能、战斗／长期／跨设备验收仍未完成。
