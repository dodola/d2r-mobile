# Android 手动验收流程

本文用于验收 D2R 3.3.94012 的 Android 包。目标是确认新版本已经越过 TACT `0x80` 关卡，并验证触控桥、存档、HUD、实体手柄和更新器。测试只操作隔离包 `org.d2r.newprobe`，不要卸载或清除该包的数据；游戏 RomFS 和模块位于应用数据目录，清除数据后需要重新部署。

## 测试包

当前候选包：

```text
D:\AndroidSwitch\analysis\parity-20261019\d2r-new\test-apk-driver-v10\d2r-94012-driver-v10.apk
```

```text
APK SHA-256:
c9482296691bd25528d3f1bf94390e4273bd492fbb2ff03e49ef38e7d51566ec

native SHA-256:
90d44db654fb519830b5c66dd46516c7e62be7188eaffbe5db879a8b10d93322

游戏模块 Build ID:
db0507f36b84200ac05f75d76ed5026f
```

该包使用 `GameActivity` 直接启动游戏，并包含 Turnip sysmem 驱动、命令状态缓存和 Turnip 管线缓存种子。设备上已有的 `files/game.romfs`、模块和存档会继续使用。需要系统 Adreno 驱动时，使用 `test-apk-strict-v10\d2r-94012-test.apk`。

## 准备

