# android-integration 代码移植第一版（2026-10-20）

本版从本地 `main@62c6db8` 建立 `codex/android-integration-optimizations`，参考
[vickwv/d2r-mobile 的 android-integration](https://github.com/vickwv/d2r-mobile/tree/3da783c04ff6eca0870f850ffd290682f62f0088)。
这是无设备条件下的代码移植版：未安装 APK，未执行 GPU，未证明画面、帧率、功耗或实机稳定性。

## 移入内容

| 来源提交 | 本地实现 | 默认行为 |
| --- | --- | --- |
| `e7eafcc` | 仅移入 `dm_cursor.c` 的确认框判定顺序修复。模态框先取得输入，底下被设置页盖住的暂停菜单不能拦截 Apply/Discard。输入锁和替按键部分保留本地实现。 | 启用 |
| `7961a71` | `nvn_vk.c` 自带 Android 平台守卫；CMake 定义 `DM_EXPECT_VULKAN_BACKEND`，工具链缺少平台宏时直接报错。未移入本地没有的录制计时改动。 | 启用 |
| `0f107b3` | 将所有 mip/layer 合入一个暂存缓冲与一次复制批次，复用一次解排布工作区。保留本地 D24S8/D32FS8 深度、模板分面上传。每个区域按 texel/block 字节数与四字节的最小公倍数对齐，检查整数溢出。 | 启用，可回退逐 subresource 提交 |
| `68852db` | BC1–BC7 格式映射和原生压缩上传；按功能位、具体格式能力与 image 格式查询决定能否使用。增量刷新保留 CPU 快照和 GPU 所有权语义；B2T 重排有填充的行/层；T2T 校验压缩块边界；设备像素探针仍返回解码像素。 | 实验，关闭 |
| `d9643cd` | 本地已经缩小清零范围的数组不重复改；只缩小剩余 `plan_resources` 的清零范围。 | 启用 |
| `e735628` | Android 专用让出退避：每线程 16 ms 内前 16 次正常 yield，之后插入 100 us sleep。配置用 `pthread_once` 读取，避免共享普通变量的初始化竞争；等待耗时包括 sleep。 | 实验，关闭 |

上传与增量刷新分别放在 `nvn_vk_texture_upload.inc`、`nvn_vk_texture_refresh.inc`，由真实后端和离线测试共享，避免另写一套测试实现。

对方增量刷新把压缩块列数、行数直接用作 `bufferRowLength` / `bufferImageHeight`。
本版改为对应的 texel 数；这两个字段即使在压缩格式下也以 texel 为单位，见
[Vulkan VkBufferImageCopy](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferImageCopy.html)。

## 实验开关

标记文件位于当前应用私有数据根下的 `files/d2r-mobile/diagnostics/`，重启应用后生效。
由 `nativeConfigure` 在创建游戏线程与 Vulkan 设备之前读取；不依赖外部 shell 进程的环境变量。

| 标记文件 | 效果 | 底层变量 |
| --- | --- | --- |
| `disable-texture-upload-batch` | 改回每个 mip/layer 单独分配和提交，使用相同的编码与对齐检查 | `DM_TEXTURE_UPLOAD_BATCH=0` |
| `enable-bc-passthrough` | 请求原生 BC；设备或格式不支持时保持 CPU 解码 | `DM_BC_PASSTHROUGH=1` |
| `enable-yield-backoff` | 开启 Android 让出退避 | `DM_YIELD_BACKOFF=1` |

原生 BC 当前只选择支持的 2D、数组、cube 采样/传输图像；IMAGE 存储图像及其他维度保留解码。
原生 BC 的 T2T 限于相同格式、相同大小、合法块边界的区域；混合压缩/解码表示或缩放复制会明确报错，
不能据此宣称完整覆盖所有游戏路径。遇到该边界应关闭 BC 实验开关，后续再实现表示转换。
客体请求原压缩格式的 CPU ReadTexels 仍沿用原有具名拒绝；调试像素探针可以读回压缩块再解码。
原生 BC 纹理不进入原本只理解解码像素的可选 shader 纹理转储。

未移入强制丢弃附件内容、按分辨率跳过遮挡查询、25 ms 节拍、分辨率预算修改、脏页查找缓存、
对方 Activity/打包链或固定二进制预热清单。保留本地驱动、离线安装、版本身份验证、存档及管线缓存体系。

## 已执行的验证

- Android NDK 27.2.12479018 / arm64-v8a / API 29 / Release：基础配置和完整 94012 配置均编译、链接成功，沿用严格告警设置。
- 离线上传测试：普通版和 AddressSanitizer/UndefinedBehaviorSanitizer 版各 **72,135** 项检查，零失败。
  测试执行生产上传/刷新代码、真实 Tegra swizzle/deswizzle 和 BC 解码；CPU recorder 检查实际暂存字节及复制区域。
  覆盖 R8/RG8 小 mip、3D 纹理、多层数组、252 层 × 7 mip 图集、两种深度/模板格式、BC 解码与直传、
  压缩纹理局部刷新、同页 GPU 改写块保留、溢出拒绝及退避时间窗口。
- 非 Android 平台守卫：GCC 严格预处理/语法检查通过；错误指定 Android 目标却缺少平台宏时按预期拒绝。
  这不等于执行过 macOS/iOS 工具链。
- 现有 `run_merge_checks.py` 回归通过：节拍、present、真实缺页/脏页并发、触控、200 个 pthread 生命周期、73 项存档恢复等。
- frontend-design-premium 静态审计：零错误、零告警；未执行设备上的确认框点击、视觉与辅助功能验收。

离线测试命令（Linux/WSL + GCC；Vulkan 头也可来自 NDK）：

```sh
python3 dm/android/tests/run_integration_port_checks.py \
  --output /tmp/d2r-integration-port \
  --vulkan-include /path/to/ndk/sysroot/usr/include
python3 dm/android/tests/run_merge_checks.py --output /tmp/d2r-merge-regression
```

本机收据位于 `D:/AndroidSwitch/analysis/android-integration-port-20261020/`。
`host/result.json` 明确记录 `device_contacted=false`、`gpu_executed=false`，保留运行命令和生产源码哈希；
`regression/result.json` 保存现有回归结果。没有生成或分发游戏内容。

## 有设备后的验收

1. 默认配置确认建角、进入/退出游戏、设置确认框 Apply/Discard、存档与前后台往返。
2. 批处理开关交错对比首次遇怪、进入新区、图集载入；核对画面与实际停顿，检查内存峰值。
3. 分别开启 BC、退避，再组合开启；记录设备、驱动身份、分辨率、温度和场景。BC 需要覆盖数组/cube/mip、
   局部 CPU 刷新、B2T/T2T 与可能出现的缩放/存储边界，确认无 Vulkan 错误、STOP 或画面变化。
4. 完成上述测试后再讨论实验选项是否改为默认；本版没有引用对方设备的 FPS 作为本地收益。
