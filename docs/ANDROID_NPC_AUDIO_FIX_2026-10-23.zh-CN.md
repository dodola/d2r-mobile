# Android NPC 对白无声排查与修复（2026-10-23）

状态：代码、回归与离线 APK 已完成；PPG 游戏内 NPC 语音已获用户听感确认，验收通过。

## 发现

1. 游戏请求 48 kHz、PCM16、6 声道。既有 PPG / TNA 启动日志均记录 `channels=6`。例如 `D:/AndroidSwitch/analysis/d2r-mobile-port/PPG_AN00/turnip-v2-startup.log:163`。
2. `dm/android/audio_aaudio.inc` 原样把 6 声道交给 AAudio。既有 TNA 系统日志记录 `channelMask = 0x8000003f`（索引布局，没有 FL / FR / C 等声道位置），见 `D:/AndroidSwitch/analysis/d2r-mobile-port/TNA_AN00/skin-ubo-runtime-verify/fixed-logcat.txt:8772`。这种布局不能保证系统将中置路正确混到手机双声道输出。
3. Apple 路径此前已因同类问题加入 5.1 下混，并记载游戏顺序为 FL / FR / C / LFE / SL / SR，NPC 对白位于中置。该函数原先位于 Android 条件编译分支之外，Android 未使用。
4. 复查上一轮尚未提交的菜单按键补丁，发现导航圆钮手指被标为 `DM_TC_NONE`。Android 适配器会把这个类别的手指再转发为原始 guest touchscreen，造成 Npad / 原始触屏同时输入的漏洞，可能干扰讲话。已修正触点归属。

上述是代码和日志确认的问题。随后在 PPG 上进入卡夏交谈并播放介绍，用户回复“已经修复了”，确认本次 NPC 语音故障已解决；不据此扩大为全部输入和生命周期场景均已验收。

## 改动

- 提取既有 Apple 定点下混函数到 `dm/host/audio_mix.h`，两端共用。中置与同侧环绕合入左右声道，预留 3 dB 余量并饱和限幅，按既有算法不合入 LFE。
- Android 6 声道输入在 AAudio 回调里按最多 256 帧下混，实际声卡打开双声道。暂存区属于 AudioOut 对象，回调不分配内存。
- guest 声道查询仍返回 6；队列消耗和缓冲释放按原始 6 声道字节计，硬件时钟按帧计。单、双声道直通。
- 打开 / 重开流的日志明确记录 `guest_channels=6 device_channels=2 downmix=5.1-to-stereo`。
- 导航圆钮保留目标类别，仅抑制持续 held key；不再把它转发为原始触屏。
- 测试包版本为 `1.2.3-94012`，versionCode `9`。

## 已完成验证

- Android native 构建通过。
- WSL Clang AddressSanitizer / UndefinedBehaviorSanitizer：实际 `audio_render` 回调验证中置正负信号、左右定位、LFE、饱和、跨 256 帧边界、跨 guest 缓冲、欠载补零及单 / 双声道逐字节直通；10 万块队列压力测试通过，22,846,824 字节全部按序收回，末次通知到达。
- TNA `A6BFUT1C07005372` 独立真实 AAudio 探针通过。6 声道只给中置三角波：实际双声道、33 次回调、172,800 guest 字节、28,320 个非零输出采样；单 / 双声道探针也通过。各探针覆盖缓冲释放、停止 / 恢复和关闭。该测试证明 PCM 进入实际 AAudio 流，不等同于 NPC 讲话听感验收。
- TNA 上软件队列探针同样通过 10 万块压力测试。
- Android 生产触控适配器独立回归通过 103 项检查，包括五个导航圆钮的 Npad 发布、持续接触和释放，以及 guest touchscreen 触点数始终为零。
- 共享触控 49/49 用例、4474/4474 检查；技能手势 86/86 检查通过。
- 打包器 17 项、Android Python 检查 5 项通过。
- 正式签名、204 项载荷核验通过。未执行 Apple 整体构建。

独立 Android 探针构建：

```powershell
cmake --build D:/AndroidSwitch/build/android-ppg-slots-20261023 --target dm_android_audio_device_probe dm_android_audio_queue_probe dm_android_touch_bridge_fallback_probe -j8
```

## 产物

- APK：`D:/AndroidSwitch/output/releases/D2R-Mobile-1.2.3-94012-npc-audio-20261023/D2R-Mobile.apk`
- APK SHA-256：`ed72b73b3b36c0e434f7adab989651de5ce2d9d5d221575a698aa661da5b5f48`
- native SHA-256：`8d4db1f2b77da21bc94fb9229f3d523292e7428a7aeac3d71aae9d462d223769`
- 签名证书 SHA-256：`db6a9e98d27f4037df39b284699fef125ff83f6e92d6631ea250ce98ac747d0d`
- 本修复与 NPC 按键改动随后一并纳入 `1.2.4-94012` 的完整变更，屏幕适配与最新产物见 [屏幕比例验收记录](ANDROID_DISPLAY_ASPECT_2026-10-23.zh-CN.md)。语音验收时在 PPG 安装的是上述同版本、同签名、同原生库的诊断包。

## PPG 验收结果

PPG 序列号 `AVAY025422000548` 已连接并完成 NPC 语音验收。升级前已备份存档、配置与旧日志，位置为 `D:/AndroidSwitch/analysis/npc-audio-ppg-20261023/before-save-config.tar`，备份 134,656 字节。

- 安装 `D2R-Mobile-1.2.3-94012-npc-audio-ppg-debug-20261023/D2R-Mobile-debuggable.apk`，APK SHA-256 为 `09546a364be7b88a3bc8cdddec2401144b8e10e340d944541f858f5c378b38e5`。与普通离线 APK 的原生库 SHA-256 一致，差别为诊断可读权限。
- 进入现有 4 级角色，流亡者营地，卡夏（cls 150）。通过世界点击接近卡夏后停在一级“交谈 / 取消”；一次确认进入“介绍 / 闲聊 / 邪恶洞穴”二级菜单，再确认开始讲话。
- 日志记录 `guest_channels=6 device_channels=2 downmix=5.1-to-stereo`；当前图形配置为 Turnip Mesa 26.2.3、双槽。
- 日志记录 `OpenPanel ScrollSpeechPanel` 和相应关闭。用户在当前设备听感验收后回复“已经修复了”。NPC 对白无声问题验收通过。
- 完整记录与机器可读结果：`D:/AndroidSwitch/analysis/npc-audio-ppg-20261023/confirmed-game-startup.log`、`confirmed-status.json`、`acceptance-result.json`。
- 本轮仅为定位与菜单确认读取少量画面，其余使用日志和状态。未将语音确认扩大为长按确认、返回穿透、全部暂停恢复场景均通过；这些仍可单独回归。
