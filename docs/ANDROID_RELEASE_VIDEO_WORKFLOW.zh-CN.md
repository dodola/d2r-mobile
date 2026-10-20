# Android 1.1.0 说明视频：制作记录与续作指南

更新日期：2026-10-20（北京时间）。本文记录本机的实际工程和导出状态，供后续继续修改；本地绝对路径不是公共下载链接。

## 1. 从这里继续

| 内容 | 本机路径 |
| --- | --- |
| 视频工程根目录 | `D:/AndroidSwitch/output/videos/d2r-1.1.0` |
| 当前可编辑合成 | `D:/AndroidSwitch/output/videos/d2r-1.1.0/index.html` |
| 最新已有导出 | `D:/AndroidSwitch/output/videos/d2r-1.1.0/out/d2r-1.1.0-v3.mp4` |
| 首次向用户提供的导出 | `D:/AndroidSwitch/output/videos/d2r-1.1.0/out/d2r-1.1.0-v1.mp4` |
| 字幕 | `D:/AndroidSwitch/output/videos/d2r-1.1.0/out/subtitles.zh-CN.srt` |
| 本文的随工程副本 | `D:/AndroidSwitch/output/videos/d2r-1.1.0/README.md` |
| 离线 APK 与资源包 | `D:/AndroidSwitch/output/releases/D2R-Mobile-1.1.0-94012-20261020` |
| 仓库 | `D:/AndroidSwitch/runtime/d2r-mobile` |
| 使用的 skill | `C:/Users/dodol/.agents/skills/huashu-motion/SKILL.md` |

已有 v1、v2、v3 三次导出，均为 **60 秒、1920×1080、60 fps、H.264 + AAC**，大小约 17.27 MB。`.om/renders.jsonl` 中三者的输入 token 相同，表示同一合成输入的重复导出，不能把版本序号理解为三轮内容改版。

视频已导出，尚未收到用户对内容、节奏和音色的最终认可。后续先看 v3，再根据具体反馈修改。不要继续使用早期技术介绍方向。

## 2. 已确定的内容和风格

用户要求：

> “我们不是从技术的角度上，我们是从功能的角度上介绍，相当于整体更新了什么内容，相对上一版。”
>
> “重点是优化了性能和稳定性。”
>
> “用中文女生，动漫系主播的那种。”
>
> “重新截图有些分辨率低。” / “需要截取使用触摸操控的界面。”

执行方向：玩家视角的整体功能更新；暗金、炭黑、纸白配色；Cinzel 英文字标与 Noto Sans SC 中文；中文女性合成口播、字幕、配乐、音效。素材使用真实设备截图和触屏点按录像。

内容基线来自 10 月 18–20 日主线的 37 个 first-parent 提交，对照此前 91738 离线版；并非相对当日所有中间 94012 构建均为新增。详见仓库中的 [三天更新核对](ANDROID_UPDATE_REVIEW_2026-10-18_20.zh-CN.md) 与 [本版说明](ANDROID_RELEASE_1_1_0.zh-CN.md)。核对时的主线提交为 `4d95a686272ba9624f870938b3bd11abfc09cd14`。

措辞边界：

- 「术士君临」与 94012 配套离线版属于重点。
- 摇杆和离线安装之前已有，应说“完善／升级”，不说首次加入。
- 性能和稳定性可以介绍优化方向；不能用历史测试数字证明最终安装包普遍提速，也不能把视频 60 fps 当作游戏运行 60 fps。
- MOD 指适配、版本匹配且已安装的资源包；不能宣称任意 PC MOD 或 D2RMM 脚本直接加载。
- 一键全开只作用于**当前地图**。
- 手柄输入已有代码支持，本次无实体手柄硬件演示。
- 存档镜头展示真实菜单入口，不是后台恢复完整实测；不宣称锁屏问题已解决。
- 设备拍摄用的是 `1.1.0-94012-launcher` 可调试安装版；截图和点按记录不等于交付的不可调试 APK 已完成全部玩法、帧率、长时间稳定性验收。

