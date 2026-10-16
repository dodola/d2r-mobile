# Android Vulkan 顶点格式兼容与边界验证

本批继续兼容性与性能总目标，在 main 上实现。参考 ZeldaWWHDRecomp 的能力驱动设计，
未移植其 GX2/Latte Shader 或 GLSL 替换代码。保持现有 Android/Metal 平台边界。

## 实现

- 独立顶点格式映射，不扩大纹理格式支持范围。覆盖 R/RG/RGB/RGBA 的 8/16/32 位
  float、normalized、integer、scaled 格式，以及本批验证的七种打包格式，共 79 种。
- 查询 `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT`。RGB 不可直接获取时拆为 RG+B，保留
  3/6/12 字节的实际宽度；不以 RGBA 读取替代 RGB，不额外复制顶点数据。
- scaled 优先使用支持的原生 8/16 位格式；缺失时按相同宽度整数获取，再在 Shader
  中转换为 float。32 位 scaled 使用整数获取和转换，Vulkan 没有对应原生 scaled 格式。
- 从 SPIR-V 反射实际标量类型和向量宽度，校验 metadata 的 location；不依赖其保留字段。
- 受影响输入在入口重建至 Private 变量，保留原来的完整值读取、动态分量读取、默认
  z=0/w=1 和 BuiltIn 输入。重定型相关 AccessChain、InBoundsAccessChain、CopyObject
  及同存储类指针合流。复用类型声明，避免重复非聚合类型；重排全局变量避免前向类型引用。
- 惰性缓存适配模块；PSO 身份包含拆分及有符号/无符号转换位。持久化 Shader ABI 升至 2，
  旧管线缓存不匹配后正常重建。顶点原始 IR 与适配模块跟随现有设备生命周期保留，增加内存占用。
- 校验属性 offset、stride、分量对齐、额外 location 和设备限制。
  数组绘制及实例属性检查最后一次获取是否超出 NVN 绑定范围，计算避免 uint64 溢出。

## 系统驱动 GPU 验证

设备 A6BFUT1C07005372 / TNA-AN00 / Android 14 / Adreno 642L，驱动 ID 8，
Qualcomm proprietary，Build `923a446bf8, I23d7d7df15, 1725536844`。
实测普通 RGB 格式支持；8/16 位 scaled 缺失。自然路径实际验证整数转换，
RGB 替代路径通过 `DM_VK_VERTEX_SPLIT=1` 强制验证，不宣称本机自然触发 RGB 拆分。

使用真实 NVN 记录器和本 worktree Vulkan 后端，独立 CPU 解码建立预期值，RGBA32F 读回。

- 79 种格式 × 四种 Shader 输入宽度 × 四种布局，每轮 1,264 案例。
- 布局为紧密排列、带 offset/首顶点的交错排列、16 位索引及负 base vertex、
  32 位索引及非零 base instance 的实例属性。末尾属性同时抵达逻辑范围与实际池末端。
- Shader 同时读取完整输入及动态分量，保留 gl_VertexIndex；检查缺失分量默认值。
- 自然/强制拆分 × 命令缓存关闭/开启，四轮共 5,056 案例，读回 20,709,376 字节。
- 每轮原有 120 个 attachment 案例及基础 compute/draw/真实写故障检查也通过。
- 四轮读回 SHA256 均为
  `938b6190257866176094d16de787b265d19192ee67a9384c1200e309c606e88b`。
- 实际生成的 18 个适配模块全部通过 `spirv-val --target-env vulkan1.1`。
- 数组、交错数组、实例范围分别缩短一字节，均在绘制前以指定边界错误拒绝。
- 从实际游戏捕获的 25 个顶点模块，拆分/无符号转换/有符号转换组合共 75 个模块通过 IR 校验。
  三个另行构造的输入指针别名模块，九种适配结果通过 IR 校验；这项是结构验证，不宣称 GPU 别名回归。

最终证据：`D:/AndroidSwitch/analysis/compat-performance-20261016/vertex-input/device-packed/result.json`。
早期目录保留了重复类型问题和验证工具问题；不将其作为最终通过证据。

## 游戏与安装回归

原 Android shell 仅替换 native library，签名及 16 KiB 对齐检查通过，使用 `adb install -r -t`。
安装后实际映射库 SHA256：
`3320b534408b09a882ec1fa56e89f99b45e72c3dc3c58f92e77b093dcb335e45`。
APK：`D2R-Vertex-Compatibility.apk`，SHA256：
`76ec42d0469a20e58a9478df001454acf9bdd2ec3e6776098531705f6b8095be`。

系统驱动正常启动，角色选择与营地截图无明显拉伸；摇杆短距离移动后只读探针确认坐标变化，
仍在 area 1。采集阶段没有发送输入，GPU 夹具已结束。

营地实际呈现采集：1,011 帧 / 60.291489 秒 = **16.75195 FPS**；
帧间隔 median 58.118881 ms，p95 66.459688 ms，最大 116.212865 ms。
13 次采样确认相同玩家、区域与坐标；这是采样验证，不是连续状态证明。
电池 41–42°C、thermal status 2；未证明热稳定，未做交错 A/B，不宣称相较之前提速或回退。
营地日志未出现新顶点适配标记，新路径功能以 GPU 夹具证明。

UID 10200，ROMFS 8,973,693,207 字节，保留资源、Shader 文件、存档和设置。
数据与实际安装核验记录在同目录 `delivery-validation.json`；存档正常自动保存允许更新。

## 限制及后续

- **索引顶点获取的完整边界尚未实现。** 索引可能由 GPU 生成，不使用过期 CPU 数据推算范围。
  本批仅验证正常的索引/base vertex 绘制，不能推导 GPU 生成索引越界已解决。
- 现有 divisor > 1、非零 Component、复杂聚合输入和部分其他 NVN 顶点格式仍是显式边界。
  输入指针跨函数逃逸、混合存储类选择、PtrAccessChain 的适配显式拒绝，不默默生成错误 IR。
- 原生 scaled 分支未在本设备执行；没有 Mali/PowerVR 或其他设备验收。
- 未验证实际战斗和长期运行；GPU 主耗时、驱动恢复和广泛性能对照仍需继续。

复现 GPU 验证：

```powershell
python -X utf8 dm/android/tests/run_attachment_device.py --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache --build D:/AndroidSwitch/build/d2r-system-driver-main --output <new-directory> --vertex-checks --run-device --serial A6BFUT1C07005372
```

捕获 Shader 的 IR 校验使用 `dm/android/tests/run_vertex_spv_audit.py`，需要包含 `.spv.gz`
的本地 cache、单独输出目录和设备 serial，不加载游戏资源。
