#pragma once

/* UI threads only publish a request. Guest target reconstruction runs in the
 * original notification context, never under the present/render lock. */
int dm_android_render_scale_request(float scale);
float dm_android_render_scale_requested(void);
