/* Isolated real-AAudio probe; no game assets or guest SDK event fixture.
 * Six-channel input is centre-only to cover the NPC dialogue path. */
#include "../../host/hle_audio.c"

static struct { const void *key; void *value; } objects[8];
uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
void *objmap_get(const void *key) {
    for (unsigned i = 0; i < 8; ++i) if (objects[i].key == key) return objects[i].value;
    return NULL;
}
void objmap_put(const void *key, void *value) {
    for (unsigned i = 0; i < 8; ++i) if (!objects[i].key) { objects[i].key = key; objects[i].value = value; return; }
    abort();
}
void *objmap_del(const void *key) {
    for (unsigned i = 0; i < 8; ++i) if (objects[i].key == key) { objects[i].key = NULL; return objects[i].value; }
    return NULL;
}
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; abort(); }
void hle_signal_system_event(void *event) { (void)event; abort(); }

int main(void) {
    const int channels[] = {6, 2, 1};
    int failed = 0;
    for (unsigned i = 0; i < sizeof channels / sizeof *channels; ++i) {
        dm_android_audio_metrics before, after;
        dm_android_audio_snapshot(&before);
        after = before;
        int result = dm_android_audio_probe_channels(channels[i], &after);
        failed |= result != 0;
        printf("{\"guest_channels\":%d,\"device_channels\":%d,\"centre_only\":%s,\"result\":%d,"
               "\"callbacks\":%llu,\"game_bytes\":%llu,\"nonzero_output_samples\":%llu,\"real_device_test\":true}\n",
               channels[i], channels[i] == 6 ? 2 : channels[i], channels[i] == 6 ? "true" : "false", result,
               (unsigned long long)(after.render_callbacks - before.render_callbacks),
               (unsigned long long)(after.game_bytes_copied - before.game_bytes_copied),
               (unsigned long long)(after.nonzero_samples - before.nonzero_samples));
    }
    return failed;
}