## 3. 时间线与口播

| 章节 | 画面区间（秒） | 口播开始 | 重点 |
| --- | --- | --- | --- |
| hook | 0–8 | 0.25 | 安卓新版：游戏内容、操控、运行体验 |
| expansion | 8–16 | 8.25 | 术士君临、94012 离线版 |
| controls | 16–28 | 16.25 | 摇杆、技能、点按背包／技能树、HUD |
| quality | 28–36 | 28.25 | 性能稳定性优化、新增自动／均衡档 |
| saves | 36–42 | 36.05 | 后台存档与恢复流程 |
| launcher | 42–52 | 42.25 | 启动器、离线资源、MOD、当前地图全开 |
| end | 52–60 | 52.25 | 品牌收尾、更多设备与场景仍在验证 |

完整口播稿在工程 `script.txt`。实际 TTS 输入为 `tools/segments.json` 的 7 个段落，两者目前一致，改稿时必须同步。章节设计在 `treatment.md`；如叙事或时长变化，先更新 treatment，再改画面。

## 4. 工程结构与修改入口

以下路径相对视频工程根目录：

| 文件／目录 | 用途 |
| --- | --- |
| `index.html` | Studio 和渲染器实际读取的 HTML / CSS / GSAP 合成；本地库、字体，无需打包前端项目 |
| `tools/build_film.py` | 生成合成的维护源码：样式、场景、可编辑变量、时间线、配乐 ABC 均在这里 |
| `tools/segments.json` | 实际配音输入，格式为 `[段落 id, 文本]` 列表 |
| `tools/narrate.py` | 分段 Edge TTS、MP3、词边界 JSON、时长清单 |
| `tools/assemble_assets.py` | 拼接口播、生成词 cue／字幕、裁切启动器截图 |
| `tools/capture.py` | ADB 原生 PNG 截图；设备路径和序列号写在文件内 |
| `tools/capture_touch_sequence.py` | 本次固定屏幕状态下的连续拍摄脚本，不能盲目重跑 |
| `assets/captures/` | 本次原生截图，含可用素材和失败／命名不准确的中间截图 |
| `assets/narration-segments.json` | 每段实际使用的 MP3、文本与时长 |
| `assets/narration.wav` | 60 秒对齐后的口播，48 kHz、单声道 PCM |
| `assets/voice.words.json` | 123 条词／语义别名 cue，供 OM.cue 使用 |
| `assets/subtitles.json` | 24 条字幕的文本与时间 |
| `assets/sound.wav` | OM 生成的音乐／音效底轨，已按口播 cue 闪避；不是完整混音 |
| `assets/touch-native.mp4` | 原生触屏录像，2652×1200，约 15.95 秒，原始可变帧率 |
| `assets/touch-panels-fast.mp4` | 当前合成实际使用的面板录像片段，标注 1.6× |
| `assets/fonts/`、`vendor/` | 本地字体、GSAP、OM 运行时 |
| `.om/history/`、`.om/history.jsonl` | Studio 编辑历史 |
| `.om/renders.jsonl`、`.om/sound.json` | 导出和声音生成记录 |
| `out/` | MP4 和 SRT 输出，保留旧版本用于比较 |

**两套修改入口须保持一致：** Studio／`om set` 直接改 `index.html`；`python tools/build_film.py` 则按生成器重新覆盖整个 `index.html`。如果在 Studio 改过标题、颜色或时长，先把变化移植回生成器，或备份当前 HTML，再决定是否重建。只改了 HTML 就重跑生成器，会丢失这些修改。

不要重跑 `tools/prepare.py`：它属于旧技术方向，会覆盖功能稿。`directions/`、`tools/previous-technical-script.txt`、旧 `voice-*.mp3` 和 `assets/voice-preview.mp3` 是草稿／试听材料，不是当前完整配音。`build_film.py --touch-only` 会覆盖主合成为触屏验证稿，也不是最终导出命令。

