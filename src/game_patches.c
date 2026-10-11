#include "game_patches.h"

#include "log.h"
#include "memory_patch.h"

#include <math.h>

/*
 * Hook callbacks cannot carry user data, so they retain the immutable game
 * description selected during installation. Everything else stays local to
 * the installer below.
 */
static const GameLayout *g_game;
static const Config *g_config;
static BOOL g_installed;
/* Process-lifetime storage used as replacement targets for absolute operands. */
static float g_visual_client_step;
static float g_client_frames_per_millisecond;
static float g_camera_scroll_scale;
static float g_camera_shake_decay;
static u32 g_retail_visual_fps = 30u;
/* Fractional-rate state is reset whenever a game session starts. */
static u32 g_particle_update_accumulator;
/* Number of 30 Hz CPU particle simulation steps run so far. */
static u32 g_particle_sim_tick;
static u32 g_w3d_time_remainder;
static u32 g_audio_update_accumulator;
static u32 g_limiter_interval_remainder;
static u32 g_limiter_interval_divisor;
static u32 g_keyboard_last_poll_ms;
static u32 g_keyboard_time_remainder;
static BOOL g_keyboard_repeat_tick;
static BOOL g_keyboard_clock_initialized;
/* Live GlobalData camera settings may be restored between game sessions. */
static float g_retail_camera_adjust_speed;
static float g_applied_camera_adjust_speed;
static float g_retail_keyboard_rotate_speed;
static float g_applied_keyboard_rotate_speed;
static BOOL g_camera_settings_captured;

enum {
    LOGIC_FPS = 15u,
    RETAIL_CLIENT_FRAMES_PER_LOGIC_TICK = 2u,
    LOGIC_PHASE_COUNT = 6u,
    W3D_RETAIL_MILLISECONDS_PER_SECOND = 990u
};

typedef u32(THISCALL *GameClientGetFrameNumberFn)(void *game_client);
typedef void(THISCALL *SubsystemUpdateFn)(void *subsystem);
typedef u8(THISCALL *AudioUpdateFn)(void *audio_manager);
typedef void(THISCALL *KeyboardPollFn)(void *keyboard);
typedef u8(THISCALL *KeyboardCheckRepeatFn)(void *keyboard);
typedef void **(THISCALL *AddIndicatorFn)(void *stage, void **out_indicator, void *target);
typedef int(THISCALL *W3DViewScrollByFn)(void *view, const float *delta);
typedef float(THISCALL *W3DViewGetZoomFn)(void *view);
typedef int(THISCALL *W3DViewSetZoomFn)(void *view, float zoom);

typedef struct ZoomStep {
    float multiplier;
    float offset;
} ZoomStep;

static AddIndicatorFn g_original_add_indicator[ADD_INDICATOR_COUNT];
static W3DViewScrollByFn g_original_scroll_by;
static AudioUpdateFn g_original_audio_update;
static KeyboardPollFn g_original_keyboard_poll;
static KeyboardCheckRepeatFn g_original_keyboard_check_repeat;
static ZoomStep g_zoom_in_step;
static ZoomStep g_zoom_out_step;

typedef struct PatchInstaller {
    /* A mismatch poisons the remaining install; the caller rolls back once. */
    PatchTransaction transaction;
    BOOL ok;
} PatchInstaller;

static BOOL select_camera_rate_constants(u32 target_fps) {
    const float zoom_in_retail_multiplier = 0.96f;
    const float zoom_out_retail_multiplier = 1.05f;
    float exponent;

    if (target_fps == 0) return FALSE;
    exponent = (float)g_retail_visual_fps / (float)target_fps;
    /*
     * Held zoom is an affine recurrence, not a linear distance. These are the
     * fractional affine steps of the retail 30 Hz transforms:
     *
     *   zoom in:  z' = 0.96*z - 1
     *   zoom out: z' = 1.05*z + 1
     *
     * Repeating each selected step target_fps times produces the same
     * one-second transform as repeating the retail step thirty times.
     */
    g_zoom_in_step.multiplier = powf(zoom_in_retail_multiplier, exponent);
    g_zoom_in_step.offset =
        -1.0f * (1.0f - g_zoom_in_step.multiplier) /
        (1.0f - zoom_in_retail_multiplier);
    g_zoom_out_step.multiplier = powf(zoom_out_retail_multiplier, exponent);
    g_zoom_out_step.offset =
        (1.0f - g_zoom_out_step.multiplier) /
        (1.0f - zoom_out_retail_multiplier);
    g_camera_shake_decay = powf(0.75f, exponent);
    g_camera_scroll_scale = exponent;
    return TRUE;
}

static float normalize_camera_adjust_speed(float retail, u32 target_fps) {
    /* Values outside (0,1) are nonstandard overshoot modes; preserve them. */
    if (retail <= 0.0f || retail >= 1.0f) return retail;
    return 1.0f - powf(1.0f - retail,
                       (float)g_retail_visual_fps / (float)target_fps);
}

static void *active_w3d_view(void) {
    if (g_game == NULL) return NULL;
    return *(void **)game_address(g_game, g_game->camera_input.w3d_view_pointer);
}

static int THISCALL scroll_by_at_retail_speed(void *view, const float *delta) {
    float scaled[2];

    if (g_original_scroll_by == NULL) return 0;
    if (delta == NULL) return g_original_scroll_by(view, NULL);
    /*
     * Arrow, RMB-drag, and edge scrolling all converge on W3DView::scrollBy.
     * Scaling here also makes the view's stored m_scrollAmount agree with the
     * actual world displacement used by terrain-height adjustment.
     */
    scaled[0] = delta[0] * g_camera_scroll_scale;
    scaled[1] = delta[1] * g_camera_scroll_scale;
    return g_original_scroll_by(view, scaled);
}

static int apply_held_zoom(const ZoomStep *step) {
    void *view = active_w3d_view();
    void **vtable;
    W3DViewGetZoomFn get_zoom;
    W3DViewSetZoomFn set_zoom;
    float zoom;

    if (view == NULL) return 0;
    vtable = *(void ***)view;
    get_zoom = (W3DViewGetZoomFn)vtable[W3D_VIEW_GET_ZOOM_SLOT];
    set_zoom = (W3DViewSetZoomFn)vtable[W3D_VIEW_SET_ZOOM_SLOT];
    zoom = get_zoom(view);
    return set_zoom(view, zoom * step->multiplier + step->offset);
}

static int THISCALL held_zoom_in_at_retail_speed(
    void *behavior, int context, char active, int scroll, int rotate) {
    (void)behavior;
    (void)context;
    (void)scroll;
    (void)rotate;
    return active ? apply_held_zoom(&g_zoom_in_step) : 0;
}

static int THISCALL held_zoom_out_at_retail_speed(
    void *behavior, int context, char active, int scroll, int rotate) {
    (void)behavior;
    (void)context;
    (void)scroll;
    (void)rotate;
    return active ? apply_held_zoom(&g_zoom_out_step) : 0;
}

/*
 * KW 1.02 GameEngine_DispatchLogicPhase (Steam VA 0x006560A6) batches the
 * six logic phases correctly only when clientFPS / 15 divides six. Return the
 * exclusive phase limit for every integral ratio from three through six.
 */
static u32 STDCALL get_phase_batch_limit(u32 phase, u32 ratio) {
    /* ceil(ratio * phase / 6) identifies this phase's client-frame slice. */
    u32 client_slot = (ratio * phase + LOGIC_PHASE_COUNT - 1u) / LOGIC_PHASE_COUNT;
    /* The stock loop increments EDI first, hence the exclusive +1 limit. */
    return (LOGIC_PHASE_COUNT * client_slot) / ratio + 1u;
}

/*
 * KW 1.02 GameEngine_UpdatePhaseInterpolation, Steam VA 0x0064333E.
 * Interpolation must advance once per client slice, not once per raw phase;
 * otherwise the 2-3 and 5-6 batches at 60 FPS expose the wrong visual sample.
 */
static void THISCALL update_phase_interpolation(void *engine_pointer) {
    u8 *engine = (u8 *)engine_pointer;
    u32 ratio = g_config->target_fps / LOGIC_FPS;
    u32 phase = *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE);
    u32 client_slot = (ratio * phase + LOGIC_PHASE_COUNT - 1u) / LOGIC_PHASE_COUNT;
    float interpolation;

    if (ratio == 3u || ratio == 6u) {
        /* Preserve bit-for-formula stock behavior for the already-valid ratios. */
        interpolation = (float)phase / (float)LOGIC_PHASE_COUNT;
    } else {
        interpolation = (float)client_slot / (float)ratio;
    }

    if (interpolation > 1.0f) interpolation = 1.0f;
    *(float *)(engine + ENGINE_LOGIC_PHASE_INTERPOLATION) = interpolation;
}

