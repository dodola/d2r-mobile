# Android shoulder and first-person camera

中文: [over-the-shoulder-camera.zh-CN.md](over-the-shoulder-camera.zh-CN.md)

Adapted from upstream `dc5778d` for the D2R 3.3.94012 Android Vulkan backend. Select Original/Shoulder/First person on the custom Camera view row and apply; original is the default. Drag vertically on empty right-side space for shoulder pitch; first person also accepts horizontal turning. Turn buttons and movement stick work together.

`nvn_vk_cam.h` contains pure row-vector, reversed-Z math. `nvn_vk_cam_runtime.inc` recognizes 608-byte main and 1440-byte scene constants, modifying view/projection/inverse/previous-frame/depth fields in immutable UBO copies. Prior GPU writes are published first. Buffers written by the current instruction retain the original alias path so copies cannot break writes. Known orthographic culling shader `1188eaedc8db3285` is excluded. Signatures/addresses apply only to this game version.

`dm_cam.c` publishes original/perspective matrices and converts sticks, taps, names/health bars/ground-item labels and offscreen threats. The label gate is original-program `main+0x1237c60`. `dm_world.c` measures walls on the collision grid; pure math recovers the rig from original height. Turns update input references. Mode changes withdraw old matrices immediately and refresh game-thread references by generation.

Test override `DM_CAM=0/1/2` wins over settings; invalid values disable it. `DM_CAM_SENSOR=1` uses gyroscope rotation around gravity, `=2` uses steering tilt, default off. Sensors listen only during active foreground touch gameplay with the camera running. `DM_CAM_DIST/H/LOOK/LOOKH/EYE/FWD/PITCH/FOV/TAU/NEAR/REACH` tune the rig for diagnostics. Background, lost focus, menus, dialogs or stopped camera remove callbacks/sensors; touch cancellation clears undelivered turns.

CPU math, integration and input tests pass. Full device visuals, skill aiming and all-scene acceptance were not performed. See the [port record](UPSTREAM_ANDROID_PORT_2026-10-28.md) for evidence.