## 5. 可直接继续使用的高清素材

拍摄设备为 HONOR 60 Pro / TNA-AN00，ADB 序列号 `A6BFUT1C07005372`。横屏原图 **2652×1200**，竖屏 **1200×2652**，没有用放大旧截图替代重拍。

| `assets/captures/` 下文件 | 内容 |
| --- | --- |
| `14-touch-controls-final.png` | 可用的触屏主界面原图 |
| `20-touch-hero-native.png` | 当前合成主界面：左摇杆、右圆形技能按钮、底部 HUD |
| `15-touch-panel.png` | 点按打开的背包 |
| `16-touch-skill-tree.png` | 术士技能树；合成也用于资料片章节 |
| `17-touch-options.png` | 保存并退出等选项菜单 |
| `03-auto-quality.png` | 实际已选中自动画质 |
| `18-balanced-native.png` | 实际已选中均衡画质 |
| `19-original-restored.png` | 恢复原画质后的系统菜单 |
| `26-class-select-native.png` | 本次新拍的术士角色选择 |
| `31-launcher-portrait-final.png` | 竖屏启动器“游戏已准备好” |
| `32-launcher-settings.png`、`33-launcher-mod.png` | 启动设置与 MOD 区域 |

不要仅凭文件名判断截图：`04-balanced-quality.png` 实际仍选中原画质；`05-performance-quality.png` 实际选中均衡。`09-inventory.png`、`10-skill-panel.png`、`13-touch-system.png` 也不是文件名所称内容，且有菜单式操控覆盖；不要替换进当前触屏重点镜头。`30-launcher-portrait.png` 截到了旋转过渡，应使用 31。

触屏模式要看左摇杆和右侧圆形技能按钮。此时顶部按钮文字是“菜单操控”，表示点击后切换到菜单操控；看到“触摸操控”按钮文字时，反而可能正处于另一种菜单／手柄式布局。

### 重新截图

在设备已进入目标界面后运行：

```powershell
Set-Location 'D:/AndroidSwitch/output/videos/d2r-1.1.0'
python tools/capture.py 34-new-touch

# 可选：先执行一个 ADB shell 动作，再截图；不要重复写 adb shell。
python tools/capture.py 35-new-panel input tap 1625 1140
```

脚本用 Python 读取 `adb exec-out screencap -p` 二进制输出。若换设备，先改脚本中的序列号；不要照抄旧坐标。设备点按、模式切换和拍摄按顺序执行，不能并发。

`capture_touch_sequence.py` 假定起点是特定选项面板，会修改画质、点击多处坐标并返回 HOME。只作为本次拍摄过程记录；续拍优先使用单次截图和手工确认。22、27 等所谓 launcher 截图实际上仍是游戏画面，不应采用。角色已执行保存并退出后才重启应用补拍启动器；临时竖屏锁定已恢复为 `wm user-rotation free`，画质恢复原画质。

### 重新生成当前录像片段

保留原始录像，导出片段时转换为固定帧率，避免将原文件的可变帧率元数据直接用作渲染参数：

```powershell
ffmpeg -y -v error -ss 2.4 -i assets/touch-native.mp4 -t 6.4 -an -vf 'setpts=PTS/1.6,fps=60' -c:v libx264 -crf 17 -preset fast -movflags +faststart assets/touch-panels-fast.mp4
```

合成在 20.8–24.8 秒使用该片段，24.65 秒开始淡出；画面注明“面板点按 1.6×”，之后返回完整触屏 HUD。它只证明拍摄中的点按操作，不是实时性能展示。

## 6. 配音与字幕的重建方法

当前声线为 Edge TTS `zh-CN-XiaoyiNeural`，`rate='+5%'`、`pitch='+28Hz'`，用于贴近用户要求的中文动漫系女主播风格；没有克隆某个人的声音。已生成音频可离线复用，新合成需联网。