/*
 * KW 1.02 GameLogic_UpdatePhase, Steam VA 0x005EF10F, drains pending
 * Object-to-Drawable updates once after the final phase in each client slice.
 */
static BOOL STDCALL is_client_slice_end(u32 phase) {
    u32 ratio = g_config->target_fps / LOGIC_FPS;
    u32 client_slot = (ratio * phase + LOGIC_PHASE_COUNT - 1u) / LOGIC_PHASE_COUNT;
    /* floor(6 * slot / ratio) is the final phase assigned to that slice. */
    u32 end_phase = (LOGIC_PHASE_COUNT * client_slot) / ratio;
    return phase == end_phase;
}

/*
 * KW 1.02 W3DDisplay_RenderAndPresentFrame, Steam VAs 0x004A5A7C and
 * 0x004A5AB7. Retail advances 30 * 33 = 990 W3D milliseconds per nominal
 * second. The remainder also selects the 16/17 ms and 13/14 ms live-step
 * sequences used by the camera and other W3DView consumers at 60/75 FPS.
 */
static u32 STDCALL advance_w3d_time(u32 client_frame_delta) {
    const TimingLayout *timing = &g_game->timing;
    volatile u32 *accumulated = (volatile u32 *)game_address(
        g_game, timing->w3d_accumulated_time_ms);
    volatile u32 *live_step = (volatile u32 *)game_address(
        g_game, timing->w3d_milliseconds_per_frame);
    u32 current = *accumulated;
    u32 scaled;
    u32 elapsed;

    /* Fixed-point accumulator: numerator units are W3D-ms * targetFPS. */
    scaled = g_w3d_time_remainder +
             client_frame_delta * W3D_RETAIL_MILLISECONDS_PER_SECOND;
    elapsed = scaled / g_config->target_fps;
    g_w3d_time_remainder = scaled % g_config->target_fps;
    current += elapsed;
    *accumulated = current;
    /* Publish the next quotient so direct step readers share the same long-term rate. */
    *live_step = (g_w3d_time_remainder + W3D_RETAIL_MILLISECONDS_PER_SECOND) /
                 g_config->target_fps;
    return current;
}

/*
 * Map the high-rate client counter into the frame-authored 30 Hz domain.
 * The quotient/remainder form computes ceil(actual * 30 / target) without a
 * potentially overflowing full-width product. Repeated results are expected:
 * the game's existing duplicate-frame guards then skip surplus updates.
 */
static u32 read_client_frame(void *game_client) {
    void **vtable = *(void ***)game_client;
    GameClientGetFrameNumberFn get_frame_number =
        (GameClientGetFrameNumberFn)vtable[0x78u / sizeof(void *)];
    return get_frame_number(game_client);
}

static u32 to_retail_visual_frame(u32 actual_frame) {
    u32 target_fps = g_config->target_fps;
    return (actual_frame / target_fps) * g_retail_visual_fps +
           ((actual_frame % target_fps) * g_retail_visual_fps + target_fps - 1u) /
               target_fps;
}

static u32 THISCALL get_retail_visual_frame(void *game_client) {
    if (game_client == NULL || g_config->target_fps < g_retail_visual_fps) return 0;
    return to_retail_visual_frame(read_client_frame(game_client));
}

/*
 * Drawable::calcPhysicsXform integrates locomotor springs (pitch, roll, yaw
 * wobble, suspension height) once per GameClient frame without a time step,
 * so at high FPS vehicles bounced and tilted several times too hard. Its frame
 * guard now reads the synthetic 30 Hz frame, restoring retail spring rates.
 *
 * Its caller only applies a tilt when the dispatcher reports a fresh result,
 * so between 30 Hz updates this wrapper supplies a blend of the last two
 * results instead. The blend lags the springs by one retail frame (33 ms) but
 * avoids both 30 Hz stepping and frames without any tilt.
 */
typedef u8(THISCALL *CalcPhysicsXformFn)(void *drawable, float *info);

enum {
    PHYSICS_XFORM_FLOATS = 4, /* pitch, roll, yaw, z offset */
    PHYSICS_CACHE_SIZE = 8192, /* power of two */
    PHYSICS_CACHE_PROBES = 16
};

typedef struct PhysicsXformCacheEntry {
    void *drawable;
    u32 retail_frame;
    float previous[PHYSICS_XFORM_FLOATS];
    float current[PHYSICS_XFORM_FLOATS];
} PhysicsXformCacheEntry;

static CalcPhysicsXformFn g_original_calc_physics_xform;
static PhysicsXformCacheEntry g_physics_cache[PHYSICS_CACHE_SIZE];

static PhysicsXformCacheEntry *find_physics_entry(void *drawable, u32 retail_frame) {
    u32 hash = (u32)(((uintptr_t)drawable >> 3) * 2654435761u);
    PhysicsXformCacheEntry *reusable = NULL;
    u32 probe;

    for (probe = 0; probe < PHYSICS_CACHE_PROBES; ++probe) {
        PhysicsXformCacheEntry *entry =
            &g_physics_cache[(hash + probe) & (PHYSICS_CACHE_SIZE - 1u)];
        if (entry->drawable == drawable) return entry;
        /* Entries not refreshed for two retail frames belong to dead drawables. */
        if (reusable == NULL &&
            (entry->drawable == NULL || retail_frame - entry->retail_frame > 2u)) {
            reusable = entry;
        }
    }
    if (reusable != NULL) {
        reusable->drawable = drawable;
        reusable->retail_frame = retail_frame - 2u;
    }
    return reusable;
}

/*
 * Diagnostic (debug_motion_frames > 0): follow the first moving vehicle of a
 * session and log one line per client frame describing how its drawn
 * position is interpolated between logic ticks. Offsets are TW 1.10 Drawable
 * and Object fields (see docs/goals_detailed.md, B4).
 */
enum {
    DRAWABLE_HISTORY_FRAME = 0x138,
    DRAWABLE_FLAGS = 0x144,
    DRAWABLE_HISTORY_PREVIOUS = 0xD0,
    DRAWABLE_HISTORY_CURRENT = 0xDC,
    DRAWABLE_INTERPOLATED_POSITION = 0x124,
    DRAWABLE_OBJECT = 0x140,
    OBJECT_POSITION = 0x38,
    OBJECT_PREVIOUS_POSITION = 0x1A8,
    OBJECT_PREVIOUS_POSITION_VALID = 0x1C1
};

static void *g_motion_drawable;
static u32 g_motion_frames_logged;
static u32 g_motion_last_frame;
static LARGE_INTEGER g_motion_start;
static LARGE_INTEGER g_motion_frequency;

static void log_motion_sample(void *drawable, u32 actual_frame) {
    u8 *d = (u8 *)drawable;
    u8 *object;
    u8 *engine;
    const float *p1 = (const float *)(d + DRAWABLE_HISTORY_PREVIOUS);
    const float *p2 = (const float *)(d + DRAWABLE_HISTORY_CURRENT);
    const float *out = (const float *)(d + DRAWABLE_INTERPOLATED_POSITION);
    const float *pos;
    const float *prev;
    LARGE_INTEGER now;
    double us;

    if (g_config->debug_motion_frames == 0 ||
        g_motion_frames_logged >= g_config->debug_motion_frames) {
        return;
    }
    object = *(u8 **)(d + DRAWABLE_OBJECT);
    if (object == NULL) return;
    if (g_motion_drawable == NULL) {
        /* Lock onto the first drawable whose logic history shows movement. */
        if (p1[0] == p2[0] && p1[1] == p2[1]) return;
        g_motion_drawable = drawable;
        QueryPerformanceFrequency(&g_motion_frequency);
        QueryPerformanceCounter(&g_motion_start);
        log_line("motion log: frame us phase s hist dflags | obj.x obj.y prev.x prev.y"
                 " prevok | p1.x p1.y p2.x p2.y | out.x out.y");
    }
    if (drawable != g_motion_drawable || actual_frame == g_motion_last_frame) return;
    g_motion_last_frame = actual_frame;
    engine = *(u8 **)game_address(g_game, g_game->timing.game_engine_pointer);
    pos = (const float *)(object + OBJECT_POSITION);
    prev = (const float *)(object + OBJECT_PREVIOUS_POSITION);
    QueryPerformanceCounter(&now);
    us = (double)(now.QuadPart - g_motion_start.QuadPart) * 1000000.0 /
         (double)g_motion_frequency.QuadPart;
    log_printf("M %u %.0f %u %.4f %u 0x%02X | %.3f %.3f %.3f %.3f %u | "
               "%.3f %.3f %.3f %.3f | %.3f %.3f",
               actual_frame, us,
               engine ? *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE) : 0u,
               engine ? (double)*(float *)(engine + ENGINE_LOGIC_PHASE_INTERPOLATION) : -1.0,
               *(u32 *)(d + DRAWABLE_HISTORY_FRAME), d[DRAWABLE_FLAGS],
               (double)pos[0], (double)pos[1], (double)prev[0], (double)prev[1],
               object[OBJECT_PREVIOUS_POSITION_VALID],
               (double)p1[0], (double)p1[1], (double)p2[0], (double)p2[1],
               (double)out[0], (double)out[1]);
    if (++g_motion_frames_logged == g_config->debug_motion_frames) {
        log_line("motion log: complete");
    }
}

