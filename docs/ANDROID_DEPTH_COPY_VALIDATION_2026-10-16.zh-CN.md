# Android Vulkan 深度／模板复制回退

本批继续兼容性与性能总目标，工作位于 main，保持现有 Android/Metal 平台边界。
参考 ZeldaWWHDRecomp 的能力分级思路，独立实现 NVN 后端的 Vulkan 复制逻辑；
没有引入 GX2/Latte 转换器或复制其源码。本批主要补齐功能，不宣称提高游戏 FPS。

## 修复的边界

此前 `copy_t2t` 的缩放分支要求颜色 aspect，因此深度目标缩放直接中止。
现在对相同格式、不同图像的最近邻复制选择：

1. 等尺寸：`vkCmdCopyImage`；组合格式分别复制 depth、stencil。
2. 缩放且格式支持源/目标 blit：原生 `vkCmdBlitImage`，组合格式分别处理两个 aspect。
3. 深度格式不能 blit，但支持采样和深度附件：使用内置 Shader 绘制复制。

绘制回退处理 2D 图像及其 mip/数组层。按目标像素中心用整数计算源 texel，
深度通过 `gl_FragDepth` 写入；组合格式先将目标区域 stencil 置零，再用八次
位掩码绘制恢复 stencil。无需 `VK_EXT_shader_stencil_export`。它最多需要九次绘制，
不是零成本操作，只在原生 blit 不可用时默认启用。保留目标区域之外的深度与 stencil。

内部绘制不更改 guest 逻辑状态，结束时使 Vulkan 命令绑定缓存失效，
保证后续原绘制/计算重新绑定。源/目标视图、描述符、framebuffer 和 render pass
属于 submission，在真实 GPU fence 完成后回收；utility 模块、布局和兼容管线在设备生命周期内复用。
先结束原 render pass/occlusion query，避免内部复制绘制计入游戏遮挡查询。

未采取清空目标、跳过复制或跳过绘制的保底策略。不支持的语义仍明确中止：
自复制/格式重解释、深度线性过滤、无法绘制的维度/格式等。
已有 CPU 打包深度＋模板上传边界没有在本批实现；此次组合格式夹具由真实 GPU clear 生成。
绘制回退验证的是正常深度值 [0,1]，没有证明特殊浮点负值、NaN 或超范围深度的位级复制。

另外，`TextureReadTexels` 原先对组合格式以两个 aspect 同时执行 buffer/image 复制，
不符合 Vulkan 的分 aspect 传输要求。现分别读取深度和 stencil，并重组 NVN 的
D24S8 / D32FS8 格式，D32FS8 的 padding 清零，保留调用方行间 padding。

## 内置 Shader 的可复现构建

源码在 `dm/gfx/nvn/shaders/`；NDK 27.2.12479018 的 `glslc` 离线生成
`nvn_vk_depth_copy_spv.inc`，应用无需运行时 GLSL 编译器。
生成及检查均运行 `spirv-val --target-env vulkan1.1`；检查生成结果逐字节一致：

```powershell
python tools/android/generate_depth_copy_shaders.py --check
```

原生构建记录同时包含 GLSL 和生成 header 的哈希。
`DM_VK_DEPTH_COPY_DRAW=1` 仅供诊断，在可绘制格式上强制绕过原生 blit；正常运行按能力选择。

## 系统驱动 GPU 验证

设备：`A6BFUT1C07005372`，TNA-AN00，Android 14/API 34，Adreno 642L，系统驱动 ID 8。
原始证据位于仓库外 `D:/AndroidSwitch/analysis/compat-performance-20261016/depth-copy/`。

实际 NVN API 记录与 Vulkan 后端执行 `depth_copy_device_cases.inc`，
CPU 独立计算预期值，使用真实 `TextureReadTexelsStrided` 读回。

- 四种格式：D16、D32F、D24S8、D32FS8。
- 每种九个案例：放大、缩小、非整数比例、局部偏移、等尺寸直接复制、不同 mip、
  不同数组层、两层批量复制，以及 mip/数组层组合。
