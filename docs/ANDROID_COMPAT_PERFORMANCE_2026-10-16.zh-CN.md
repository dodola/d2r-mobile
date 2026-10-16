# Android 兼容性与性能优化：缓存与采样

基线 main `62faf1e`。保持 NVN recorder 的 `mtl_*` 接口和现有 Android/Apple 平台边界。
参考 ZeldaWWHDRecomp `f10b56621ed13e0053e7cf8190e740834377e45a` 的按能力降级、缓存校验和
驱动隔离思路，结合本项目已验证的 Qualcomm Shader 修复实现；没有移植其 GX2/Latte 转换器。
X1-BOX 的特性协商和先前驱动分析也不能替代本项目实际 Shader/资源验证。

## 实现

- Vulkan 1.1 loader/device 和 swapchain 前置检查；按 API/扩展支持查询 DriverProperties。
- 持久化管线缓存 v2 身份包含 vendor/device、driver ID/version、UUID、subgroup、quirks、
  启用特性和 Shader ABI。使用小端序序列化，不将 native struct 内存直接写入磁盘。
- 受限读取、文件类型/符号链接检查、长度和校验和检查、Vulkan header 校验、坏缓存重建、
  驱动拒绝种子后空缓存重试。缓存是可选加速，失败不省略 draw。
- 渲染锁保护 Vulkan cache；后台线程只处理不可变快照并原子替换文件。周期保存和后台
  生命周期保存减少 Android 强杀造成的缓存损失；不是所有进程终止都会调用 atexit。
- 常见 VkFormat 的属性按设备缓存，避免每次 present、绑定或拷贝重复查询驱动。
- 按设备限制各向异性和 LOD bias；普通格式缺少线性过滤时降级，整数纹理使用 nearest
  与整数 border color，深度比较采样保持独立策略。备用 sampler 按 descriptor slot 复用，
  重注册时在 GPU drain 后释放，避免每次 draw 新建对象。

## 本机验证

设备 `A6BFUT1C07005372` / TNA-AN00 / Android 14 API 34 / Adreno 642L。
系统驱动 ID 8，driver build `923a446bf8, I23d7d7df15, 1725536844`。
本记录不代表旧设备 Adreno 830/640，也不代表 Mali/PowerVR 验收。

原始记录位于本地 `D:/AndroidSwitch/analysis/compat-performance-20261016/`。

| 验证 | 结果与范围 |
| --- | --- |
| Android 原生构建 | CMake/NDK，`-Wall -Wextra -Werror` 通过 |
| 缓存/策略测试 | 主机 ASan+UBSan 和 Android 均通过；缓存 790 checks，包含逐字节身份失配、所有截断位置、损坏、超大文件和原子替换保留 |
| 特殊文件 | 主机真实 FIFO 拒绝测试通过；Android SELinux 不允许创建 FIFO，明确记录 `fifo_skipped=1`；符号链接拒绝两端均通过 |
| GPU 绘制与清屏 | 每轮 6 阶段 / 120 cases / 1,720,320 B，冷缓存、命中、损坏和重建后命中均无差异 |
| GPU 采样 | 每轮整数/浮点/深度比较 49,152 B 精确读回通过；确认 R32_UINT 实际命中备用 sampler |
| 翻译队列与生命周期 | 正常及 ASan/UBSan 模式通过；断言缓存保存只在 pause 的任务与 GPU drain 完成后请求，resume 不重复保存 |
| 既有主机回归 | pacing、dirty races/summary、触摸、线程生命周期与存档恢复通过 |

包含采样用例的同一 GPU 夹具共创建 13 条管线，结果如下。这里只记录 Vulkan 创建
调用的累计 CPU 墙钟耗时，不能换算成游戏 FPS 或持续 GPU 耗时。

| 进程 | 缓存读取 | 管线创建累计 ms |
| --- | --- | ---: |
| 冷缓存 | absent | 77.975 |
| 命中 | hit，22,195 B | 26.649 |
| 人为损坏 | rejected，重建 | 82.975 |
| 重建后命中 | hit，22,195 B | 23.837 |

详见 `gpu-samplers/result.json`、各进程 `*-device.log`；其模块通过 spirv-val，
本轮所有进程均为系统驱动 ID 8。独立夹具使用合成 Shader/几何，不宣称是游戏帧。

## 完整游戏

候选 APK 为设备现有 APK 的 native-only 更新，保留 Java、原游戏模块、驱动库与资源。
签名及 16 KiB 对齐检查通过。安装前后 UID 均为 10200，原 `game.romfs` 仍为
8,973,693,207 B；未卸载、清数据或更换驱动。已有 `KKK` 角色仍可见。

APK SHA-256：`7101cd9d04d72b9e687d44352bdd60b911e5466781cbe784e8cd9f7c4b0677a6`。
native SHA-256：`be6350365a6acf57cb466f805ec0894a9f918505f13bd73b3e69f30bafeb8086`。

真实原 nnMain 两次冷进程启动均到达角色选择 3D 界面，系统驱动日志记录 `quirks=0x3`。
截图 `game-cold.png`、`game-warm.png` 用于检查人物及场景，未见原先的明显人物变形；
这不等于全场景、全动画逐像素验证。首次磁盘缓存为空；进入菜单并退到安装器后，
缓存文件为 1,044,344 B。重启后明确记录 `status=1 seed_bytes=1044260`。

相同首帧均为 91 draws，首次创建阶段的 `pipeline_ms` 为 521.779 / 25.145。
这是单组冷/热观察，不将资源/系统页缓存影响排除在总启动耗时之外，也不据此声称
稳定游戏 FPS 提升。角色选择后期的 CPU 提交窗口约 15 FPS，尚未完成 SurfaceFlinger
实际显示间隔、热稳态及营地/战斗场景对照；稳定帧率优化仍在工作范围中。

## 复现

```powershell
python dm/android/tests/run_compat_checks.py --output D:/AndroidSwitch/analysis/compat-repeat/policy --run-device --serial DEVICE_SERIAL

python dm/android/tests/run_attachment_device.py --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache --build D:/AndroidSwitch/build/d2r-system-driver-main --output D:/AndroidSwitch/analysis/compat-repeat/gpu --run-device --sampler-checks --cache-roundtrip --serial DEVICE_SERIAL

python tools/android/build_native.py --build D:/AndroidSwitch/build/d2r-system-driver-main --output D:/AndroidSwitch/analysis/compat-repeat/native-build.json
```

NDK/生成资源样本/已有原始 Shader 夹具是本地前提，不随仓库提供。主机测试可使用
`run_compat_checks.py --host --sanitize --vulkan-include INCLUDE_DIR --output /tmp/compat`。
Vulkan snapshot 仍在渲染锁内执行，需要继续测量大缓存时的开销；Mali/PowerVR、
自定义驱动恢复、更广 Shader/格式覆盖和稳定帧率优化仍未完成，见持续工作清单。