static u8 THISCALL calc_physics_xform_interpolated(void *drawable, float *info) {
    void *game_client = *(void **)game_address(
        g_game, g_game->drawable_physics.game_client_pointer);
    PhysicsXformCacheEntry *entry;
    u32 target_fps = g_config->target_fps;
    u32 actual_frame;
    u32 retail_frame;
    u32 remainder;
    float alpha;
    u32 i;

    if (game_client == NULL) return g_original_calc_physics_xform(drawable, info);
    actual_frame = read_client_frame(game_client);
    log_motion_sample(drawable, actual_frame);
    retail_frame = to_retail_visual_frame(actual_frame);
    entry = find_physics_entry(drawable, retail_frame);
    if (entry == NULL) return g_original_calc_physics_xform(drawable, info);

    if (g_original_calc_physics_xform(drawable, info)) {
        /* A new 30 Hz result; keep the last one for blending if it is adjacent. */
        if (entry->retail_frame + 1u == retail_frame) {
            memcpy(entry->previous, entry->current, sizeof(entry->previous));
        } else {
            memcpy(entry->previous, info, sizeof(entry->previous));
        }
        memcpy(entry->current, info, sizeof(entry->current));
        entry->retail_frame = retail_frame;
    } else if (entry->retail_frame != retail_frame) {
        /* No locomotor physics for this drawable (or not yet computed). */
        return 0;
    }

    /*
     * Retail time t = actual * 30 / target lies in (retail_frame - 1, retail_frame].
     * Blend previous -> current by t's position in that interval.
     */
    remainder = (u32)(((u64)actual_frame * g_retail_visual_fps) % target_fps);
    alpha = remainder == 0u ? 1.0f : (float)remainder / (float)target_fps;
    for (i = 0; i < PHYSICS_XFORM_FLOATS; ++i) {
        info[i] = entry->previous[i] + (entry->current[i] - entry->previous[i]) * alpha;
    }
    return 1;
}

static void THISCALL update_particles_at_retail_rate(void *manager) {
    SubsystemUpdateFn update;
    u32 target_fps = g_config->target_fps;

    if (manager == NULL) return;
    update = (SubsystemUpdateFn)game_address(
        g_game, g_game->visual.particle_simulation.target);
    if (target_fps <= g_retail_visual_fps) {
        update(manager);
        return;
    }
    /* Bresenham-style rate conversion: exactly 30 calls per targetFPS frames. */
    g_particle_update_accumulator += g_retail_visual_fps;
    if (g_particle_update_accumulator >= target_fps) {
        g_particle_update_accumulator -= target_fps;
        ++g_particle_sim_tick;
        update(manager);
    }
}

/*
 * CPU particles (DefaultDrawModule) are simulated at the retail 30 Hz, so
 * drawn as-is they move in 30 Hz steps. While a system is drawn, each
 * particle's position is temporarily replaced by a blend of its positions
 * from the previous and the latest 30 Hz step, then restored, so simulation
 * never sees the blended values. The blend lags one step (33 ms), like
 * Drawable interpolation.
 *
 * Particles live in pools, so an address can be reused by a new particle.
 * The remaining-life counter (decremented once per step) tells them apart.
 */
typedef int(THISCALL *ParticleDrawFn)(void *draw_module, void *a, void *b, void *c);
typedef u32(THISCALL *ParticleStorageCountFn)(void *storage);
typedef u8 *(THISCALL *ParticleStorageGetFn)(void *storage, u32 index);

enum {
    PARTICLE_CACHE_SIZE = 16384, /* power of two */
    PARTICLE_CACHE_PROBES = 16,
    PARTICLE_DRAW_MAX = 4096
};

typedef struct ParticleCacheEntry {
    u8 *particle;
    u32 tick;
    u32 life;
    float previous[3];
    float current[3];
} ParticleCacheEntry;

static ParticleDrawFn g_original_particle_draw;
static ParticleCacheEntry g_particle_cache[PARTICLE_CACHE_SIZE];
static float *g_particle_restore_position[PARTICLE_DRAW_MAX];
static float g_particle_restore_value[PARTICLE_DRAW_MAX][3];

static ParticleCacheEntry *find_particle_entry(u8 *particle, BOOL *is_new) {
    u32 hash = (u32)(((uintptr_t)particle >> 3) * 2654435761u);
    ParticleCacheEntry *reusable = NULL;
    u32 probe;

    for (probe = 0; probe < PARTICLE_CACHE_PROBES; ++probe) {
        ParticleCacheEntry *entry =
            &g_particle_cache[(hash + probe) & (PARTICLE_CACHE_SIZE - 1u)];
        if (entry->particle == particle) {
            *is_new = FALSE;
            return entry;
        }
        /* Entries not refreshed for two steps belong to dead or culled particles. */
        if (reusable == NULL &&
            (entry->particle == NULL || g_particle_sim_tick - entry->tick > 2u)) {
            reusable = entry;
        }
    }
    if (reusable != NULL) reusable->particle = particle;
    *is_new = TRUE;
    return reusable;
}

static void track_particle(ParticleCacheEntry *entry, BOOL is_new,
                           const float *position, u32 life) {
    u32 tick = g_particle_sim_tick;
    /* Same particle: life unchanged within a step, or one less per step. */
    BOOL continues = !is_new && (entry->life == life || entry->life == life + 1u);

    if (!continues) {
        memcpy(entry->previous, position, sizeof(entry->previous));
    } else if (entry->tick != tick) {
        if (entry->tick + 1u == tick) {
            memcpy(entry->previous, entry->current, sizeof(entry->previous));
        } else {
            memcpy(entry->previous, position, sizeof(entry->previous));
        }
    }
    memcpy(entry->current, position, sizeof(entry->current));
    entry->tick = tick;
    entry->life = life;
}

static int THISCALL draw_particles_interpolated(void *draw_module, void *a, void *b, void *c) {
    u8 *system = *(u8 **)((u8 *)draw_module + PARTICLE_DRAW_MODULE_SYSTEM);
    void *storage;
    void **vtable;
    ParticleStorageGetFn get_particle;
    float alpha;
    u32 count;
    u32 saved = 0;
    u32 i;
    int result;

    if (system == NULL || g_config->target_fps <= g_retail_visual_fps) {
        return g_original_particle_draw(draw_module, a, b, c);
    }
    storage = *(void **)(system + PARTICLE_SYSTEM_STORAGE);
    if (storage == NULL) return g_original_particle_draw(draw_module, a, b, c);

    vtable = *(void ***)storage;
    count = ((ParticleStorageCountFn)vtable[PARTICLE_STORAGE_COUNT_SLOT])(storage);
    get_particle = (ParticleStorageGetFn)vtable[PARTICLE_STORAGE_GET_SLOT];
    if (count > PARTICLE_DRAW_MAX) count = PARTICLE_DRAW_MAX;
    /* Fraction of the current 30 Hz step that has elapsed. */
    alpha = (float)g_particle_update_accumulator / (float)g_config->target_fps;

    for (i = 0; i < count; ++i) {
        u8 *particle = get_particle(storage, i);
        ParticleCacheEntry *entry;
        float *position;
        BOOL is_new;
        u32 k;

        if (particle == NULL) continue;
        entry = find_particle_entry(particle, &is_new);
        if (entry == NULL) continue;
        position = (float *)(particle + PARTICLE_POSITION);
        track_particle(entry, is_new, position,
                       *(u32 *)(particle + PARTICLE_LIFE_REMAINING));

        g_particle_restore_position[saved] = position;
        memcpy(g_particle_restore_value[saved], position, sizeof(g_particle_restore_value[saved]));
        ++saved;
        for (k = 0; k < 3; ++k) {
            position[k] = entry->previous[k] +
                          (entry->current[k] - entry->previous[k]) * alpha;
        }
    }

    result = g_original_particle_draw(draw_module, a, b, c);

    for (i = 0; i < saved; ++i) {
        memcpy(g_particle_restore_position[i], g_particle_restore_value[i],
               sizeof(g_particle_restore_value[i]));
    }
    return result;
}

/*
 * Preserve the stock limiter's observed 33 ms retail client frames while
 * subdividing them across the selected high-rate schedule. The original
 * limiter first truncates one 30 Hz interval after applying network scale;
 * two of those intervals are one complete 15 Hz logic-cycle budget. A
 * remainder accumulator distributes that integer budget without replacing
 * the game's timeGetTime/Sleep(0), no-limit, telemetry, or history paths.
 */
