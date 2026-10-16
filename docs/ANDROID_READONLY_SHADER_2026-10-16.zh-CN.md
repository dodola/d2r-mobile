# 只读 Shader 标记实验

对两份 HDR 片元 Shader 的两个只读 Storage Buffer 进行了离线接口审计，确认访问链只有
整数索引后的 load，没有 store、atomic 或别名写入；加入 `NonWritable` 后通过
Vulkan 1.1 SPIR-V 校验。设备上使用系统 Vulkan driver id 8、原分辨率和固定营地做了
30 秒方向性采样，实际呈现 **22.143 FPS**，当时 thermal status 为 3；此前原始 Shader
在不同热状态下约 20.7–22.5 FPS。没有形成可归因的稳定收益，因此不修改发布资源。

实验 Shader 已恢复为 `hdr-shader-current` 中的原始 `.spv.gz` 与 `.vkmeta`，并逐文件比对
通过。相关实验文件保留在性能证据目录，不进入 APK 和默认运行路径。
