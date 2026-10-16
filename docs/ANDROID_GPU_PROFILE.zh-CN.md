# Android Vulkan 设备兼容策略

`dm/gfx/nvn/nvn_vk_profile.h` 将设备探测和兼容规则分成两层：

1. `NvnVkGpuProfile` 是创建 Vulkan 设备时采集的一次性能力快照，包含 API、GPU
   vendor/device、driver ID/version 以及 subgroup 能力。
2. `NvnVkQuirkRegistry` 由快照生成，保存 shader 兼容变换和渲染档位。录制/回放
   路径只查询 registry，不再直接比较厂商 ID。

当前规则保持原有行为：Qualcomm proprietary driver 始终启用 skin UBO 变换；只有
同时支持 compute subgroup arithmetic 时才启用 ballot 变换。其它驱动保持 full 档，
因此引入 profile 不会改变 Turnip 或未知驱动的启动路径。

启动时还会生成包含设备、驱动、subgroup、quirk 和 Vulkan cache UUID 的固定长度
`NvnVkPipelineCacheKey`，并打印其版本、UUID 前缀和 quirk 位。它目前用于定义持久化
缓存的身份边界；磁盘读写仍需单独接入，不能把进程内 `VkPipelineCache` 当作跨启动缓存。

以后增加设备规则时，应在 `nvn_vk_quirk_registry_resolve` 中增加一个小规则，并把
它映射到独立的 `NvnVkQuirk` 位。不要在 shader 加载函数中增加新的 vendor/version
判断。这样 profile 可以继续用于 pipeline cache key、启动自检和动态 RenderTier，
而不会把设备特判散落到渲染器各处。