/*
 * Diagnostic (debug_frame_times > 0): one line per client frame from inside
 * the stock limiter: real time since the previous frame (QPC, µs), the
 * limiter's planned interval, the logic phase, timeGetTime (its step size
 * shows the effective timer resolution), and the adaptive multiplier.
 */
static u32 g_frame_times_logged;
static LARGE_INTEGER g_frame_time_previous;
static LARGE_INTEGER g_frame_time_frequency;

static void log_frame_time_sample(u8 *engine, u32 interval, u32 multiplier) {
    LARGE_INTEGER now;
    double delta_us = 0.0;

    if (g_config == NULL || g_config->debug_frame_times == 0 ||
        g_frame_times_logged >= g_config->debug_frame_times) {
        return;
    }
    QueryPerformanceCounter(&now);
    if (g_frame_times_logged == 0) {
        QueryPerformanceFrequency(&g_frame_time_frequency);
        log_line("frame log: dt_us planned_ms phase timeGetTime multiplier");
    } else {
        delta_us = (double)(now.QuadPart - g_frame_time_previous.QuadPart) *
                   1000000.0 / (double)g_frame_time_frequency.QuadPart;
    }
    g_frame_time_previous = now;
    log_printf("F %.0f %u %u %u %u", delta_us, interval,
               *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE),
               (u32)timeGetTime(), multiplier);
    if (++g_frame_times_logged == g_config->debug_frame_times) {
        log_line("frame log: complete");
    }
}

static u32 STDCALL get_retail_limiter_interval(void *engine_pointer) {
    const PacingLayout *pacing;
    i32 signed_multiplier;
    u32 multiplier;
    u32 retail_frame_ms;
    u32 logic_cycle_budget_ms;
    u32 scaled;
    u32 interval;
    float scale;
    float logic_ms;

    if (engine_pointer == NULL || g_game == NULL) return 1u;
    pacing = &g_game->pacing;
    signed_multiplier = *(volatile i32 *)(
        (u8 *)engine_pointer + ENGINE_PACING_UPDATE_MULTIPLIER);
    if (signed_multiplier < 1) signed_multiplier = 1;
    multiplier = (u32)signed_multiplier;

    scale = *(volatile float *)game_address(g_game, pacing->network_scale);
    if (!(scale >= 0.5f)) scale = 0.5f;
    if (scale > 1.0f) scale = 1.0f;
    logic_ms = *(volatile float *)game_address(
        g_game, pacing->milliseconds_per_logic_frame);
    if (!(logic_ms > 0.0f)) return 1u;

    retail_frame_ms = (u32)(
        logic_ms /
        ((float)RETAIL_CLIENT_FRAMES_PER_LOGIC_TICK * scale));
    if (retail_frame_ms == 0u) return 1u;
    logic_cycle_budget_ms =
        retail_frame_ms * RETAIL_CLIENT_FRAMES_PER_LOGIC_TICK;

    if (g_limiter_interval_divisor != multiplier) {
        if (g_limiter_interval_divisor == 0u) {
            g_limiter_interval_remainder = 0u;
        } else {
            /* Retain the same fractional phase when adaptive pacing changes. */
            g_limiter_interval_remainder =
                (g_limiter_interval_remainder * multiplier +
                 g_limiter_interval_divisor / 2u) /
                g_limiter_interval_divisor;
        }
        g_limiter_interval_divisor = multiplier;
    }

    scaled = g_limiter_interval_remainder + logic_cycle_budget_ms;
    interval = scaled / multiplier;
    g_limiter_interval_remainder = scaled % multiplier;
    if (interval == 0u) interval = 1u;
    log_frame_time_sample((u8 *)engine_pointer, interval, multiplier);
    return interval;
}

/*
 * Audio request admission, Limit accounting, playback completion, and queue
 * cleanup all run inside the manager's client update. Retain their retail
 * 30 Hz interleaving while the rest of the client advances at the configured
 * rate. ComputeClientFrameDeltaMilliseconds observes the skipped client-frame
 * span, so authored millisecond delays still advance by the correct amount.
 */
static u8 THISCALL update_audio_at_retail_rate(void *audio_manager) {
    u32 target_fps;

    if (g_original_audio_update == NULL) return 0;
    if (g_config == NULL || g_config->target_fps <= g_retail_visual_fps) {
        return g_original_audio_update(audio_manager);
    }
    target_fps = g_config->target_fps;
    g_audio_update_accumulator += g_retail_visual_fps;
    if (g_audio_update_accumulator < target_fps) return 0;
    g_audio_update_accumulator -= target_fps;
    return g_original_audio_update(audio_manager);
}

/*
 * Objective markers are APT clips created when their target enters (or
 * leaves) the screen. Their screen position is pushed from the clip's
 * OnInitialized callback, which only runs on the next APT movie frame. APT
 * frames advance on a real-time ~33 ms accumulator, so above 30 FPS a new
 * clip was drawn unpositioned for one or more client frames. Requesting the
 * engine's own forced APT tick makes the callback run before that draw.
 */
static void request_apt_tick(void) {
    u8 *player;

    if (g_game == NULL) return;
    player = *(u8 **)game_address(g_game, g_game->objective_ui.apt_player_pointer);
    if (player != NULL) player[APT_PLAYER_FORCE_TICK] = 1;
}

static void **add_indicator_with_apt_tick(u32 kind, void *stage,
                                          void **out_indicator, void *target) {
    void **result = g_original_add_indicator[kind](stage, out_indicator, target);
    request_apt_tick();
    return result;
}

static void **THISCALL add_object_indicator(void *stage, void **out, void *target) {
    return add_indicator_with_apt_tick(0, stage, out, target);
}

static void **THISCALL add_offscreen_indicator(void *stage, void **out, void *target) {
    return add_indicator_with_apt_tick(1, stage, out, target);
}

static void **THISCALL add_waypoint_indicator(void *stage, void **out, void *target) {
    return add_indicator_with_apt_tick(2, stage, out, target);
}

/*
 * The selection-details panel forwards a unit name to APT only when it
 * differs from the previous one. Deselecting and quickly reselecting the
 * same object makes the panel's Flash clip reset its name field, and at high
 * FPS several client frames of panel updates reach APT in one ~30 Hz tick.
 * The unchanged name was then never re-sent and the placeholder "Unit Name"
 * stayed. Re-send it every time the panel is told its name.
 */
typedef void(THISCALL *SetUnitNameFn)(void *panel, void *name);
typedef void(THISCALL *PushUnitNameFn)(void *panel);
static SetUnitNameFn g_original_set_unit_name;
static PushUnitNameFn g_push_unit_name;

static void THISCALL set_unit_name_always_push(void *panel, void *name) {
    g_original_set_unit_name(panel, name);
    if (((u8 *)panel)[APT_GADGET_INITIALIZED] != 0) g_push_unit_name(panel);
}

/*
 * Production buildings rebuild their exit/production buttons on a later
 * frame than the name push, and that Flash layout pass can reset the name
 * field again. While a single object is selected, re-send the name every
 * client frame (the stock update already re-sends rank and ammo each frame),
 * so any reset is corrected on the next APT tick.
 */
typedef void(THISCALL *SingleUnitUpdateFn)(void *handler);
static SingleUnitUpdateFn g_original_single_unit_update;

/*
 * Selecting an object runs in two C++ steps on consecutive client frames:
 * the panel state/name/production setup, then the production/exit button
 * rebuild. Deselecting tears the production UI down the same way. At 30 FPS
 * one APT (Flash) frame always ran between such steps; at high FPS several
 * steps reached the panel within one APT frame, and switching from one
 * production building to another left the name field on its "Unit Name"
 * placeholder. Give each step its own APT frame, as at 30 FPS.
 */
typedef void(THISCALL *SetPanelStateFn)(void *panel, int state);
static SetPanelStateFn g_original_set_panel_state;

static void THISCALL set_panel_state_with_apt_tick(void *panel, int state) {
    int previous = *(int *)((u8 *)panel + SELECTION_PANEL_STATE);
    g_original_set_panel_state(panel, state);
    if (previous != state) request_apt_tick();
}

static void THISCALL single_unit_update_with_name(void *handler) {
    u8 *panel_impl;
    u8 *gadget;

    if (((u8 *)handler)[SELECTION_HANDLER_REBUILD_BUTTONS] != 0) request_apt_tick();
    g_original_single_unit_update(handler);
    panel_impl = *(u8 **)((u8 *)handler + SELECTION_HANDLER_PANEL);
    if (panel_impl == NULL) return;
    gadget = *(u8 **)(panel_impl + SELECTION_PANEL_GADGET);
    if (gadget != NULL && gadget[APT_GADGET_INITIALIZED] != 0) g_push_unit_name(gadget);
}

