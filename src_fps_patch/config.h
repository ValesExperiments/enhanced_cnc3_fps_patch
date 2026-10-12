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
    /* Lower the client rate while the PC can't hold it (keeps 100% game speed). */
    BOOL adaptive_fps;
    /* Diagnostic: trail-system draws to log (0 = off). */
    u32 debug_trail_draws;
    /* Diagnostic: 1 = queued UI (APT) calls per call site each second; 2 = per-frame trace. */
    u32 debug_apt_calls;
    /* Diagnostic: lines of per-frame motion for newly created objects. */
    u32 debug_spawn_frames;
    /* Diagnostic: comma-separated patch features to skip (bisecting glitches). */
    char debug_disable[512];
    /* Diagnostic: sample the main thread into fps_profile.log (src_multicore). */
    BOOL profiler;
    u32 profiler_report_seconds;
    /* src_multicore M1: instant particle-list insertion point. */
    BOOL multicore_particle_index;
    /* Diagnostic: also run the original searches and count disagreements. */
    BOOL multicore_verify;
    /* src_multicore M3a: no GPU-memory read in the trail vertex builder. */
    BOOL multicore_trail_fix;
} Config;

void config_set_defaults(Config *config);
BOOL config_load(Config *config, const wchar_t *path);

#endif
