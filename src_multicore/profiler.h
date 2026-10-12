#ifndef MULTICORE_PROFILER_H
#define MULTICORE_PROFILER_H

#include "common.h"

/*
 * Sampling profiler for the game's main thread (diagnostic, off by default).
 *
 * A background thread suspends the main thread about 1000 times a second,
 * records its instruction pointer and the return addresses on its stack,
 * and resumes it. Samples are tagged with the zone the main thread reported
 * (logic phase, 30 Hz effects, everything else). Every report period the
 * counts go to fps_profile.log as RVAs, to be named with Ghidra offline.
 */

typedef enum ProfileZone {
    /* Everything else on the main thread: drawing, UI, frame-limiter waits. */
    PROFILE_ZONE_CLIENT = 0,
    PROFILE_ZONE_LOGIC = 1,
    PROFILE_ZONE_EFFECTS = 2,
    PROFILE_ZONE_COUNT = 3
} ProfileZone;

/* Written by the main thread only; read by the sampler. */
extern volatile LONG g_profile_zone;

static inline LONG profile_enter(ProfileZone zone) {
    LONG previous = g_profile_zone;
    g_profile_zone = (LONG)zone;
    return previous;
}

static inline void profile_leave(LONG previous) {
    g_profile_zone = previous;
}

typedef struct ProfilerSetup {
    const wchar_t *output_path;
    u8 *game_module;
    u32 game_image_size;
    const char *target_name;
    u32 pe_timestamp;
    u32 report_seconds;
} ProfilerSetup;

/* Starts sampling the calling thread (call it on the game's main thread). */
BOOL profiler_start(const ProfilerSetup *setup);
/* Once per limited client frame, on the main thread. */
void profiler_note_frame(u32 rate);

#endif