static void reset_keyboard_clock(void) {
    g_keyboard_last_poll_ms = timeGetTime();
    /* Seed one immediate tick, matching stock's first post-reset increment. */
    g_keyboard_time_remainder = 1000u;
    g_keyboard_repeat_tick = FALSE;
    g_keyboard_clock_initialized = TRUE;
}

static BOOL keyboard_retail_tick_due(void) {
    u32 now;
    u32 elapsed;
    u32 scaled;

    if (!g_keyboard_clock_initialized) reset_keyboard_clock();
    now = timeGetTime();
    /* Unsigned subtraction intentionally handles timeGetTime's wraparound. */
    elapsed = now - g_keyboard_last_poll_ms;
    g_keyboard_last_poll_ms = now;
    if (elapsed >= 1000u) {
        /* A long stall still produces only one stock repeat on resumption. */
        g_keyboard_time_remainder = 0;
        return TRUE;
    }
    scaled = g_keyboard_time_remainder + elapsed * g_retail_visual_fps;
    if (scaled < 1000u) {
        g_keyboard_time_remainder = (u32)scaled;
        return FALSE;
    }
    /* Never burst missed repeats after a stall; stock emits at most one per poll. */
    g_keyboard_time_remainder = (u32)(scaled % 1000u);
    return TRUE;
}

static void defer_physical_key_timestamps(void *keyboard) {
    u8 *event;
    u8 *end;
    u32 next_frame;

    if (keyboard == NULL) return;
    event = *(u8 **)((u8 *)keyboard + KEYBOARD_EVENT_BEGIN);
    end = *(u8 **)((u8 *)keyboard + KEYBOARD_EVENT_END);
    next_frame = *(u32 *)((u8 *)keyboard + KEYBOARD_INPUT_FRAME) + 1u;
    while (event != end) {
        u32 key = event[0];
        *(u32 *)((u8 *)keyboard + KEYBOARD_KEY_SEQUENCE_BASE +
                 key * KEYBOARD_KEY_STATE_STRIDE) = next_frame;
        event += KEYBOARD_KEY_STATE_STRIDE;
    }
}

/*
 * Keyboard::inputFrame is private to hardware-event timestamps and the stock
 * autorepeat routine. Advance it on a wall-clock 30 Hz gate shared by normal,
 * extra, and loading-screen polls, but retain every physical hardware poll.
 */
static void THISCALL poll_keyboard_at_retail_rate(void *keyboard) {
    g_keyboard_repeat_tick = keyboard_retail_tick_due();
    if (keyboard != NULL && g_keyboard_repeat_tick) {
        ++*(u32 *)((u8 *)keyboard + KEYBOARD_INPUT_FRAME);
    }
    if (g_original_keyboard_poll != NULL) {
        g_original_keyboard_poll(keyboard);
    }
}

static u8 THISCALL check_keyboard_repeat_at_retail_rate(void *keyboard) {
    BOOL repeat_tick = g_keyboard_repeat_tick;
    g_keyboard_repeat_tick = FALSE;
    if (!repeat_tick) {
        /* Retail would first observe these events on the next 30 Hz tick. */
        defer_physical_key_timestamps(keyboard);
        return 0;
    }
    if (g_original_keyboard_check_repeat == NULL) return 0;
    return g_original_keyboard_check_repeat(keyboard);
}

static void log_mismatch(const char *name, u32 rva,
                         const void *expected, size_t size) {
    log_text("ERROR: runtime code mismatch at ");
    log_line(name);
    log_hex32("  RVA=", rva);
    if (size >= sizeof(u32)) {
        log_hex32("  actual first four bytes=", load_u32(game_address(g_game, rva)));
        log_hex32("  expected first four bytes=", load_u32(expected));
    }
}

/*
 * Check and write are intentionally one operation: the bytes resolved during
 * DllMain are checked again immediately before mutation, catching another
 * injector or an unsupported near-match without a separate verifier path.
 */
static void patch_bytes(PatchInstaller *installer, const char *name, u32 rva,
                        const void *expected, const void *replacement, size_t size) {
    void *destination = game_address(g_game, rva);
    if (!installer->ok) return;
    if (memcmp(destination, expected, size) != 0) {
        log_mismatch(name, rva, expected, size);
        installer->ok = FALSE;
        return;
    }
    if (!patch_transaction_write(
            &installer->transaction, destination, replacement, size)) {
        log_text("ERROR: could not write runtime patch at ");
        log_line(name);
        log_u32("  writes already recorded=", (u32)installer->transaction.count);
        installer->ok = FALSE;
    }
}

static void expect_bytes(PatchInstaller *installer, const char *name, u32 rva,
                         const void *expected, size_t size) {
    if (!installer->ok) return;
    if (memcmp(game_address(g_game, rva), expected, size) != 0) {
        log_mismatch(name, rva, expected, size);
        installer->ok = FALSE;
    }
}

static void patch_pointer(PatchInstaller *installer, const char *name, u32 rva,
                          const void *expected_target, const void *replacement_target) {
    /* x86 absolute operands contain relocated virtual addresses, not RVAs. */
    u32 expected = (u32)(uintptr_t)expected_target;
    u32 replacement = (u32)(uintptr_t)replacement_target;
    patch_bytes(installer, name, rva, &expected, &replacement, sizeof(replacement));
}

static void patch_branch(PatchInstaller *installer, const char *name,
                         const BranchSite *site, u8 opcode,
                         const void *replacement, size_t replacement_size) {
    i32 displacement;
    i32 actual_target;
    u8 *instruction = game_address(g_game, site->instruction);

    if (!installer->ok) return;
    /* Validate both the opcode and decoded original destination. */
    if (instruction[0] == opcode) {
        memcpy(&displacement, instruction + 1u, sizeof(displacement));
        actual_target = (i32)(site->instruction + 5u) + displacement;
        if (actual_target >= 0 && (u32)actual_target == site->target) {
            if (!patch_transaction_write(
                    &installer->transaction, instruction, replacement, replacement_size)) {
                log_text("ERROR: could not write runtime patch at ");
                log_line(name);
                installer->ok = FALSE;
            }
            return;
        }
    }
    log_text("ERROR: runtime branch mismatch at ");
    log_line(name);
    log_hex32("  RVA=", site->instruction);
    installer->ok = FALSE;
}

enum { NO_INTERLEAVED_PUSH = 0, PUSH_EDI = 0x57, PUSH_EBX = 0x53 };

static void redirect_frame_read(PatchInstaller *installer, const char *name,
                                u32 rva, u8 interleaved_push) {
    static const u8 virtual_call[] = { 0x8B, 0x01, 0xFF, 0x50, 0x78 };
    u8 virtual_call_with_push[] = { 0x8B, 0x01, 0, 0xFF, 0x50, 0x78 };
    u8 replacement[6] = { 0xE8, 0, 0, 0, 0, 0 };
    const u8 *expected = virtual_call;
    size_t size = sizeof(virtual_call);

    if (interleaved_push != NO_INTERLEAVED_PUSH) {
        /* Some sites push a register between the vtable load and the call. */
        virtual_call_with_push[2] = interleaved_push;
        expected = virtual_call_with_push;
        size = sizeof(virtual_call_with_push);
        replacement[0] = interleaved_push;
        replacement[1] = 0xE8;
        encode_rel32(&replacement[2], game_address(g_game, rva + 6u),
                     get_retail_visual_frame);
    } else {
        /* ECX already contains GameClient* and the helper returns its value in EAX. */
        encode_rel32(&replacement[1], game_address(g_game, rva + 5u),
                     get_retail_visual_frame);
    }
    patch_bytes(installer, name, rva, expected, replacement, size);
}

static void log_incoming_timing(void) {
    const TimingLayout *timing = &g_game->timing;
    log_u32("Incoming client FPS=",
            load_u32(game_address(g_game, timing->client_fps)));
    log_u32("Incoming W3D milliseconds/client-frame=",
            load_u32(game_address(g_game, timing->w3d_milliseconds_per_frame)));
    log_hex32("Incoming client-FPS float bits=",
              load_u32(game_address(g_game, timing->client_fps_float)));
    log_hex32("Incoming audio milliseconds/client-frame bits=",
              load_u32(game_address(g_game, timing->audio_milliseconds_per_frame)));
    log_hex32("Incoming visual seconds/client-frame bits=",
              load_u32(game_address(g_game, timing->visual_seconds_per_frame)));
}

static void install_continuous_visuals(PatchInstaller *installer) {
    const VisualLayout *visual = &g_game->visual;
    void *retail_step = game_address(g_game, visual->retail_step);

    /* These systems integrate motion continuously and therefore need 1/targetFPS. */
    patch_pointer(installer, "camera visual-step operand",
                  visual->camera_step_operand, retail_step, &g_visual_client_step);
    patch_pointer(installer, "laser visual-step operand",
                  visual->laser_step_operand, retail_step, &g_visual_client_step);
    patch_pointer(installer, "scripted-model visual-step operand",
                  visual->model_step_operand, retail_step, &g_visual_client_step);
}

