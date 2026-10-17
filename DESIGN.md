---
version: alpha
colors:
  primary: "#e9d9ba"
  background: "#0d0d0f"
  text: "#e9d9ba"
  statusScrim: "#0d0d0fe6"
typography:
  status:
    fontFamily: "Android system sans-serif, Noto Sans CJK"
    fontSize: "18px"
  fps:
    fontFamily: "Android system monospace"
    fontSize: "12px"
spacing:
  statusHorizontal: "28px"
  statusVertical: "18px"
  fpsHorizontal: "8px"
  fpsVertical: "4px"
  fpsTop: "10px"
omitted:
  - section: rounded
    reason: "Touch controls reuse the upstream circular targets, not card geometry."
---

## Overview
An Android host for the original offline 3D game. The original game's rendered
interface owns the visual identity. The shell's single job is to host that image
and give truthful preparation or failure feedback. Never replace game frames
with mock artwork or test geometry. Existing iOS gameplay is the sibling flow.

## Colors
GameActivity is the canonical owner of shell colors. The near-black background
keeps the unrendered Surface unobtrusive; warm parchment text follows the game's
interface materials. Status disappears only after actual successful presentation.
Primary shell actions reuse the same parchment value through TouchControlsView.TEXT.

## Typography
Use Android's locale-capable native font and scaled text. Chinese is the shell's
current language. Original game language follows Activity.Configuration.

## Layout
The root fills the available Android window. `GameViewport` centers the largest
16:9 rectangle within it; the SurfaceView and every gameplay/menu touch control
share that rectangle. The original 1600×900 / 1280×720 game image keeps its
proportions on wide phones and taller tablets, with near-black unused space.
This adapts presentation to the screen without changing the game's camera or HUD.
Preparation and failure occupy a stable full-window overlay. Gameplay controls
reuse the upstream C layout and gesture state machine.
Status padding is
in density-independent units and text sizing follows Android font scale.

## Elevation & Depth
The status scrim overlays rather than resizes the game surface. No shadows.

## Shapes
The original game owns game UI shape. The shell has a rectangular full-screen
surface and status scrim.

## Components
GameActivity owns shell state: prepare files, attach surface, start once, report
native progress, reveal real presentation, retain actionable failures. Surface
loss suspends graphics and drains before releasing its acquired native window.
Native text status supplies an accessibility live region. No network permission.

`GameDisplayMetrics` owns aspect-fit geometry and the rolling frame-count sample.
`GameActivity` owns a passive top-center `FPS 30.0` readout inside GameViewport.
It uses the existing native presented-frame counter and monotonic wall time,
updates after at least one second, shows `FPS —` before its first sample, and
resets on pause/resume. This measures the game's present submission cadence,
not physical-display scanout timing. It never intercepts touches or announces
every sample through an accessibility live region. The sampler can report zero
when the game stalls; no synthetic 30/60 value is substituted.

Runtime tokens remain canonical: FPS text/scrim reuse `TouchControlsView.TEXT`
and `INK`; GameActivity maps the documented 12sp monospace role, 8dp/4dp padding
and 10dp top inset to the native TextView. There are no new palette values.

## Android touch adapter
The unchanged `dm/ios/Sources/dm_touch_controls.c` owns layout, hit testing,
movement axes, skill aiming, skill-set timing and sampled-input pulse behavior.
`TouchControlsView` owns rendering tokens: ink #30241b at 50% opacity,
rim #a08359 at 73%, parchment #e9d9ba, pressed #80623c at 80%; its native
Canvas painter consumes these canonical Java constants. Coordinates use dp,
with original safe-inset and tall-screen scaling. Android cutout insets are
converted into GameViewport-local distances after layout, so centered margins
are not applied a second time. Buttons remain anchored while
pressed; the joystick knob follows actual output axes. Labels describe controller
slots until native skill identity is audited. Virtual accessibility buttons expose
clicks and directional movement without dragging.

Menu and combat modes are explicitly selected because this Android build's
91738 scene observer is not mapped yet. The mode switch cancels all held input.
Unclaimed combat touches continue through original HID touchscreen states.
Portal, difficulty mutation, multi-row belt actions and inventory mouse/drag
bridges stay absent until the current game's native identities are established.
Never copy 94012 guest addresses into this Android build.

## Do's and Don'ts
`RendererSettingsDialog` owns the native System menu's rendering controls.
It follows `ShellTextInputDialog`: Android AlertDialog and ScrollView own modal
focus, Back, scrollbars, responsive bounds and accessibility. Native Switch
controls own checked semantics and keyboard operation; the system-menu entry
uses a native Button. Ink, rim, pressed and parchment reuse TouchControlsView
tokens. No game camera, viewport, touch-layout or palette value changes.

The menu explains clearing and writeback in player-facing terms. Changes save
atomically to one private settings file and apply at a rendering frame boundary;
show requested versus actually applied state, and retain the prior selection
with an inline retry message on failure. This is a reversible setting with no
confirmation step. Closing returns focus to the trigger. Backgrounding or an
SDK text-entry request dismisses the menu and releases held input. The game's
original System/pause screen remains owned by the original game.

