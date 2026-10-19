/* Native input regression: exercise the real backend, not a copied model.
 * On Android link with input_backend.c and the ordinary host dependencies.
 * An offline single-thread harness may stub platform locks/logs/touch cleanup;
 * that checks state semantics only, not concurrency or Activity dispatch. */
#include "../input_backend.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

static int ready=1;
static int source_ready(void *opaque){(void)opaque;return ready;}
static uint64_t buttons(void){dm_android_npad_state s;assert(!dm_android_input_read_npad(&s,0,1));return s.buttons;}
int main(void){
    assert(dm_android_input_publish_controller_buttons(2)==ENODEV);
    dm_android_input_config c={.source="offline-controller-regression",.panel_width=1280,.panel_height=720,
        .source_ready=source_ready,.logical_npad_id=0,.logical_npad_style=1};
    assert(!dm_android_input_bind(&c));assert(!dm_android_input_initialize_npad());
    assert(!dm_android_input_publish_controller_buttons(2));assert(!dm_android_input_publish_controller_buttons(0));
    assert(buttons()==2);assert(buttons()==0); // short press latch consumed once
    assert(!dm_android_input_publish_controller_buttons(2));
    assert(!dm_android_input_publish_actions(4,0,0,0,0));assert(buttons()==6);
    assert(!dm_android_input_publish_actions(0,0,0,0,0));assert(buttons()==2); // touch release preserves held controller B
    assert(!dm_android_input_publish_actions(4,0,0,0,0));dm_android_input_cancel_touch();assert(buttons()==2);
    assert(!dm_android_input_publish_controller_buttons(0));assert(buttons()==0);
    assert(!dm_android_input_publish_actions(2,0,0,0,0));
    assert(!dm_android_input_publish_controller_buttons(2));assert(buttons()==2);
    assert(!dm_android_input_publish_controller_buttons(0));assert(buttons()==2); // controller release preserves held touch B
    assert(!dm_android_input_publish_actions(0,0,0,0,0));assert(buttons()==0);
    assert(!dm_android_input_publish_controller_buttons(2));assert(!dm_android_input_cancel_controller(2));assert(buttons()==2);
    assert(dm_android_input_cancel_controller(1ull<<20)==EINVAL);assert(buttons()==2);
    assert(!dm_android_input_publish_controller_buttons(0));assert(buttons()==0); // retaining another device creates no fresh edge
    assert(!dm_android_input_publish_controller_buttons(2));assert(!dm_android_input_cancel_controller(0));assert(buttons()==0);
    assert(!dm_android_input_publish_controller_buttons(2));assert(!dm_android_input_publish_actions(4,0,0,0,0));
    dm_android_input_cancel();assert(buttons()==0);
    assert(dm_android_input_publish_controller_buttons(1ull<<20)==EINVAL);assert(buttons()==0);
    ready=0;assert(dm_android_input_publish_controller_buttons(2)==ENODEV);assert(!dm_android_input_publish_controller_buttons(0));
    ready=1;assert(buttons()==0);
    // Removing one controller must preserve another device's analog stick,
    // even when no buttons remain held. Cancellation must not take touch mode.
    assert(!dm_android_input_publish_controller_actions(2,20000,0,0,0));
    assert(!dm_android_input_cancel_controller_actions(0,20000,0,0,0));
    dm_android_npad_state state;
    assert(!dm_android_input_read_npad(&state,0,1));assert(state.buttons==0 && state.lx==20000);
    assert(!dm_android_input_cancel_controller_actions(0,0,0,0,0));
    assert(!dm_android_input_read_npad(&state,0,1));assert(state.buttons==0 && state.lx==0);
    dm_android_input_touch_activity();assert(!dm_android_input_controller_mode());
    assert(!dm_android_input_cancel_controller_actions(2,20000,0,0,0));
    assert(!dm_android_input_controller_mode());
    assert(!dm_android_input_cancel_controller_actions(0,0,0,0,0));assert(buttons()==0);
    assert(dm_android_input_cancel_controller_actions(1ull<<20,0,0,0,0)==EINVAL);
    assert(dm_android_input_cancel_controller_actions(0,32768,0,0,0)==EINVAL);
    assert(dm_android_input_cancel_controller_actions(0,0,0,0,-32768)==EINVAL);
    assert(buttons()==0);
    puts("Controller B: tap latch, held keys, touch coexistence, cancellation and invalid input passed.");
    return 0;
}