static void install_keyboard_timing(PatchInstaller *installer) {
    static const u8 frame_increment[] = { 0xFF, 0x86, 0x28, 0x0E, 0x00, 0x00 };
    static const u8 frame_increment_patch[] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    const KeyboardLayout *keyboard = &g_game->keyboard;
    u8 poll_patch[5] = { 0xE8, 0, 0, 0, 0 };
    u8 repeat_patch[5] = { 0xE8, 0, 0, 0, 0 };

    g_original_keyboard_poll = (KeyboardPollFn)game_address(
        g_game, keyboard->hardware_poll.target);
    g_original_keyboard_check_repeat = (KeyboardCheckRepeatFn)game_address(
        g_game, keyboard->check_repeat.target);

    encode_rel32(&poll_patch[1],
                 game_address(g_game, keyboard->hardware_poll.instruction + 5u),
                 poll_keyboard_at_retail_rate);
    encode_rel32(&repeat_patch[1],
                 game_address(g_game, keyboard->check_repeat.instruction + 5u),
                 check_keyboard_repeat_at_retail_rate);

    patch_bytes(installer, "keyboard private-frame increment",
                keyboard->input_frame_increment,
                frame_increment, frame_increment_patch,
                sizeof(frame_increment));
    patch_branch(installer, "keyboard hardware-poll call",
                 &keyboard->hardware_poll, 0xE8,
                 poll_patch, sizeof(poll_patch));
    patch_branch(installer, "keyboard autorepeat call",
                 &keyboard->check_repeat, 0xE8,
                 repeat_patch, sizeof(repeat_patch));
}

static void install_audio_timing(PatchInstaller *installer) {
    const AudioLayout *audio = &g_game->audio;
    void *update_function = game_address(g_game, audio->update_function);

    g_original_audio_update = (AudioUpdateFn)update_function;
    patch_pointer(installer, "SageAudioManager update vtable entry",
                  audio->update_vtable_entry,
                  update_function, update_audio_at_retail_rate);
}

static void install_selection_panel_name(PatchInstaller *installer) {
    const SelectionPanelLayout *panel = &g_game->selection_panel;
    void *original = game_address(g_game, panel->set_unit_name_function);

    g_original_set_unit_name = (SetUnitNameFn)original;
    g_push_unit_name = (PushUnitNameFn)game_address(
        g_game, panel->push_unit_name_function);
    patch_pointer(installer, "SetUnitName vtable entry",
                  panel->set_unit_name_vtable_entry, original,
                  (const void *)set_unit_name_always_push);

    original = game_address(g_game, panel->set_state_function);
    g_original_set_panel_state = (SetPanelStateFn)original;
    patch_pointer(installer, "selection panel SetState vtable entry",
                  panel->set_state_vtable_entry, original,
                  (const void *)set_panel_state_with_apt_tick);

    original = game_address(g_game, panel->single_unit_update_function);
    g_original_single_unit_update = (SingleUnitUpdateFn)original;
    patch_pointer(installer, "SingleUnitModeHandler update vtable entry",
                  panel->single_unit_update_vtable_entry, original,
                  (const void *)single_unit_update_with_name);
}

static void install_objective_indicator_tick(PatchInstaller *installer) {
    static const char *const names[ADD_INDICATOR_COUNT] = {
        "AddObjectIndicator vtable entry",
        "AddOffScreenIndicator vtable entry",
        "AddWaypointIndicator vtable entry"
    };
    const AddIndicatorFn wrappers[ADD_INDICATOR_COUNT] = {
        add_object_indicator, add_offscreen_indicator, add_waypoint_indicator
    };
    const ObjectiveUiLayout *ui = &g_game->objective_ui;
    u32 i;

    for (i = 0; i < ADD_INDICATOR_COUNT; ++i) {
        void *original = game_address(g_game, ui->add_indicator_functions[i]);
        g_original_add_indicator[i] = (AddIndicatorFn)original;
        patch_pointer(installer, names[i], ui->add_indicator_vtable_entries[i],
                      original, (const void *)wrappers[i]);
    }
}

static void patch_entry_jump(PatchInstaller *installer, const char *name,
                             u32 function_rva, const void *target) {
    static const u8 expected[] = { 0x80, 0x7C, 0x24, 0x08, 0x00 };
    u8 replacement[sizeof(expected)] = { 0xE9, 0, 0, 0, 0 };

    encode_rel32(&replacement[1], game_address(g_game, function_rva + 5u), target);
    patch_bytes(installer, name, function_rva,
                expected, replacement, sizeof(replacement));
}

static void install_camera_timing(PatchInstaller *installer) {
    static const u8 camera_adjust_instruction[] = {
        0xD8, 0x8F, 0x14, 0x0B, 0x00, 0x00
    };
    static const u8 shake_instruction[] = { 0xF3, 0x0F, 0x59, 0x05 };
    const CameraInputLayout *camera = &g_game->camera_input;
    void *view;
    void **vtable;
    void **scroll_entry;
    uintptr_t module_base;
    uintptr_t entry_address;
    u32 entry_rva;
    void *scroll_function;

    if (!installer->ok) return;

    /*
     * Runtime configuration is reached after W3DView construction. Resolve
     * its actual derived-class vtable rather than assuming a fixed .rdata RVA,
     * then prove that slot 0x14 still points at the signature-found scrollBy.
     */
    view = active_w3d_view();
    if (view == NULL) {
        log_line("ERROR: live W3DView pointer is null");
        installer->ok = FALSE;
        return;
    }
    vtable = *(void ***)view;
    scroll_entry = &vtable[W3D_VIEW_SCROLL_BY_SLOT];
    module_base = (uintptr_t)g_game->module;
    entry_address = (uintptr_t)scroll_entry;
    if (entry_address < module_base ||
        entry_address - module_base > g_game->pe_size_of_image - sizeof(void *)) {
        log_line("ERROR: W3DView scrollBy vtable entry is outside the game image");
        installer->ok = FALSE;
        return;
    }
    entry_rva = (u32)(entry_address - module_base);
    scroll_function = game_address(g_game, camera->scroll_by_function);
    g_original_scroll_by = (W3DViewScrollByFn)scroll_function;
    patch_pointer(installer, "W3DView scrollBy vtable entry",
                  entry_rva, scroll_function, scroll_by_at_retail_speed);

    /*
     * Only the held-key behaviors are replaced. The mouse-wheel handler keeps
     * calling the stock zoomIn/zoomOut methods once per physical wheel detent.
     */
    patch_entry_jump(installer, "held zoom-in behavior",
                     camera->zoom_in_behavior_function,
                     held_zoom_in_at_retail_speed);
    patch_entry_jump(installer, "held zoom-out behavior",
                     camera->zoom_out_behavior_function,
                     held_zoom_out_at_retail_speed);

    /* The live GlobalData fields are changed later, after all code writes succeed. */
    expect_bytes(installer, "camera height-adjust instruction",
                 camera->camera_adjust_instruction,
                 camera_adjust_instruction, sizeof(camera_adjust_instruction));

    /* Preserve the retail 30 Hz camera-shake half-life at the raised client rate. */
    expect_bytes(installer, "camera shake damping instruction",
                 camera->shake_decay_operand - sizeof(shake_instruction),
                 shake_instruction, sizeof(shake_instruction));
    patch_pointer(installer, "camera shake damping operand",
                  camera->shake_decay_operand,
                  game_address(g_game, camera->retail_shake_decay),
                  &g_camera_shake_decay);
}

static void install_frame_authored_effects(PatchInstaller *installer) {
    const VisualLayout *visual = &g_game->visual;

    /* Keep all stored and compared timestamps in the same synthetic domain. */
    redirect_frame_read(installer, "tracer reset frame read",
                        visual->tracer_reset_frame_call, NO_INTERLEAVED_PUSH);
    redirect_frame_read(installer, "tracer update frame read",
                        visual->tracer_update_frame_call, PUSH_EDI);
    redirect_frame_read(installer, "cloud-effect frame read",
                        visual->cloud_frame_call, NO_INTERLEAVED_PUSH);
    redirect_frame_read(installer, "Anim2D timestamp frame read",
                        visual->anim2d_timestamp_frame_call, NO_INTERLEAVED_PUSH);
    redirect_frame_read(installer, "Anim2D update frame read",
                        visual->anim2d_update_frame_call, NO_INTERLEAVED_PUSH);
}

