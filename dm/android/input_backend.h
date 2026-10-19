#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t id; float x, y; } dm_android_pointer;
typedef struct {
    const char *source;
    uint32_t panel_width, panel_height;
    // Must read the actual installed Android listener/route readiness. This
    // callback must be nonblocking and must not call back into input_backend.
    int (*source_ready)(void *opaque);
    void *opaque;
    // 0 = no logical action channel (no connected Npad is reported).
    // 1 = FullKey; 2 = Handheld. These describe logical Android input, not a
    // claim that a physical Switch controller has been attached.
    uint32_t logical_npad_id, logical_npad_style;
} dm_android_input_config;
typedef struct {
    int64_t sampling;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes, reserved;
} dm_android_npad_state;
typedef struct {
    int bound, touch_initialized, npad_initialized;
    uint32_t requested_styles, requested_ids[10], requested_id_count;
    int handheld_activation_mode;
    uint64_t touch_publications, touch_samples, action_publications, npad_samples;
    uint64_t input_resets;
    /* Copy-only diagnostics: publication state and the last sample actually
     * returned after title filters and lifecycle generation validation. */
    uint64_t action_buttons, controller_buttons, raw_buttons;
    uint64_t returned_buttons_seen, returned_nonzero_samples;
    uint32_t returned_id, returned_style;
    dm_android_npad_state returned_state;
    int source_ready, controller_mode;
} dm_android_input_status;
// All configuration/publication/init methods return 0 or positive errno.
int dm_android_input_bind(const dm_android_input_config *config);
int dm_android_input_publish_touch(const dm_android_pointer *points, size_t count);
/* For NativeActivity worker snapshots which can cross a lifecycle reset. */
int dm_android_input_publish_touch_at_generation(const dm_android_pointer *points, size_t count, uint64_t generation);
void dm_android_input_cancel(void);
/* Touch cancellation must not release a physically held controller key. */
void dm_android_input_cancel_touch(void);
int dm_android_input_publish_actions(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
/* Physical controller snapshots are kept separate from the touch/menu
 * action channel.  This preserves the iOS contract: controller buttons are
 * merged with touch buttons, while controller sticks are suppressed while
 * touch mode is active and use 25%/15% hysteresis when taking it back. */
int dm_android_input_publish_controller_actions(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
/* Reject a worker snapshot sampled before a full input reset.  The generation
 * comparison and publication are atomic with respect to cancellation. */
int dm_android_input_publish_controller_actions_at_generation(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry, uint64_t generation);
/* Mark an actual touch interaction.  A displaced physical stick must first
 * return to centre before taking control back, matching dm_gamepad_touched. */
void dm_android_input_touch_activity(void);
int dm_android_input_controller_mode(void);
/* Controller buttons have their own source lifetime, so a MotionEvent or
 * touch overlay reset cannot erase a held physical button. */
int dm_android_input_publish_controller_buttons(uint64_t buttons);
/* Full remaining snapshot: cancellation never creates edges or takes input mode. */
int dm_android_input_cancel_controller_actions(uint64_t remaining_buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
int dm_android_input_cancel_controller(uint64_t remaining_buttons);
int dm_android_input_initialize_touch(void);
int dm_android_input_initialize_npad(void);
int dm_android_input_set_ids(const uint32_t *ids, size_t count);
int dm_android_input_set_styles(uint32_t styles);
int dm_android_input_set_handheld_mode(int mode);
int dm_android_input_get_style(uint32_t id, uint32_t *style);
int dm_android_input_bind_style_event(uint32_t id,void *event,int autoclear);
void dm_android_input_style_changed(void);
int dm_android_input_read_npad(dm_android_npad_state *out, uint32_t id, uint32_t style);
// read_touch returns number of states (0 or 1) or negative errno.
int dm_android_input_read_touch(void *out, int capacity);
void dm_android_input_status_get(dm_android_input_status *out);
