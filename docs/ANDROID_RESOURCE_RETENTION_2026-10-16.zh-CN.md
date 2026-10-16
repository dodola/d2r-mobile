# 系统 Vulkan 驱动：跨提交复用附件对象

本批开始以真实游戏性能为主要目标。保持系统驱动、原始 Shader、1.0 渲染比例，
不省略绘制或改变同步、回写语义。目标设备为 TNA-AN00 / Android 14 / Adreno 642L，
序列号 `A6BFUT1C07005372`，Qualcomm 系统驱动 ID 8。

## 定位与实现

30 秒 DWARF simpleperf 采样把 `vkCreateFramebuffer`、`vkDestroyFramebuffer` 和驱动
内存分配/释放定位为 CPU 热点。此前图像视图、RenderPass、Framebuffer 仅在一次提交内
缓存，营地每 60 帧仍创建 2,640 个 Framebuffer 和 2,160 个 ImageView。

现在每个提交槽位保留不可变对象；只有槽位真实 Fence 完成并重置命令池后，才再次使用。
复用键保留完整附件描述、图像子资源、尺寸、布局与依赖；clear 值仍由每次 BeginRenderPass
传入。销毁纹理时先 drain 两个槽位，再清除引用它们的 Framebuffer/RenderPass/ImageView，
最后销毁源图像。图形会话结束也清空缓存。

每槽最多保留 128 个 Pass、256 个视图；超过阈值在下一次安全 begin 回收。
数组额外预留这些保留项，原有每提交 4,096 个新对象容量不受挤占。
默认开启，`DM_VK_RETAIN_RESOURCES=0` 可关闭；原有 `DM_DISABLE_RESOURCE_CACHE` 同时关闭复用。
`resource-control.txt` 仅供已显式启用的诊断进程，在提交边界切换，普通启动不轮询。

## 验证入口

```powershell
python -X utf8 dm/android/tests/run_attachment_device.py --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache --build D:/AndroidSwitch/build/d2r-system-driver-main --output <新的证据目录> --resource-checks --run-device --serial A6BFUT1C07005372
```

专项测试以独立预期颜色检查完整像素：off/on/on/off、改变 clear 值、交替目标、
同地址和存储纹理销毁重建四次、单提交 270 层超过两个缓存阈值。每种命令缓存状态
374 次新增绘制、6,127,616 字节读回；两种状态共 748 次、12,255,232 字节，结果一致。
同时执行既有 120 个附件案例与基础 Compute/真实页写入故障/紧凑记录检查。

原始证据根目录：`D:/AndroidSwitch/analysis/compat-performance-20261016/cpu-sampling/`。
`resources-gpu-2` 验证诊断开启候选；默认开启版本另存结果，不混用构建身份。

真实游戏用 `compare_resource_retention.py` 同进程交错切换。该工具核对系统驱动 ID、
玩家/区域/坐标、无混合配置窗口，排除任何存在管线创建的样本；FPS 取 SurfaceFlinger
实际呈现时间戳。排除样本保留在原目录，不能解释为游戏崩溃。

## 结果与交付

同进程六个有效 60 秒样本（`resource-ab-2` 前五轮及 `resource-ab-3` 补测）：

| 缓存保留 | 三轮实际 FPS | 中位数 |
|---|---|---|
| 关闭 | 17.2401 / 17.8656 / 17.8603 | 17.8603 |
| 开启 | 19.2946 / 19.2507 / 19.2857 | 19.2857 |

中位数提升 **7.98%**。最早的独立配对为 16.9084 → 19.2504；由于关闭样本随后变快，
不能只选择这组并把 14% 描述为稳定提升。所有有效轮次玩家、区域和坐标一致，
温度 40–41°C、thermal status 2；未证明热稳定。开启时窗口内视图和 Framebuffer 新建均为 0。
每 60 帧 replay 约 1,439–1,506 ms → 1,133–1,135 ms，retire 约 308–311 ms → 134–139 ms。
这些是 CPU 阶段统计，不能与 FPS 直接相加换算。

`resource-ab-1/2-retain-0`、`resource-ab-2/5-retain-1` 有新管线创建，保留并排除。
`resource-ab-summary.json` 保存有效样本来源和计算。

默认开启版本的 `resources-gpu-default` 重新通过上述全部专项和基础检查。
覆盖安装后，普通模式（无 resource-control 标记）营地 60.080 秒实测 **19.324 FPS**，
20 个窗口无管线创建，P95 帧间隔 58.125 ms。前一次部署采样 `default-camp` 含一次
12.795 ms 管线创建，保留记录；交付核对采用 `default-camp-2`。
截图中人物没有明显拉伸；这不能代替战斗、长期或跨设备验证。

`retention-delivery-validation.json` 确认设备实际映射库与源码构建、签名 APK 一致，
UID 10200、8,973,693,207 字节 ROMFS 和 15 个存档/设置文件保留；仅正常游戏过程的
KKK.d2s.meta 改变，三个主 Shader 与原始备份一致，仍为驱动 ID 8。

- 原生库 SHA-256：`ad2a5cd361e38c1a1735156a0ed718c8853351a08224d91fcb019975e507fe81`
- APK：`D2R-Resource-Default.apk`
- APK SHA-256：`29c0016ebb7873afc4ad40ce2ae6b881324a4240725662e5028278ae699055b5`

开启后的 30 秒 DWARF 采样共 6,818 样本、0 丢失，已不再以 Framebuffer 创建/销毁为主要热点。
后续主要关注绑定、pool_read/写保护和实际 GPU 工作。本批尚未达到 30 FPS，性能目标继续。