| Capability | Canonical owner | Source of truth | Allowed variants | Verification |
| --- | --- | --- | --- | --- |
| Rendering preferences | RendererOptionsStore / RendererOptions | attachment-mode.txt; native applied flags | two independent switches | Java model + device logs |
| Modal and scrollbar | Android AlertDialog / ScrollView | ShellTextInputDialog sibling | renderer settings | manual device interaction |
| Feedback | RendererSettingsDialog status TextView | nativeStatus renderer_flags | applied, pending, failed | native ack and operator check |

`ShellTextInputDialog` owns all Android SDK text-entry modals. It uses native
EditText/IME and AlertDialog focus, Back and accessibility behavior; the panel
uses opaque ink derived from TouchControlsView.INK and shared parchment TEXT.
The SDK supplies title, description, initial text and UTF-16 length bounds.
Confirm returns actual entered text once; Cancel, Back, backgrounding and
Activity destruction return the SDK cancellation result and release the wait.
Invalid length retains the text with a correction message. Input never creates
or submits a game character automatically; the original game validates names
and owns its create action. The game surface remains behind the modal.

Keep engine paths, shader numbers and compatibility terminology in diagnostics.
Never claim game completion from a frame counter: real 3D scene evidence is a
separate device verification gate. Preserve original game resources and saves.

## Offline installation

`InstallerActivity` owns the launcher and the one-time resource preparation flow.
The Chinese-language audience is a phone/tablet user with a downloaded APK and
one matching `.d2rpack`, with no desktop tools or file-management knowledge.
Native Android Buttons and ACTION_OPEN_DOCUMENT own keyboard, touch, file-picker
geometry and accessibility; no browser controls or broad storage permission are
introduced. The existing GameActivity preparation overlay is the sibling flow.

The visual signature is the real three-stage sequence: choose resources, import
and verify, start the game. The quiet single-column screen reuses the existing
parchment/ink palette and system CJK font, with a 28sp bold heading, 17sp body,
14sp help/utility text, 24dp outer spacing, 640dp maximum content width, and 52dp
minimum native button height. It scrolls on short/narrow screens, respects system
insets and font scaling, and reserves status, progress and action geometry across
idle, progress, error and retry. No new artwork or palette is needed.

Runtime tokens remain canonical (ownership model B): TouchControlsView.TEXT maps
to body/button text; RIM maps to progress and disabled text; opaque RIM maps to
the small brand label; opaque INK maps to button backgrounds; PRESSED maps to
pressed and focused button states. The existing #0d0d0f shell background is
reused. Native buttons retain focus/ripple feedback and disabled semantics;
status changes use the existing polite accessibility-live-region pattern.

`OfflineInstallSession` owns one process-wide worker and no Activity reference.
It serializes file preparation, cancels import when leaving the foreground,
ignores duplicate selections, and only opens GameActivity after verified commit.
Byte progress is determinate during copy/hash, unknown checks are indeterminate,
and commit is named separately. Cancellation, format/version errors, truncated
or corrupt content, and insufficient space each retain a visible recovery path.
The screen promises reuse of completed verified files, not byte-level resume of
an interrupted file. Initial installation asks for one system document selection;
normal subsequent launches use the private receipt plus file size/mtime checks.
Changed or legacy resources receive a full hash check before adoption.

`OfflinePackageInstaller` owns pinned manifests, bounded streaming, disk planning,
staging, full-content SHA-256 checks and restart recovery. Game saves are outside
its path allowlist. A matching pending commit can complete without the source
pack; an incomplete import requires selecting the pack again. No network or
storage permission is requested. Installation success is independent of game
playability or compatibility on other GPUs.

The prepared launcher stays open with a native “启动游戏” button so users can
recover from a driver failure on their next launch. First-time import still
starts the game automatically after verification. A native vertical RadioGroup
exposes “自动推荐（推荐）”, “系统驱动” and “内置兼容驱动（Turnip）” without a popup.
InstallerActivity owns those native radio controls and its existing ScrollView;
GraphicsDriverSettings owns the saved choice and applies it before Vulkan starts.
GraphicsDriverPolicy owns hardware recommendations. Native 48dp radio targets
reuse parchment TEXT and opaque RIM, Android focus/checked semantics and the
existing system CJK font. A stable three-line description names the recommendation
or recovery action. Selection is saved immediately; startup consumes the saved
choice. The source of this behavior is the user's request for driver selection
at the entry point; no extra confirmation or network permission is involved.

The launcher icon is a separate original raster emblem: obsidian iron, antique
bronze and molten red identify the dark-fantasy game at small sizes. The preserved
master and generation prompt live in `dm/android/artwork/`; the icon packer owns
resampling. Adaptive art stays inside the central 60% safe region and uses the
existing shell background; five legacy density assets keep older launchers
readable. This branding does not change the original in-game visual identity.
