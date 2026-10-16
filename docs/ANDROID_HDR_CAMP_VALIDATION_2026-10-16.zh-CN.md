# HDR Shader 调查、日志减量与系统驱动营地验证

本批继续推进兼容性与性能总目标，未完成跨设备、战斗、深度复制替代路径和顶点格式审计。
运行设备仍为 `A6BFUT1C07005372`：TNA-AN00、Android 14、Adreno 642L，系统驱动 ID 8。
所有设备证据保存在仓库外的
`D:/AndroidSwitch/analysis/compat-performance-20261016/hdr-shader-current/`。

## 重型 Pass 与实际 Shader 的关联

上一批计时的最重组是颜色 serial 23、深度 serial 29 的 1280×720 HDR 目标，
测量范围含 Pass 前后的同步，不能视为纯 Shader 执行时间。
本批对设备正在使用的 Shader 文件直接取样并记录 SHA-256，避免分析历史替换版本。
一次显式帧追踪记录 969 次绘制：

| 目标颜色 serial | 片元 Shader | 此帧绘制次数 |
| --- | --- | ---: |
| 23 | `38f344adf801b153` | 154 |
| 23 | `5c1ca4e7c3e4657c` | 140 |
| 35 | `b3d4dd57cd4d630c` | 89 |

因此前两者属于该 HDR 颜色目标；第三个高频 Shader 属于另一个目标，不能混入重型组。
证据：`manifest.json`、`trace-tail.log`、`draw-association.json`、`instructions.json`。
绘制频次仍不等于 Shader GPU 耗时。

## 离线优化候选没有测出收益

`38f3…` 原始 SPIR-V 为 276,820 字节，17,312 条指令，包括大量函数局部变量和载入/存储。
NDK 的 `spirv-opt -O` 将它缩为 107,936 字节，`spirv-val --target-env vulkan1.1` 通过。
`5c1c…` 的当前版本已是优化结果，再运行相同优化没有改变模块大小。

只替换 `38f3…` 的 `.spv.gz` 和同步更新的 `.vkmeta`，每次停止游戏后部署并校验文件，
启动预热至少 600 帧，要求最近一个窗口无新管线编译，再采集 45 秒实际呈现。
四轮均为同一个角色菜单，电池 41°C、系统热状态 2；未控制 GPU 频率。

| 顺序 | Shader | 实际呈现 FPS |
| --- | --- | ---: |
| 0 | 原版 | 15.026 |
| 1 | 优化版 | 14.984 |
| 2 | 原版 | 14.941 |
| 3 | 优化版 | 14.893 |

两组均值约为 14.984 与 14.939 FPS，差约 -0.30%，不能证明性能改善或显著退化。
这也不能证明驱动内部产生了相同的机器码。模块缩小并不代表 GPU 工作减少。
候选仅完成模块合法性及探索性完整游戏运行，没有原始 Shader 输入的逐像素等价证明。
因此没有更改默认资源缓存，结束时已按设备原始文件逐字节恢复并复核三对取样 Shader。
此阶段 14 个存档及设置文件内容哈希全部未变。

证据：`optimizer-survey.json`、`ab-summary.json`、`ab-0-original/` 至
`ab-3-optimized/`。`run_ab.py` 是本次有停止/启动和恢复操作的设备脚本，
重用前必须检查现存输出、备份及当前设备状态。

## 减少普通运行的逐次复制日志

普通菜单每帧执行三次纹理复制，旧实现每次都写 `COPY_T2T` 详情。
现在仅在前两个呈现帧和显式帧追踪期间写详情，正常运行改为随已有窗口输出
`RENDER_TEXTURE_COPY_PROFILE`，保留调用数、缩放调用数和目标 texel 数。
这些是逻辑复制量，不是实测显存带宽或 GPU 时间。

更新原生库后，45 秒菜单采集没有逐次复制详情；各完整 60 帧窗口均为 180 次复制，
63,160,320 个目标 texel，即每帧三次、1,052,672 texel。
采集区间日志从旧版原始 Shader 两轮的平均 305,008.5 字节降至 62,034 字节，约少 79.66%。
实际呈现为 14.995 FPS，没有据此宣称 FPS 提升。
启动详情仍存在；随后一次营地显式追踪保留两次复制详情和 1,264 次绘制详情，
请求标记已消费，验证追踪功能没有因日志门控丢失。

原生构建通过；APK 保留原安装壳的 Java、资源和模块，只更新原生库，16 KiB 对齐和签名检查通过。
实际安装并映射的库 SHA-256：
`2af3368e4e629d85afb39bc5d681c8267024c799006343605221907e209ee211`。
证据：`log-gated-native-final.json`、`D2R-Render-Logging-final.build.json`、
`log-gated-menu/`、`log-gating-summary.json`、`trace-gating-check.json`。

## 营地场景与移动验证

`capture_present_profile.py` 新增可选 `--scene-probe`、`--level`、`--stationary`。
使用只读的 `scene_91738_probe.c`，核对已知原游戏布局签名、PID、玩家 ID/指针及区域。
开始、每次状态采样和结束均检查；静止模式还检查坐标不变。
输出记录 probe 二进制哈希、全部场景采样和身份验证是否通过。
这属于离散采样，不能证明采样间没有短暂切换。仅填写 `--scene` 仍不会自动声称验证身份。

负向检查在角色菜单指定区域 1，probe 返回空玩家列表，采集正确拒绝，
`completed=false`、`scene_identity_verified=false`。
随后通过普通游戏确认进入罗格营地，预热后采集 60 秒：

- 实际呈现 988 帧，跨度 59.777 秒，16.511 FPS。
- 帧间隔中位数 58.114 ms、p95 66.501 ms、p99 66.724 ms、最大 174.231 ms。
- 13 次场景采样均为区域 1，同一个玩家、相同坐标。
- 电池 41°C、系统热状态 2；没有证明频率或温控稳定，不作跨场景提速对比。
- 静止截图及随后短距离摇杆移动截图未见此前明显的人物拉伸；坐标变化且玩家、区域不变。
- 这只覆盖此营地和这次短移动，没有完成战斗、所有动作或长期稳定性验收。

入口示例（需先编译并安装只读 probe，输出目录必须尚不存在）：

```powershell
python dm/android/tests/capture_present_profile.py --serial A6BFUT1C07005372 --scene rogue-encampment --scene-probe files/attachment-scene-probe --level 1 --stationary --seconds 60 --output D:/AndroidSwitch/analysis/camp-new-run
```

安装使用覆盖更新，UID 仍为 10200，ROMFS 仍为 8,973,693,207 字节；没有卸载或清数据。
进入游戏和移动后，角色存档、元数据和地图文件由游戏更新，新增共享仓库文件；
原有文件无缺失，设置内容未变。已保留进入游戏前和验证后的备份，没有覆盖回旧存档。
显式诊断标记均已恢复为不存在，最终仍在系统驱动营地中运行。
证据：`system-camp/`、`camp-stick-movement.json`、本地截图及 `delivery-validation.json`。

下一批继续处理深度/模板复制和顶点格式的能力边界，并针对真实 GPU 工作选择优化，
不把此次日志减量、候选否定结果或单个营地验证当作总目标完成。
