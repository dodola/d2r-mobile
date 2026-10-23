# Android 启动器视觉更新（2026-10-23）

English: [ANDROID_LAUNCHER_STYLE_2026-10-23.md](ANDROID_LAUNCHER_STYLE_2026-10-23.md)

基于已完成的[启动设置与存档管理](ANDROID_LAUNCHER_SAVES_2026-10-23.zh-CN.md)，本轮统一启动器视觉，版本更新为 `1.2.1-94012` / `versionCode=7`。

## 视觉与实现

- 原创静态背景：遗迹拱门、石墙轮廓、冷暖雾光、微尘和边缘暗角。`LauncherStyle.Backdrop` 使用 Android Canvas，按视口尺寸缓存渐变，不依赖图片下载、游戏素材或持续动画。
- 卡片：半透明深色渐变、细金属边框、分区图标和标题分隔线。标题使用系统衬线字体，正文和数值保持清晰的无衬线字体。
- 设置：每行增加功能图标、独立选择区域和右侧箭头；支持按压涟漪及焦点描边。禁用时降低文字和图标亮度。
- 操作按钮：启动使用金铜渐变和播放图标；导入、导出、备份与帮助使用描边按钮和对应图标。启动入口继续固定在底部。
- 开关与弹窗：FPS 开关和选择弹窗统一金铜配色；弹窗使用圆角深色面板并压暗背景。

`LauncherStyle.java` 集中背景、图标和控件状态。`launcher_theme.xml` 仅由 `InstallerActivity` 采用；`launcher_dialog.xml` 提供弹窗面板。所有可见文字继续引用现有统一语言表。

## 验证和范围

独立模拟器包使用合成文件完成检查：中文默认字号和英文 120% 字号两轮各 39 项通过；主机存档/配置检查 100 项通过。已检查横屏双列、竖屏滚动后的存档区域、固定启动按钮及设置弹窗，控件可点击、文字可换行、无按钮被底部导航栏遮挡。APK 打包单元测试 17 项、i18n 475 条和译文生成检查通过。

存档协议、游戏启动流程和 native 渲染代码未更改；native SHA-256 仍为 `47d7f4bf68faaf7227096b74de7c22a369d7c421c37c9c97bad42c45161ff162`。本轮没有实机安装或游戏内验收，未操作 TNA；PPG 仍待连接。

本机截图和日志位于 `D:/AndroidSwitch/analysis/launcher-style-20261023/qa-zh/` 与 `qa-en-font120/`，中文预览拼图为 `qa-zh/review.png`。QA 包版本标识用于独立测试，不是正式 APK 的版本。

本机预览包：`D:/AndroidSwitch/output/releases/D2R-Mobile-1.2.1-94012-launcher-style-20261023/D2R-Mobile.apk`。沿用原发布签名、非 debuggable，可覆盖升级并复用已有资源包。APK SHA-256：`56bd05871333fbf8e43d71e366578f683aa3d3e87815445ef9fb7f183abfc3cb`。

本次源码仅包含原创绘制代码和 Android 主题资源，不包含游戏美术、真实存档或构建产物。