打开 PowerShell，设置 ADB 路径：

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb devices -l
```

确认目标设备显示为 `device`。安装候选包时要保留应用数据：

```powershell
& $adb install -r "D:\AndroidSwitch\analysis\parity-20261019\d2r-new\test-apk-driver-v10\d2r-94012-driver-v10.apk"
& $adb shell dumpsys package org.d2r.newprobe | Select-String "versionCode|versionName|lastUpdateTime"
& $adb shell run-as org.d2r.newprobe touch files/d2r-mobile/diagnostics/private-vulkan-sysmem
```

预期版本为 `versionCode=4`。测试前建议接上充电器；如果测试实体手柄，先把手柄连接到设备。Turnip 启动日志应出现 `driver id=18`、`Mesa 26.2.3` 和 `PIPELINE_WARM_START entries=125`；如果出现系统驱动则是 `driver id=8`。

## 1. 冷启动与 TACT 关卡

```powershell
& $adb shell am force-stop org.d2r.newprobe
& $adb logcat -c
& $adb shell am start -W -n org.d2r.newprobe/org.d2r.binaryprobe.GameActivity
```

等待游戏进入主菜单，通常需要 20 到 40 秒。读取游戏自己的启动日志：

```powershell
& $adb exec-out run-as org.d2r.newprobe cat files/d2r-mobile/diagnostics/game-startup.log > "$env:TEMP\game-startup.log"
Select-String -Path "$env:TEMP\game-startup.log" -Pattern "ANDROID_D2R_HOST_ATTACH|ANDROID_TOUCH_GAME_ATTACH|ANDROID_GAME_STAGE|FATAL|host call 0x80"
```

通过条件：

- 出现 `ANDROID_D2R_HOST_ATTACH ...`。
- 出现 `ANDROID_TOUCH_GAME_ATTACH result=1 host=1`。
- 出现 `ANDROID_GAME_STAGE 游戏画面已呈现`。
- 日志中没有 `FATAL Android build-specific host call 0x80`。
- 进程仍在运行：

```powershell
& $adb shell pidof org.d2r.newprobe
```

## 2. 触控桥

进入角色后，在游戏场景中完成以下操作：

1. 按住左下浮动摇杆，分别向上、下、左、右移动，确认角色连续移动，松手后停止。
2. 点按主技能和技能位按钮，确认按钮有按下反馈，并触发对应技能。
3. 按住战斗技能，确认持续施放或瞄准流程符合 iOS 行为。
4. 打开角色面板、背包和技能树，确认靠边面板可以打开、关闭并返回游戏。新版不再显示 `菜单操控` 与 `系统` 浮层按钮；进入“选项 → 自定义 → Android 设置”检查画质、图形优化、地图、MOD 和 FPS 开关，关闭后应返回自定义页且保留待应用值。分别检查局外点按、局内触控和实体手柄输入后的首触接管。
5. 在宽屏或平板设备上检查 HUD 是否保持在安全区域，摇杆和技能按钮没有落到屏幕外。

检查触控输入日志：

```powershell
Select-String -Path "$env:TEMP\game-startup.log" -Pattern "ANDROID_UPSTREAM_TOUCH|ANDROID_NPAD_RETURN" | Select-Object -Last 20
```

摇杆持续拖动时应出现非零轴值，松手后应回到 `axes=0,0,0,0`。日志中的按钮位只作为传输证据，技能是否命中、商人是否成交仍需看游戏内实际结果。

## 3. 存档与冷启动读档

选择或创建一个测试角色，进入游戏，移动到一个容易辨认的位置，然后执行普通后台保存：

```powershell
& $adb shell input keyevent 3
Start-Sleep -Seconds 8
Select-String -Path "$env:TEMP\game-startup.log" -Pattern "ANDROID_SAVE_REQUEST|ANDROID_SAVE_APPLY|ANDROID_SAVE_WAIT|存档提交|background save"
```

通过条件：

- 至少出现一次 `ANDROID_SAVE_REQUEST ... result=1`。
- 出现 `ANDROID_SAVE_WAIT ... commits=`，并完成一次 `存档提交`。
- `files/d2r-mobile/save/<角色名>/` 中存在非空的 `.d2s`、`.key` 和 `.map`。

随后做完整冷启动：

```powershell
& $adb shell am force-stop org.d2r.newprobe
& $adb shell am start -W -n org.d2r.newprobe/org.d2r.binaryprobe.GameActivity
```

等待角色选择页出现，确认目标角色仍在列表中，进入角色并确认等级、位置和背包内容与保存前一致。再退出游戏或返回主菜单一次，确认存档目录没有被新进程覆盖成空目录。

如果需要单独测试暂停保存，先打开游戏内暂停页，再让应用进入后台；日志应同时出现暂停相关 observer 和存档提交。若角色处于战斗、死亡或切换地图状态，应分别记录结果。

## 4. 实体手柄

此步骤需要真实硬件手柄。ADB 注入的按键或摇杆事件不能作为实体手柄验收证据。

1. 用 USB 或蓝牙连接手柄，确认系统输入设备中出现手柄。
2. 进入游戏，检查移动摇杆、右摇杆、ABXY、十字键、肩键和扳机。
3. 让角色在场景中移动并攻击，确认没有轴漂移、按键卡住或重复触发。
4. 断开手柄，确认触摸重新接管；重新连接后确认手柄重新接管，摇杆先回中再产生新输入。
5. 在多手柄场景下只移除一只设备，确认另一只手柄和触摸状态没有被错误清除。

可在日志中搜索输入绑定和设备切换：

```powershell
Select-String -Path "$env:TEMP\game-startup.log" -Pattern "ANDROID_INPUT_BIND|physical_switch_controller|controller|gamepad|ANDROID_NPAD_RETURN"
```

记录手柄型号、连接方式、按下/松开结果和重连时间。没有真实手柄时，这一项应标记为未验收。

## 5. HUD 与 iOS 视觉对照

在同一个角色场景分别截取主界面、技能按下、背包、菜单、地图和药水数量变化画面。对照 iOS 包检查：

- 技能图标的普通帧、按下帧、空槽、副手和充能角标。
- 左右生命/法力球、药水、回城、地图、菜单和难度条的相对位置。
- 触摸布局与手柄布局切换后，六个 HUD 控件的身份是否重新建立。
- 面板打开、角色死亡、过场和加载画面下 HUD 是否正确隐藏或恢复。
- 中英文切换后，菜单、触控提示和无障碍文本是否同步变化。

本轮候选包的触控移动、普通后台存档、强制停止后的冷启动读档以及关键 host 日志已在设备上走通；实体手柄、真实交易和技能加点、秘境和所有 HUD 状态仍需按上面的场景继续验收。

## 6. 更新器

当前 v10 候选包没有写入更新清单地址，因此不会请求更新。要验证更新器，使用打包工具的 `--update-url` 重新生成一个测试包，并把地址指向可返回 JSON 的 HTTPS 端点：

```json
{
  "build": 5,
  "note": "测试版本说明",
  "urgent": false,
  "install_url": "https://example.com/d2r-android.apk"
}
```

构建命令示例：

```powershell
python dm/tools/android_d2r_apk.py `
  --repo D:\AndroidSwitch\runtime\d2r-mobile `
  --sdk "$env:LOCALAPPDATA\Android\Sdk" `
  --java "C:\Program Files\Java\jdk-21" `
  --build-tools 35.0.0 `
  --android-platform android-35 `
  --native "D:\AndroidSwitch\build\android-strict-main-20261019\libd2r_mobile_android.so" `
  --images "D:\AndroidSwitch\analysis\parity-20261019\d2r-new\android-images-touch-run-v4" `
  --hud-assets "D:\AndroidSwitch\analysis\parity-20261019\d2r-new\android-hud-assets" `
  --out "D:\AndroidSwitch\analysis\parity-20261019\d2r-new\test-apk-update-manual" `
  --update-url "https://example.com/d2r-version.json"
```

在角色选择页检查普通和紧急两种提示；点击提示或按 Y 应打开系统浏览器。相同或更旧的构建号不应留下旧提示。把地址改成 HTTP、错误主机或返回超限响应时，确认不会打开安装页，失败提示只在没有已有更新提示时出现。

画质对比可在 `系统 → 画质模式` 中选择：自动、100%、90% 和 75%。自动模式允许游戏内动态降到 60%，100% 固定 1280×720 的 3D 内部尺寸；输出分辨率仍为 1280×720。

## 结果记录

每轮至少记录：

- APK SHA-256、native SHA-256、游戏模块 Build ID。
- 设备型号、Android 版本、手柄型号和连接方式。
- 冷启动是否越过 `0x80`，以及关键日志行。
- 触控、存档、手柄、HUD 和更新器的逐项结果。
- 失败场景的截图、日志时间点和复现步骤。

`premium-audit.json` 是受保护文件，不读取、不提交，也不要放入测试证据目录。