static void install_drawable_physics_timing(PatchInstaller *installer) {
    const DrawablePhysicsLayout *physics = &g_game->drawable_physics;
    u8 call_patch[5] = { 0xE8, 0, 0, 0, 0 };

    g_original_calc_physics_xform = (CalcPhysicsXformFn)game_address(
        g_game, physics->apply_call.target);
    /* The once-per-frame guard now advances the springs at the retail rate. */
    redirect_frame_read(installer, "physics-xform frame read",
                        physics->dispatcher_frame_call, PUSH_EBX);
    encode_rel32(&call_patch[1],
                 game_address(g_game, physics->apply_call.instruction + 5u),
                 calc_physics_xform_interpolated);
    patch_branch(installer, "applyPhysicsXform dispatcher call",
                 &physics->apply_call, 0xE8, call_patch, sizeof(call_patch));
}

/*
 * Units are drawn on a Catmull-Rom spline between their previous and current
 * logic positions. The history refresh and the spline evaluation only run
 * while "clientFrame - Drawable::historyFrame < 6": three logic ticks at
 * stock 30 FPS, but exactly one tick at 90 FPS. A measured 90 FPS trace
 * showed the test failing on every sixth frame, freezing the drawn position
 * for one frame (a 15 Hz stutter) and leaving stopped units short of their
 * destination until a stale-transform fallback snapped them into place.
 * Restore the stock three-tick window: 3 * targetFPS / 15 client frames.
 */
static void install_drawable_interpolation_window(PatchInstaller *installer) {
    static const char *const names[2] = {
        "interpolation history window", "interpolation spline window"
    };
    const u8 stock_window = 6u;
    u8 window = (u8)(3u * (g_config->target_fps / LOGIC_FPS));
    u32 i;

    for (i = 0; i < 2; ++i) {
        patch_bytes(installer, names[i],
                    g_game->drawable_interpolation.window_immediates[i],
                    &stock_window, &window, sizeof(window));
    }
}

static void install_particle_and_cursor_timing(PatchInstaller *installer) {
    static const u8 gpu_opcode[] = { 0x0F, 0xAF, 0x05 };
    static const u8 radius_opcode[] = { 0xD8, 0x0D };
    const VisualLayout *visual = &g_game->visual;
    u8 particle_patch[5] = { 0xE8, 0, 0, 0, 0 };

    /* Gate only CPU simulation; W3D buffer preparation remains per-client-frame. */
    encode_rel32(&particle_patch[1],
                 game_address(g_game, visual->particle_simulation.instruction + 5u),
                 update_particles_at_retail_rate);
    patch_branch(installer, "FX particle simulation call",
                 &visual->particle_simulation, 0xE8,
                 particle_patch, sizeof(particle_patch));

    {
        void *draw = game_address(g_game, g_game->particle_draw.default_draw_function);
        g_original_particle_draw = (ParticleDrawFn)draw;
        patch_pointer(installer, "CPU particle DefaultDrawModule draw vtable entry",
                      g_game->particle_draw.default_draw_vtable_entry, draw,
                      (const void *)draw_particles_interpolated);
    }

    expect_bytes(installer, "GPU particle expiry instruction",
                 visual->gpu_particle_fps_instruction,
                 gpu_opcode, sizeof(gpu_opcode));
    patch_pointer(installer, "GPU particle client-FPS operand",
                  visual->gpu_particle_fps_operand,
                  game_address(g_game, g_game->timing.client_fps),
                  &g_retail_visual_fps);

    /* Radius decal periods are milliseconds, so convert with targetFPS/1000. */
    expect_bytes(installer, "radius-cursor throb instruction",
                 visual->radius_cursor_fps_instruction,
                 radius_opcode, sizeof(radius_opcode));
    patch_pointer(installer, "radius-cursor frames-per-millisecond operand",
                  visual->radius_cursor_fps_operand,
                  game_address(g_game, visual->retail_frames_per_millisecond),
                  &g_client_frames_per_millisecond);
}

static void install_phase_scheduler(PatchInstaller *installer) {
    static const u8 batch_expected[] = {
        0x8D, 0x47, 0xFF, 0x0F, 0xAF, 0xC1, 0x99, 0x6A, 0x06, 0x5E, 0xF7,
        0xFE, 0x40, 0x6B, 0xC0, 0x06, 0x99, 0xF7, 0xF9, 0x8B, 0xF0, 0x46
    };
    static const u8 interpolation_expected[] = { 0xF3, 0x0F, 0x2A, 0x41, 0x40 };
    static const u8 flush_template[] = {
        0xA1, 0, 0, 0, 0, 0x33, 0xD2, 0xF7, 0x35, 0, 0, 0, 0,
        0x6A, 0x06, 0x8B, 0xC8, 0x58, 0x99, 0xF7, 0xF9, 0x5F, 0x8B, 0xC8,
        0x8B, 0x45, 0x08, 0x99, 0xF7, 0xF9, 0x85, 0xD2, 0x75, 0x05,
        0xE8, 0, 0, 0, 0
    };
    const SchedulerLayout *scheduler = &g_game->scheduler;
    u8 batch_patch[sizeof(batch_expected)];
    u8 interpolation_patch[sizeof(interpolation_expected)] = { 0xE9, 0, 0, 0, 0 };
    u8 flush_expected[sizeof(flush_template)];
    u8 flush_patch[sizeof(flush_template)];
    u32 batch_rva = scheduler->phase_batch_block;
    u32 interpolation_rva = scheduler->phase_interpolation_function;
    u32 flush_rva = scheduler->client_slice_flush.instruction - 34u;

    /*
     * Original register contract at KW 1.02 Steam VA 0x006560A6:
     *   EDI = first phase already dispatched, ECX = targetFPS / 15.
     * The following loop expects ESI to contain the exclusive phase limit.
     * __stdcall preserves EDI and cleans the two pushed arguments.
     */
    memset(batch_patch, 0x90, sizeof(batch_patch));
    batch_patch[0] = 0x51;
    batch_patch[1] = 0x57;
    batch_patch[2] = 0xE8;
    encode_rel32(&batch_patch[3], game_address(g_game, batch_rva + 7u),
                 get_phase_batch_limit);
    batch_patch[7] = 0x8B;
    batch_patch[8] = 0xF0;
    /* Remaining bytes are NOPs; execution rejoins at the original +22. */

    /*
     * Tail-jump from the standalone leaf entry. ECX remains GameEngine*, and
     * the helper's RET returns directly to the leaf's original caller.
     */
    encode_rel32(&interpolation_patch[1],
                 game_address(g_game, interpolation_rva + 5u),
                 update_phase_interpolation);

    memcpy(flush_expected, flush_template, sizeof(flush_expected));
    encode_u32(&flush_expected[1],
               (u32)(uintptr_t)game_address(g_game, g_game->timing.client_fps));
    encode_u32(&flush_expected[9],
               (u32)(uintptr_t)game_address(g_game, g_game->timing.logic_fps));
    encode_rel32(&flush_expected[35], game_address(g_game, flush_rva + 39u),
                 game_address(g_game, scheduler->client_slice_flush.target));

    /*
     * At KW 1.02 Steam VA 0x005EF10F, [EBP+8] is the current phase and the
     * function's saved EDI is still on the stack. The replacement asks whether
     * this phase ends the current client slice, restores EDI at the original
     * point, and conditionally calls the decoded original sub_6DE5B1 target.
     */
    memset(flush_patch, 0x90, sizeof(flush_patch));
    flush_patch[0] = 0xFF;
    flush_patch[1] = 0x75;
    flush_patch[2] = 0x08;
    flush_patch[3] = 0xE8;
    encode_rel32(&flush_patch[4], game_address(g_game, flush_rva + 8u),
                 is_client_slice_end);
    flush_patch[8] = 0x5F;
    flush_patch[9] = 0x85;
    flush_patch[10] = 0xC0;
    flush_patch[11] = 0x74;
    flush_patch[12] = 0x05;
    flush_patch[13] = 0xE8;
    encode_rel32(&flush_patch[14], game_address(g_game, flush_rva + 18u),
                 game_address(g_game, scheduler->client_slice_flush.target));
    /* The trailing NOPs rejoin at +39 with the original stack already restored. */

    patch_bytes(installer, "logic-phase batching block", batch_rva,
                batch_expected, batch_patch, sizeof(batch_patch));
    patch_bytes(installer, "logic-phase interpolation function", interpolation_rva,
                interpolation_expected, interpolation_patch,
                sizeof(interpolation_patch));
    patch_bytes(installer, "client-slice Drawable flush block", flush_rva,
                flush_expected, flush_patch, sizeof(flush_patch));
}

