# Android 图像用途与首次存储访问升级

此前每个支持 storage 格式的图像均声明 `VK_IMAGE_USAGE_STORAGE_BIT`，即使只被采样或
用作颜色附件。系统 Adreno 驱动上，减少这种额外用途的候选出现了明确 FPS 收益。
用途标记可能影响驱动的图像布局/压缩选择，但本轮没有硬件压缩计数器证据，不能把机制当成已证明。

## 实现与边界

默认按 guest `NVN_TEXTURE_FLAGS_IMAGE` 提示保留 STORAGE；其它图像初始只声明实际基础用途。
这不是假定没有 IMAGE 标记就永远不会写入：首次实际 storage 绑定之前，会创建支持 STORAGE
的新图像，GPU 复制所有 mip/数组层/深度切片，再切换到 GENERAL 布局。
采样和存储两种 shader stage 获取描述符之前就完成升级准备，纹理 incarnation 和登记版本
同时改变，使此前的绑定计划失效。没有跳过计算、绘制、同步或读回。

旧视图可能仍被已录制命令、Framebuffer、描述符引用，因此旧分配随源纹理保留，直到
FinalizeTexture 按原有顺序 drain 两槽、清除附件缓存后释放。升级单向且最多一次，
额外保留上限是一份该纹理的旧图像；在本机实测营地中升级次数为 0，未增加这一项保留内存。
经常缺失 IMAGE 提示的其它游戏可能有额外内存成本，本轮没有证明这类负载的内存表现。

`DM_VK_IMAGE_USAGE_FLAGS=0` 恢复保守分配。Android 的 `conservative-image-usage` 文件
仅供启动前对照；普通运行没有这个标记。`declared-image-usage` 用于显式开启诊断。
旧的 diag-img-usage 路径也保留合法 storage 支持，并在绑定前检查实际 Vulkan 用途。

## GPU 检查

```powershell
python -X utf8 dm/android/tests/run_attachment_device.py --fixture-tools D:/AndroidSwitch/tools/d2r-mobile-port --cache D:/AndroidSwitch/analysis/d2r-mobile-port/shader-replay-cache --build D:/AndroidSwitch/build/d2r-system-driver-main --output <新的证据目录> --image-usage-checks --run-device --serial A6BFUT1C07005372
```

四种配置交叉比较保守/新用途与命令缓存开/关。每组使用四次纹理创建（IMAGE 提示有/无），
3 个 mip、3 个数组层；同一次真实提交先清除所有子资源，再对其中一层/mip 的一个像素
执行 imageLoad/imageStore 反转，之后再次修改附件并计算。逐像素独立预期检查全部子资源，
包括未写入像素、其它 mip/层，以及二次计算的结果。检查升级次数恰为一次、提示 IMAGE 的
图像不升级、销毁后旧图像保留计数恢复。

每种配置 8 次 Compute、516,096 字节新增读回；四组共 32 次、2,064,384 字节，结果完全一致。
同时四组均通过既有 120 个附件案例和基础 NVN 检查。初次证据为
`D:/AndroidSwitch/analysis/compat-performance-20261016/cpu-sampling/image-usage-gpu-1/`。

默认开启版本重新通过同一专项，证据为 `cpu-sampling/image-usage-gpu-default/`。
此外重新通过 `image-default-depth` 的深度复制四模式，以及 `image-default-vertex` 的顶点格式回归。
该专项不能替代其它维度纹理、战斗、长期和跨设备验收。

## 实际游戏对照

TNA-AN00 / Adreno 642L / Android 14，Qualcomm 系统 Vulkan driver id 8。
保留原始 Shader 和渲染比例；相同 KKK 角色、区域 1、固定营地坐标，依次重启切换用途策略。
每轮约 60 秒实际 SurfaceFlinger 呈现，采样窗口没有新管线创建；电池温度 40–41°C，
thermal status 2，尚未证明热稳定。场景由只读探针周期核对，不是连续证明。

| 顺序 | 用途策略 | 实际呈现 FPS |
|---|---|---:|
| 0 | 新用途 | 22.712757 |
| 1 | 保守 | 19.307515 |
| 2 | 新用途 | 22.492837 |
| 3 | 保守 | 19.153939 |

两组中位数 **19.230727 → 22.602797 FPS，+17.53%**。
记录与完整坐标保存在 `cpu-sampling/image-ab-summary.json`，原始记录为 `image-ab-0..3-*`。
本机该营地统计 `storage_images=35 omitted_storage=3406 promotions=0 retained_old_images=0`，
没有触发延迟升级，也没有额外保留旧图像。

普通默认启动（无用途诊断标记）的首轮 59.617 秒为 **20.833 FPS**；同进程延长运行后
第二轮 59.842 秒为 **22.492497 FPS**，23 个日志窗口无新管线创建。保留较低首轮，
不能把短时波动解释为已证明的固定时钟或热稳态。证据为 `image-default-camp` 与
`image-default-camp-2`。

`image-delivery-validation.json` 通过源码/构建身份、实际映射库 SHA、系统驱动、默认策略、
UID 10200、ROMFS 8,973,693,207 字节、15 个存档/设置文件和三份原始 Shader 的核对。
存档仅正常运行产生的 `KKK.d2s.meta` 改变；未恢复或覆盖存档。

- APK：`cpu-sampling/D2R-Image-Default.apk`
- APK SHA-256：`a41b4076e962bfd88dd72867075091834dfd61146a77ebef9bd583e67c3c0234`
- 实际 native SHA-256：`52e0ec8d19c2555320cae41eed8eccaaf48efd3b3e7f0b9fb7157026fc12d958`

本地提交 main，不 push；保留 worktrees。
本批尚未达到 30 FPS，性能目标继续；上述营地结果不代表战斗或其它设备。