依赖：Node.js、系统 Chrome、Python、FFmpeg／FFprobe、Python `edge-tts` 与 `Pillow`。本机已安装 `edge-tts 7.3.1`、`Pillow 12.0.0`。准备新环境时可用 `python -m pip install edge-tts Pillow`。

改稿流程：

1. 同步修改 `tools/segments.json` 与 `script.txt`。
2. 运行 `python tools/narrate.py`，检查每段时长。
3. 修改 `tools/assemble_assets.py` 的 `starts` 和 60 秒总长度，确保段落不重叠且不超出结尾。
4. 运行 `python tools/assemble_assets.py`，重建口播、cue、SRT 与启动器裁切图。
5. 同步修改 `tools/build_film.py` 的场景区间、媒体时间、结尾、字幕时间和配乐，再生成 HTML。

**配音缓存陷阱：** 当前 MP3 文件名的哈希只包含文本。仅修改声线、语速、音高，脚本仍会复用旧音频；需要把这些参数纳入缓存键，或移走对应 MP3 及同名 JSON 后再合成。不要把有 MP3 但缺词边界 JSON 的不完整缓存当作可用结果。

TTS 词边界的 `offset` / `duration` 单位是 1/10,000,000 秒。`assemble_assets.py` 将其转换成全片时间，并为“术士君临”“摇杆”“自动”“均衡”“MOD”“当前地图”等生成语义 cue。画面使用 `OM.cue()` 对齐口播，不能把换配音后的旧时间直接保留。字幕按分句对齐，不依赖自动 ASR 估算。

## 7. 预览、检查与导出命令

本机 CLI 位于 npm 缓存；缓存路径可能变化。找不到时使用 `npx huashu-motion`，并核对工具版本和 skill 说明。**只继续预览／导出时不用重建配音，也不用运行生成器。**

```powershell
Set-Location 'D:/AndroidSwitch/output/videos/d2r-1.1.0'
$motionCli = 'C:/Users/dodol/AppData/Local/npm-cache/_npx/44b1559a42bed6a3/node_modules/huashu-motion/cli/om.js'

# 预览当前合成；服务存活时访问其实际打印的地址。
node $motionCli studio . --no-open
```

本次工作台地址为 `http://localhost:4848/`。进程退出后需要重新启动；地址并不保证长期在线。用另一个终端执行检查和导出：

```powershell
Set-Location 'D:/AndroidSwitch/output/videos/d2r-1.1.0'
$motionCli = 'C:/Users/dodol/AppData/Local/npm-cache/_npx/44b1559a42bed6a3/node_modules/huashu-motion/cli/om.js'

# 仅在需要按生成器重建时执行，先确认没有待保留的 Studio/HTML 修改。
python tools/build_film.py

node $motionCli check .
node $motionCli still . --at 2.2,10.5,18.8,22,25.8,31.9,33.2,38,45,49,57
node $motionCli sound .
node $motionCli render .
node $motionCli qa .
node $motionCli learn harvest .
```

`render` 自动刷新声音，并在 `out/` 生成新的 `-vN.mp4`，不要猜版本号，以工具输出和 `.om/renders.jsonl` 为准。`qa .` 检查最新导出；Studio 再次导出后，QA 对象可能已不是先前向用户提供的 v1。

Studio 支持指向画面评论。收到评论后使用 `om inbox . --wait --timeout 30`、`om look <id> .` 定位，再最小修改并 `om done <id> . --note '修改说明'`。本次没有收到 Studio 评论。

## 8. 动画和声音的维护约定

所有画面由一个暂停的 GSAP timeline 驱动，注册为 `window.__timelines.film`；章节使用 `scene:*` 标签。标题、配色与结尾停留等由 `<html data-composition-variables>` 暴露，画面元素有稳定的 `data-om-id`。新增内容沿用这些约定，方便 Studio 编辑和定位。