- 检查源纹理未变、目标区域结果及所有未覆盖区域、全部 mip/层、行间 padding。
- 使用按 tile/层/mip/案例确定的细粒度深度值，并覆盖 0 和 1。
- 对 D16/D24 的 CPU 量化预期允许最多一 LSB，D32F/模板值精确比较。
- 每个案例在 utility 前后使用相同的 guest 绘制绑定，并插入可观察的颜色清除，
  验证复制后管线/描述符/viewport 等状态恢复；每组 36 次颜色验证。
- 每个案例重置并读回真实遮挡计数，结果均只包含两次 guest 三角形（2,304 samples），
  每组 36 次检查，内部深度/模板复制绘制没有混入计数。
- 自动/强制绘制与命令缓存关/开共四组；检查日志证明缓存开关实际生效。
- 每组核对 138,240 个深度／模板 texel、622,080 字节，并保存颜色读回，
  每组文件总长 1,211,904 字节，四组 SHA-256 均为
  `828e7077bd08ebe32276f23e1b0c9c8d3f06d8ad7fd8108a6db259e500643ac3`。
- 每组四次等尺寸复制、32 次绘制缩放；四组全部通过。
  同时原有 120 个 attachment 案例及原始绘制/计算/真实写故障回归均通过。

**这台设备对上述四种深度格式均未提供 blit，因此自动路径实际走到了新增回退。**
这证明了系统驱动下真实缺失能力的替代实现，但没有验证原生深度 blit 分支；
原生分支和其他 GPU/驱动仍需后续设备验证，四组结果不能描述为“原生与绘制 blit 等价”。

```powershell
python dm/android/tests/run_attachment_device.py --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache --build D:/AndroidSwitch/build/d2r-system-driver-main --output D:/AndroidSwitch/analysis/depth-copy-new --depth-copy-checks --run-device --serial A6BFUT1C07005372
```

最终 GPU 证据：`device-precision/result.json`、`device.log`、`depth-1-device.log` 至
`depth-3-device.log`、四个 `depth-*.bin`。

## 应用交付与尚未完成范围

原生构建、签名及 16 KiB 对齐检查通过。使用原安装壳覆盖更新，仅替换原生库，
没有卸载或清数据；安装前已备份存档。原生库 SHA-256：
`c3ba42600919a85f221a0f3542f82cd568236e28958a64fb6f309960977eea08`。

APK：`D2R-Depth-Copy.apk`，SHA-256：
`ed9c720752e12d24e327aed02bcea89a7de2752ade5d7420bb700b6ab4705d50`。
构建记录：`native-final.json`、`D2R-Depth-Copy.build.json`。

覆盖更新后用系统驱动进入罗格营地，额外预热至少 120 帧、确认最近窗口没有管线编译，
再采集 60 秒实际 SurfaceFlinger 呈现：1,037 帧、59.906 秒，**17.294 FPS**；
帧间隔中位数 58.115 ms，p95 58.284 ms，最大 66.595 ms。13 次只读场景采样
均为区域 1、同一玩家和坐标。电池 41–42°C，系统热状态 2，未证明频率/温控稳定。
与此前 16.511 FPS 并非交错对照，不能据此归因或宣称提速。

查看角色菜单和营地截图，未见此前明显的人物拉伸。该营地日志没有 `DEPTH_COPY_PATH`，
因此这里只是完整游戏回归；新增深度缩放的实际功能证据来自上述系统驱动 GPU 夹具。
没有把夹具通过描述为完整战斗、所有模型或长期稳定性通过。

已核对当前进程映射原生库哈希与构建一致。UID 仍为 10200，ROMFS 大小仍为
8,973,693,207 字节，诊断标记均未启用，三对先前采样的实际 Shader 文件内容未变。
15 个存档及设置文件均保留，设置和按键内容未变；游戏正常更新 `KKK.d2s` 和元数据，
没有用旧备份覆盖正常进度。保留 `saves-before.tar` 与 `saves-after.tar`。

证据：`system-camp/`、`menu-warm.png`、`final-camp.png`、`delivery-validation.json`。
顶点格式审计、长期/战斗、原生深度 blit 与跨设备、主要 GPU 性能优化仍属于未完成范围。
