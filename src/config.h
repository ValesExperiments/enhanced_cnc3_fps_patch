#ifndef FPS_PATCH_CONFIG_H
#define FPS_PATCH_CONFIG_H

#include "common.h"

typedef struct Config {
    BOOL enabled;
    /* Parsing can succeed while a value is outside the supported schedules. */
    BOOL valid;
    u32 target_fps;
    BOOL logging;
    /* Diagnostic: client frames of one moving vehicle to log (0 = off). */
    u32 debug_motion_frames;
    /* Diagnostic: client frames of frame-pacing data to log (0 = off). */
    u32 debug_frame_times;
} Config;

void config_set_defaults(Config *config);
BOOL config_load(Config *config, const wchar_t *path);

#endif