static void install_w3d_clock(PatchInstaller *installer) {
    static const u8 special_template[] = {
        0xA1, 0, 0, 0, 0, 0x03, 0x05, 0, 0, 0, 0, 0x50, 0xA3, 0, 0, 0, 0
    };
    static const u8 normal_template[] = {
        0xA1, 0, 0, 0, 0, 0x0F, 0xAF, 0xC6, 0x01, 0x05,
        0, 0, 0, 0, 0xFF, 0x35, 0, 0, 0, 0
    };
    const W3DClockLayout *clock = &g_game->w3d_clock;
    u32 accumulated = (u32)(uintptr_t)game_address(
        g_game, g_game->timing.w3d_accumulated_time_ms);
    u32 live_step = (u32)(uintptr_t)game_address(
        g_game, g_game->timing.w3d_milliseconds_per_frame);
    u8 special_expected[sizeof(special_template)];
    u8 normal_expected[sizeof(normal_template)];
    u8 special_patch[sizeof(special_template)];
    u8 normal_patch[sizeof(normal_template)];

    /*
     * Both overwritten blocks end immediately before an existing
     * W3D_SetCurrentTimeMs call. advance_w3d_time is __stdcall, returns the new
     * accumulated time in EAX, and cleans its one argument; PUSH EAX restores
     * the original setter's stack argument.
     */
    memcpy(special_expected, special_template, sizeof(special_expected));
    encode_u32(&special_expected[1], accumulated);
    encode_u32(&special_expected[7], live_step);
    encode_u32(&special_expected[13], accumulated);
    memset(special_patch, 0x90, sizeof(special_patch));
    special_patch[0] = 0x6A;
    special_patch[1] = 0x01;
    special_patch[2] = 0xE8;
    encode_rel32(&special_patch[3],
                 game_address(g_game, clock->special_advance_block + 7u),
                 advance_w3d_time);
    special_patch[7] = 0x50;

    /* ESI is the normal path's client-frame delta and is callee-saved. */
    memcpy(normal_expected, normal_template, sizeof(normal_expected));
    encode_u32(&normal_expected[1], live_step);
    encode_u32(&normal_expected[10], accumulated);
    encode_u32(&normal_expected[16], accumulated);
    memset(normal_patch, 0x90, sizeof(normal_patch));
    normal_patch[0] = 0x56;
    normal_patch[1] = 0xE8;
    encode_rel32(&normal_patch[2],
                 game_address(g_game, clock->normal_advance_block + 6u),
                 advance_w3d_time);
    normal_patch[6] = 0x50;

    patch_bytes(installer, "special W3D time advance", clock->special_advance_block,
                special_expected, special_patch, sizeof(special_patch));
    patch_bytes(installer, "normal W3D time advance", clock->normal_advance_block,
                normal_expected, normal_patch, sizeof(normal_patch));
}

static void install_frame_limiter(PatchInstaller *installer) {
    static const u8 display_branch[] = { 0x7D, 0x13 };
    static const u8 display_patch[] = { 0xEB, 0x13 };
    static const u8 interval_template[] = {
        0xDB, 0x86, 0x64, 0x01, 0x00, 0x00,
        0x8B, 0xD8,
        0xD8, 0x0D, 0, 0, 0, 0,
        0xD8, 0x3D, 0, 0, 0, 0,
        0xE8, 0, 0, 0, 0
    };
    const PacingLayout *pacing = &g_game->pacing;
    u8 interval_expected[sizeof(interval_template)];
    u8 interval_patch[sizeof(interval_template)];

    /* Remove the independent 29 ms presentation wait but retain its timestamp update. */
    patch_bytes(installer, "display limiter branch",
                pacing->display_limiter_branch,
                display_branch, display_patch, sizeof(display_patch));

    memcpy(interval_expected, interval_template, sizeof(interval_expected));
    encode_u32(&interval_expected[10], (u32)(uintptr_t)game_address(
        g_game, pacing->network_scale));
    encode_u32(&interval_expected[16], (u32)(uintptr_t)game_address(
        g_game, pacing->milliseconds_per_logic_frame));
    encode_rel32(&interval_expected[21],
                 game_address(g_game, pacing->limiter_interval_block +
                                      sizeof(interval_expected)),
                 game_address(g_game, pacing->limiter_conversion_target));

    memset(interval_patch, 0x90, sizeof(interval_patch));
    interval_patch[0] = 0x8B;
    interval_patch[1] = 0xD8; /* Preserve timeGetTime() in EBX. */
    interval_patch[2] = 0x56; /* GameEngine * from ESI. */
    interval_patch[3] = 0xE8;
    encode_rel32(&interval_patch[4],
                 game_address(g_game, pacing->limiter_interval_block + 8u),
                 get_retail_limiter_interval);
    patch_bytes(installer, "retail frame-limiter interval",
                pacing->limiter_interval_block,
                interval_expected, interval_patch, sizeof(interval_patch));
}

BOOL game_patches_install(const GameLayout *game, const Config *config) {
    PatchInstaller installer;

    /* Bootstrap and session hooks can both arrive; code bytes are patched once. */
    if (g_installed) return TRUE;
    if (game == NULL || config == NULL ||
        !select_camera_rate_constants(config->target_fps)) {
        return FALSE;
    }
    g_game = game;
    g_config = config;
    log_incoming_timing();

    g_visual_client_step = 1.0f / (float)config->target_fps;
    g_client_frames_per_millisecond = (float)config->target_fps / 1000.0f;
    patch_transaction_init(&installer.transaction);
    installer.ok = TRUE;

    install_continuous_visuals(&installer);
    install_keyboard_timing(&installer);
    install_audio_timing(&installer);
    install_objective_indicator_tick(&installer);
    install_selection_panel_name(&installer);
    install_camera_timing(&installer);
    install_frame_authored_effects(&installer);
    install_drawable_physics_timing(&installer);
    install_drawable_interpolation_window(&installer);
    install_particle_and_cursor_timing(&installer);
    install_phase_scheduler(&installer);
    install_w3d_clock(&installer);
    install_frame_limiter(&installer);

    /* No live timing globals have been changed yet, so rollback is complete. */
    if (!installer.ok) goto rollback;

    g_installed = TRUE;
    log_line("FPS patch installed successfully");
    log_u32("Static patch writes installed=", (u32)installer.transaction.count);
    return TRUE;

rollback:
    log_line("ERROR: patch installation failed; rolling back static patches");
    patch_transaction_rollback(&installer.transaction);
    return FALSE;
}

static void apply_live_camera_settings(void) {
    u8 *global_data;
    float *camera_adjust;
    float *keyboard_rotate;

    if (g_game == NULL || g_config == NULL) return;
    global_data = *(u8 **)game_address(g_game, g_game->timing.global_data_pointer);
    if (global_data == NULL) return;

    camera_adjust = (float *)(global_data + GLOBAL_DATA_CAMERA_ADJUST_SPEED);
    keyboard_rotate =
        (float *)(global_data + GLOBAL_DATA_KEYBOARD_CAMERA_ROTATE_SPEED);

    /*
     * A session/configuration reload may restore authored GlobalData values.
     * Capture a changed incoming value, but do not mistake our own previously
     * applied coefficient for a new retail baseline.
     */
    if (!g_camera_settings_captured ||
        load_u32(camera_adjust) != load_u32(&g_applied_camera_adjust_speed)) {
        g_retail_camera_adjust_speed = *camera_adjust;
    }
    if (!g_camera_settings_captured ||
        load_u32(keyboard_rotate) != load_u32(&g_applied_keyboard_rotate_speed)) {
        g_retail_keyboard_rotate_speed = *keyboard_rotate;
    }

    g_applied_camera_adjust_speed = normalize_camera_adjust_speed(
        g_retail_camera_adjust_speed, g_config->target_fps);
    g_applied_keyboard_rotate_speed =
        g_retail_keyboard_rotate_speed * g_camera_scroll_scale;
    *camera_adjust = g_applied_camera_adjust_speed;
    *keyboard_rotate = g_applied_keyboard_rotate_speed;
    g_camera_settings_captured = TRUE;
}

void game_patches_reset_state(void) {
    if (g_config != NULL) {
        /* Seed so particle simulation runs on the first client frame of a session. */
        g_particle_update_accumulator = g_config->target_fps - g_retail_visual_fps;
        /* Seed so audio service runs before the first logic-phase slice. */
        g_audio_update_accumulator = g_config->target_fps - g_retail_visual_fps;
        g_w3d_time_remainder = 0;
        g_limiter_interval_remainder = 0;
        g_limiter_interval_divisor = 0;
        /* Drawables from a previous session may share addresses with new ones. */
        memset(g_physics_cache, 0, sizeof(g_physics_cache));
        /* Each session may follow a new vehicle for the motion diagnostic. */
        g_motion_drawable = NULL;
        memset(g_particle_cache, 0, sizeof(g_particle_cache));
        g_frame_times_logged = 0;
        reset_keyboard_clock();
        /* First direct reader gets floor(990/FPS); the hook distributes remainder. */
        *(u32 *)game_address(g_game, g_game->timing.w3d_milliseconds_per_frame) =
            W3D_RETAIL_MILLISECONDS_PER_SECOND / g_config->target_fps;
        apply_live_camera_settings();
    }
}
