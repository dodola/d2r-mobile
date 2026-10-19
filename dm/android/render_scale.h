#pragma once

/* UI threads only publish a request. Guest target reconstruction runs in the
 * original notification context, never under the present/render lock.
 * A numeric request is fixed and overrides the 94012 dynamic controller;
 * auto mode clears that override so the controller can lower the 3D scale
 * when presented frames miss their target. The Activity restores its saved
 * preference (auto when no preference exists) before guest startup. */
int dm_android_render_scale_request(float scale);
int dm_android_render_scale_auto(void);
float dm_android_render_scale_requested(void);
