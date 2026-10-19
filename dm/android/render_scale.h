#pragma once

/* UI threads only publish a request. Guest target reconstruction runs in the
 * original notification context, never under the present/render lock.
 * A request is fixed and overrides the 94012 dynamic controller. The Activity
 * restores its saved preference (or original 1.0) before guest startup. */
int dm_android_render_scale_request(float scale);
float dm_android_render_scale_requested(void);
