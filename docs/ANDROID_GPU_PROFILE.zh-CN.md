# Android Vulkan 设备兼容策略

`dm/gfx/nvn/nvn_vk_profile.h` 将设备探测和兼容规则分成两层：

1. `NvnVkGpuProfile` 是创建 Vulkan 设备时采集的一次性能力快照，包含 API、GPU
   vendor/device、driver ID/version 以及 subgroup 能力。
2. `NvnVkQuirkRegistry` 由快照生成，保存 shader 兼容变换和渲染档位。录制/回放
   路径只查询 registry，不再直接比较厂商 ID。

当前规则保持原有行为：Qualcomm proprietary driver 始终启用 skin UBO 变换；只有
同时支持 compute subgroup arithmetic 时才启用 ballot 变换。其它驱动保持 full 档，
因此引入 profile 不会改变 Turnip 或未知驱动的启动路径。

启动时生成包含设备、驱动、subgroup、quirk、启用特性、Shader ABI 和 Vulkan cache UUID
的 `NvnVkPipelineCacheKey`。v2 身份使用明确的小端序编码，缓存写入 shader 目录同级的
`pipeline-cache/<identity-hash>.bin`，切换驱动/策略后不会沿用不兼容缓存。

加载同时检查文件大小（最多 64 MiB）、身份、长度、校验和及 Vulkan v1 header。
坏文件被忽略，驱动拒绝种子时重试空缓存；缓存创建仍失败则不使用缓存继续运行。
不会跳过绘制或修改游戏资源来掩盖失败。缓存不包含 Shader 预建组合清单。

Vulkan 数据快照在渲染锁下取得，后台线程负责临时文件、fsync 和原子替换；首次有
管线后等待初始化阶段的两秒门限，再最多每 30 秒检查一次修改。退到后台完成 GPU
drain 后也请求异步保存，正常进程退出会等待保存；Android 强杀前最后一段缓存可能丢失。
`DM_DISABLE_PIPELINE_CACHE` 关闭全部管线缓存；`DM_DISABLE_PIPELINE_CACHE_DISK` 只关闭
持久化；`DM_VK_PIPELINE_CACHE_DIR` 可指定已有父目录下的缓存目录。目录不可写时仍可渲染。

能力查询还会检查 Vulkan 1.1 门限和 swapchain 扩展，DriverProperties 仅在对应核心
版本或扩展可用时加入查询链。常见格式的属性按设备缓存；采样器的各向异性、LOD bias
受设备 feature/limit 限制，不支持线性过滤的普通格式降级为 nearest。整数纹理使用
nearest 和整数 border color，深度比较采样单独处理，不能套用普通线性过滤判定。

验证范围和原始结果入口见 [兼容与性能优化记录](ANDROID_COMPAT_PERFORMANCE_2026-10-16.zh-CN.md)。

以后增加设备规则时，应在 `nvn_vk_quirk_registry_resolve` 中增加一个小规则，并把
它映射到独立的 `NvnVkQuirk` 位。不要在 shader 加载函数中增加新的 vendor/version
判断。这样 profile 可以继续用于 pipeline cache key、启动自检和动态 RenderTier，
而不会把设备特判散落到渲染器各处。