预先设置未来场景／标签的隐藏状态，`fromTo` 使用 `immediateRender:false`；退出也显式指定起止状态。此前出现过直接跳时间点与顺序播放画面不同的 `history-dependent` 警告，已通过显式状态修正。只动画 transform／opacity，避免 width／left／top 布局动画。

配乐为生成器中的原创 ABC `om-score`：120 BPM、E minor，主题 E–G–B–A，30 小节；合成音色包括拨弦、铺底、贝斯和鼓。17 个 `OM.sfx` 落点随时间线维护，音乐在口播下闪避。改时长后需同时改场景与配乐小节，重跑 `sound`；不要手动编辑生成的 `assets/sound.wav`。

当前结尾停留变量并不能独立重排全片：字幕、媒体 `data-start` / `data-duration`、60 秒口播、进度线和配乐长度仍是固定值。真正延长影片时应整体同步这些时间。

## 9. 已完成检查与下一步

已完成：

- 触屏重点镜头先行验证，随后查看了开场、术士、背包／技能树、HUD、画质、存档、启动器和结尾的真实合成静帧。
- `om check`：**0 错误、9 警告**。此前的历史依赖警告已消失；剩余报告涉及父容器文字范围的低对比、片尾全屏图层及页眉重叠等，不能表述为“零警告”。
- `om sound` 成功；频谱在 `tools/sound-spectrum.png`。这张图检查的是底轨，不是完整口播混音。
- v1 / v2 / v3 的媒体格式、60 秒长度和 60 fps 已用 FFprobe 核对。
- `om qa` 实际检查 v3：未发现单帧闪烁，但报告多处 ≥1 秒静止和整体运动偏弱。当前是大量截图加旁白的解释片，节奏仍可改进，不能说已通过全部视觉质量检查。
- v1 完整音轨测得约 **−22.5 LUFS、真峰值 −6.9 dBFS**，音量偏保守。尚未完成响度提升版，也未验证用户对混音和音色的主观接受度。
- 已运行 `om learn harvest`。此前尝试写入偏好时因引用格式被拒绝，不能认为偏好已成功保存；用户原话已在本文保留。

推荐续作顺序：先让用户看／听现有版本，按明确反馈改；如继续打磨，优先增加真实操作录像的占比、缩短无信息变化的停留，再处理整体响度。检查片尾、字幕与 UI 不遮挡，并完整观看最终导出，不能只看静帧。

仅本机音量测量示例：

```powershell
ffmpeg -hide_banner -nostats -i out/d2r-1.1.0-v3.mp4 -vn -af 'ebur128=peak=true:framelog=verbose' -f null NUL
```

如果要调响度，先测量最新版本，分别评估口播／音乐比例和最终混音，再输出新文件；不要用底轨 LUFS 代替成片 LUFS，也不要覆盖用户正在看的旧文件。

## 10. 保存与交接范围

工程和媒体在仓库外的 `output/videos/`，本文在 Git 仓库内。只提交本文或克隆仓库**不会带走视频源码、配音、字体、截图和录屏**；换机器应复制整个工程目录，至少包含 `index.html`、`tools/`、`assets/`、`vendor/`、`script.txt`、`treatment.md`，建议连 `.om/` 和 `out/` 一起保存。

仓库不应纳入游戏资源包、APK、大量截图、音频和视频。离线交付文件独立放在上表 release 目录；说明视频不是 APK 的内容。仓库中另有与本任务无关的 `premium-audit.json`，不要一并提交或覆盖。

给后续助手的起始指令示例：

> 继续 D2R Mobile 1.1.0 说明视频。先阅读 docs/ANDROID_RELEASE_VIDEO_WORKFLOW.zh-CN.md，工程在 D:/AndroidSwitch/output/videos/d2r-1.1.0，已有 v3，三次导出输入相同。保持玩家功能视角、中文动漫系女声和原生触屏素材。先检查 Studio/HTML 是否有新修改，不要直接重跑生成器；根据我接下来的反馈修改，重跑 check、sound、render、qa，再提供实际新导出路径。
