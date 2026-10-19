#pragma once
#include <stdint.h>

int dm_drop_haptic_enabled(void);
void dm_drop_haptic_land(uint8_t *base, uint64_t unit);
int dm_drop_haptic_take(void);
