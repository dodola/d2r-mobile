# 上游同步与 Android 适配

同步 `https://github.com/jhaizhou-ops/d2r-mobile.git` 的 `main`，目标 `6edec84`。
从既有共同基底引入 8 个提交，保留已发布的 main 历史，通过 merge 同步；此前本地性能模式阶段提交已 rebase 检查 `origin/main`，无需移动。

解决 `hle_nvn.c`、`nvn_obj.h`、`hle.c`、`loader.c` 四处冲突：

- 统一上游 `resolve_import(module, name, weak)` 接口，未解析弱符号返回零，保留 Android HLE/force ELF section 路径及强导入诊断行为。
- 保留 Android shader scratch、sync、event 命令，将新增 `OP_COPY_IMAGE_STRIDE` 追加到旧 opcode 后，避免更改既有编号。
- Vulkan 消费新 image-stride 命令；buffer-to-texture 使用上游共用 pitch 校验，BC 解码按实际 image stride 访问层；未压缩图像在显式 image stride 时逐层/体切片复制，避免将任意字节间距错误取整为 Vulkan imageHeight。
- 适配上游触控缩放的新含义：Android 视口以 dp 传入，短边至少 600dp 才启用平板缩放。native→Java 的 58-float 接口保持一致。

离线验证：Android native 编译通过；上游 `t_copy_pitch.c` 的原始断言在 NDK 编译的 AArch64 ELF 中通过 Unicorn 执行（puts 与断言失败出口由离线 shim 提供）；上游 `test_touch_layout_options.py` 通过；合并冲突标记与 diff whitespace 检查通过。

证据：`D:/AndroidSwitch/analysis/compat-performance-20261017/upstream-sync/`。
用户要求先离线执行，因此没有安装或操作设备。新增层间距路径的 Vulkan GPU 读回、游戏输入/触控和冷启动回归仍待真机验证；未运行 Mac/iOS 平台测试。
