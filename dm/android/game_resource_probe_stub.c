#include "../host/hle.h"

/* The real image readback probe is generated only for a locally supplied
 * RomFS.  The production Android Activity still verifies and mounts the
 * image before starting; this stub keeps ordinary source builds independent
 * of private sample bytes. */
void dm_android_real_resource_probe(const char *root) {
    (void)root;
    LOG("ANDROID_REAL_RESOURCE_READ_SKIPPED samples_header_absent=true");
}
