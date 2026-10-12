#include "game_patches.h"

#include "log.h"
#include "memory_patch.h"
#include "particle_index.h"
#include "render_fixes.h"
#include "profiler.h"

#include <math.h>
#include <stdio.h>

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
/*
 * Effective client rate. Everything rate-dependent uses this instead of the
 * configured target: it equals target_fps unless adaptive pacing lowered it
 * because the PC could not hold the target (see the pacing monitor).
 */
static u32 g_rate = 30u;
static u32 g_rate_pending = 30u;
/* Logic phases dispatched during the current client frame. */
static u32 g_frame_phases;
/* Microseconds spent in logic dispatches this frame (adaptive pacing). */
static u32 g_frame_logic_us;
/* Microseconds and steps of the 30 Hz particle/audio updates this frame. */
static u32 g_frame_effects_us;
static u32 g_frame_effects_steps;
static LARGE_INTEGER g_logic_timer_frequency;

static u32 microseconds_since(const LARGE_INTEGER *start) {
    LARGE_INTEGER end;

    QueryPerformanceCounter(&end);
    if (g_logic_timer_frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&g_logic_timer_frequency);
    }
    return (u32)((end.QuadPart - start->QuadPart) * 1000000 /
                 g_logic_timer_frequency.QuadPart);
}
static void monitor_frame_pacing(u32 interval_ms);
static void apply_live_camera_settings(void);
/* Elapsed fraction of the current 30 Hz particle step; frozen while paused. */
static float g_particle_blend_fraction;
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
static BOOL own_schedule(void);

static u32 STDCALL get_phase_batch_limit(u32 phase, u32 ratio) {
    /* ceil(ratio * phase / 6) identifies this phase's client-frame slice. */
    u32 client_slot = (ratio * phase + LOGIC_PHASE_COUNT - 1u) / LOGIC_PHASE_COUNT;

    /* Our scheduler decides the batches: the stock dispatch runs one phase. */
    if (own_schedule()) return phase + 1u;
    /* The stock loop increments EDI first, hence the exclusive +1 limit. */
    return (LOGIC_PHASE_COUNT * client_slot) / ratio + 1u;
}

/*
 * Above 90 FPS (more than six client frames per logic tick) some client
 * frames must run no logic phase at all. GameEngine::update advances the
 * phase and dispatches once per client frame, wrapping to phase 1 (the new
 * logic tick, which may stall on the network) after phase 6. Its dispatch
 * call goes through the engine vtable to a wrapper which follows this
 * schedule for a tick of F client frames:
 *
 *   frame 1        phase 1 only (tick start; "client frame boundary" tests
 *                  phase == 1 above ratio 6, so it must not repeat)
 *   frames 2..F-1  phases 2-5, spread evenly (zero or one per frame)
 *   frame F        phase 6 (else GameEngine::update would wrap early)
 *
 * On a frame without a phase the wrapper undoes the engine's increment. F
 * alternates by Bresenham so ticks average exactly targetFPS / 15 frames
 * (160 FPS: 11, 11, 10), and interpolation advances every frame as j / F.
 */
typedef void(THISCALL *DispatchLogicPhaseFn)(void *engine, u32 phase);
static DispatchLogicPhaseFn g_original_dispatch_logic_phase;
static u32 g_tick_frames;      /* F for the current tick; 0 until the first tick */
static u32 g_tick_frame;       /* j, 1..F */
static u32 g_tick_accumulator; /* Bresenham remainder in fifteenths */

static BOOL high_rate_schedule(void) {
    return g_rate > LOGIC_FPS * LOGIC_PHASE_COUNT;
}

/*
 * Our scheduler runs every rate above 15 except the stock multiples of 15
 * (30, 45, 60, 75, 90), whose schedules the game handles itself. Between 15
 * and 90 a tick then gets F = 1..6 frames (Bresenham), each running a batch
 * of phases; above 90, F >= 7 and some frames run none.
 */
static BOOL own_schedule(void) {
    return high_rate_schedule() || (g_rate > LOGIC_FPS && g_rate % LOGIC_FPS != 0u);
}

/* Last phase of the tick's current batch (is_client_slice_end). */
static u32 g_frame_target_phase;

/* Whole client frames per logic tick; below 15 FPS a frame runs whole ticks. */
static u32 client_frames_per_tick(void) {
    u32 ratio = g_rate / LOGIC_FPS;
    return ratio != 0u ? ratio : 1u;
}

static void apply_pending_rate(void);

static u32 scheduled_phases_done(u32 frame, u32 frames) {
    if (frames <= LOGIC_PHASE_COUNT) {
        /* F <= 6: batches as the stock schedule (75 FPS: 1, 1, 1, 1, 2). */
        if (frame >= frames) return LOGIC_PHASE_COUNT;
        return (LOGIC_PHASE_COUNT * (frame != 0u ? frame : 1u)) / frames;
    }
    if (frame <= 1u) return 1u;
    if (frame >= frames) return LOGIC_PHASE_COUNT;
    /* Phases 2..5 over frames 2..F-1 (F > 6, so steps are 0 or 1). */
    return 2u + (3u * (frame - 2u)) / (frames - 3u);
}

static void write_tick_interpolation(u8 *engine) {
    *(float *)(engine + ENGINE_LOGIC_PHASE_INTERPOLATION) =
        (float)g_tick_frame / (float)g_tick_frames;
}

static BOOL logic_tick_running(void) {
    const u8 *game_client = *(const u8 *const *)game_address(
        g_game, g_game->drawable_physics.game_client_pointer);
    return game_client != NULL &&
           game_client[g_game->particle_draw.client_simulates_offset] != 0;
}

/*
 * Below 15 FPS (adaptive pacing) one logic tick per frame is not enough. The
 * game's own schedule runs one whole tick per frame (its rate global stays
 * at 15); after that tick, more whole ticks are dispatched so ticks average
 * 15 per second of frames (Bresenham: 10 FPS gives 1, 2, 1, 2...). A network
 * stall ends the burst; at most four extra ticks run per frame.
 */
/*
 * Dispatch and count the logic phases that ran (the stock batch advances
 * the engine's phase; a network stall at phase 1 runs none).
 */
static void dispatch_counted(u8 *engine, u32 phase) {
    LARGE_INTEGER start;
    u32 after;

    LONG zone = profile_enter(PROFILE_ZONE_LOGIC);

    QueryPerformanceCounter(&start);
    g_original_dispatch_logic_phase(engine, phase);
    g_frame_logic_us += microseconds_since(&start);
    profile_leave(zone);
    if (phase == 1u && !logic_tick_running()) return;
    after = *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE);
    g_frame_phases += after >= phase ? after - phase + 1u : 1u;
}

enum { MAX_CATCH_UP_TICKS = 4 };
static u32 g_catch_up_accumulator;

static void run_catch_up_ticks(u8 *engine) {
    u32 ticks;
    u32 i;

    g_catch_up_accumulator += LOGIC_FPS;
    ticks = g_catch_up_accumulator / g_rate;
    g_catch_up_accumulator %= g_rate;
    /* The frame's own tick has already run. */
    for (i = 1; i < ticks && i <= MAX_CATCH_UP_TICKS; ++i) {
        *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE) = 1u;
        dispatch_counted(engine, 1u);
        if (!logic_tick_running()) {
            /* Stalled: the next frame wraps and retries the tick. */
            *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE) = LOGIC_PHASE_COUNT;
            break;
        }
    }
}

/* Run phases first..last as this frame's batch (one stock dispatch each). */
static void dispatch_batch(u8 *engine, u32 first, u32 last) {
    u32 phase;

    g_frame_target_phase = last;
    for (phase = first; phase <= last; ++phase) {
        *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE) = phase;
        dispatch_counted(engine, phase);
    }
}

static void THISCALL dispatch_logic_phase_scheduled(void *engine_pointer, u32 phase) {
    u8 *engine = (u8 *)engine_pointer;
    u32 next;
    u32 target;

    /* A new logic tick is the only safe point to change the client rate. */
    if (phase == 1u) apply_pending_rate();
    if (!own_schedule()) {
        dispatch_counted(engine, phase);
        if (phase == 1u && g_rate < LOGIC_FPS && logic_tick_running()) {
            run_catch_up_ticks(engine);
        }
        return;
    }
    if (phase == 1u) {
        /* The first frame's batch ends here; F is committed if phase 1 runs. */
        g_frame_target_phase =
            scheduled_phases_done(1u, (g_tick_accumulator + g_rate) / LOGIC_FPS);
        dispatch_counted(engine, phase);
        /* A network stall clears the GameClient flag; the engine then reverts. */
        if (logic_tick_running()) {
            g_tick_accumulator += g_rate;
            g_tick_frames = g_tick_accumulator / LOGIC_FPS;
            g_tick_accumulator %= LOGIC_FPS;
            g_tick_frame = 1u;
            /* The rest of the first frame's batch (F <= 6 only). */
            target = scheduled_phases_done(1u, g_tick_frames);
            if (target > 1u) dispatch_batch(engine, 2u, target);
        }
        if (g_tick_frames != 0u) write_tick_interpolation(engine);
        return;
    }
    if (g_tick_frames == 0u) {
        /* Before the first scheduled tick: one phase per frame, as at 90 FPS. */
        g_frame_target_phase = phase;
        dispatch_counted(engine, phase);
        return;
    }
    next = g_tick_frame < g_tick_frames ? g_tick_frame + 1u : g_tick_frames;
    target = scheduled_phases_done(next, g_tick_frames);
    if (target >= phase) {
        dispatch_batch(engine, phase, target);
    } else {
        /* Render-only frame: undo GameEngine::update's phase increment. */
        *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE) = phase - 1u;
    }
    g_tick_frame = next;
    write_tick_interpolation(engine);
}

/*
 * GameEngine::IsClientFrameBoundary, for our schedules: true while the phase
 * counter holds the last phase of the tick's first frame, which is what the
 * stock "6 / ratio" (or "1" above ratio 6) means for every whole ratio.
 */
static u32 STDCALL is_client_frame_boundary(u8 *engine) {
    u32 phase = *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE);
    u32 ratio;

    if (own_schedule() && g_tick_frames != 0u) {
        return phase == scheduled_phases_done(1u, g_tick_frames);
    }
    ratio = *(u32 *)game_address(g_game, g_game->timing.client_fps) /
            *(u32 *)game_address(g_game, g_game->timing.logic_fps);
    if (ratio < LOGIC_PHASE_COUNT) {
        return ratio != 0u && phase == LOGIC_PHASE_COUNT / ratio;
    }
    return phase == 1u;
}

/* ECX = engine, result in AL; ECX and EDX are kept like the stock leaf's callers expect. */
__attribute__((naked)) static void is_client_frame_boundary_entry(void) {
    __asm__ volatile(
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "calll %c0\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "retl"
        :
        : "i"(is_client_frame_boundary));
}

/*
 * KW 1.02 GameEngine_UpdatePhaseInterpolation, Steam VA 0x0064333E.
 * Interpolation must advance once per client slice, not once per raw phase;
 * otherwise the 2-3 and 5-6 batches at 60 FPS expose the wrong visual sample.
 */
static void THISCALL update_phase_interpolation(void *engine_pointer) {
    u8 *engine = (u8 *)engine_pointer;
    u32 ratio = client_frames_per_tick();
    u32 phase = *(u32 *)(engine + ENGINE_CURRENT_LOGIC_PHASE);
    u32 client_slot = (ratio * phase + LOGIC_PHASE_COUNT - 1u) / LOGIC_PHASE_COUNT;
    float interpolation;

    if (own_schedule() && g_tick_frames != 0u) {
        write_tick_interpolation(engine);
        return;
    }
    if (ratio == 3u || ratio >= 6u) {
        /* Preserve bit-for-formula stock behavior for the already-valid ratios. */
        interpolation = (float)phase / (float)LOGIC_PHASE_COUNT;
    } else {
        interpolation = (float)client_slot / (float)ratio;
    }

    if (interpolation > 1.0f) interpolation = 1.0f;
    *(float *)(engine + ENGINE_LOGIC_PHASE_INTERPOLATION) = interpolation;
}

/*
 * GameEngine::update keeps ECX (this) live across its calls of the stock
 * interpolation leaf and then dispatches through [ECX], because that leaf
 * only touches XMM0/XMM1 and the flags. The C replacement may use other
 * registers, so the patched entry goes through this stub, which preserves
 * EAX/ECX/EDX and XMM2-XMM7 around it.
 */
__attribute__((naked)) static void update_phase_interpolation_entry(void) {
    __asm__ volatile(
        "pushl %%eax\n\t"
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "subl $96, %%esp\n\t"
        "movdqu %%xmm2, 0(%%esp)\n\t"
        "movdqu %%xmm3, 16(%%esp)\n\t"
        "movdqu %%xmm4, 32(%%esp)\n\t"
        "movdqu %%xmm5, 48(%%esp)\n\t"
        "movdqu %%xmm6, 64(%%esp)\n\t"
        "movdqu %%xmm7, 80(%%esp)\n\t"
        "calll %c0\n\t"
        "movdqu 0(%%esp), %%xmm2\n\t"
        "movdqu 16(%%esp), %%xmm3\n\t"
        "movdqu 32(%%esp), %%xmm4\n\t"
        "movdqu 48(%%esp), %%xmm5\n\t"
        "movdqu 64(%%esp), %%xmm6\n\t"
        "movdqu 80(%%esp), %%xmm7\n\t"
        "addl $96, %%esp\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "popl %%eax\n\t"
        "retl"
        :
        : "i"(update_phase_interpolation));
}

/*
 * KW 1.02 GameLogic_UpdatePhase, Steam VA 0x005EF10F, drains pending
 * Object-to-Drawable updates once after the final phase in each client slice.
 */
static BOOL STDCALL is_client_slice_end(u32 phase) {
    u32 ratio = client_frames_per_tick();
    u32 client_slot;
    u32 end_phase;

    /* Our schedules: a slice is this frame's batch. */
    if (own_schedule()) return phase >= g_frame_target_phase;
    /* From six client frames per tick on, every phase is its own slice. */
    if (ratio >= LOGIC_PHASE_COUNT) return TRUE;
    client_slot = (ratio * phase + LOGIC_PHASE_COUNT - 1u) / LOGIC_PHASE_COUNT;
    /* floor(6 * slot / ratio) is the final phase assigned to that slice. */
    end_phase = (LOGIC_PHASE_COUNT * client_slot) / ratio;
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
    elapsed = scaled / g_rate;
    g_w3d_time_remainder = scaled % g_rate;
    current += elapsed;
    *accumulated = current;
    /* Publish the next quotient so direct step readers share the same long-term rate. */
    *live_step = (g_w3d_time_remainder + W3D_RETAIL_MILLISECONDS_PER_SECOND) / g_rate;
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

/*
 * The synthetic 30 Hz frame is kept as a running count (whole 30 Hz frames
 * plus a remainder in 1/rate units) advanced by each new client frame, so it
 * stays continuous when the effective rate changes. Its value is
 * ceil(elapsed * 30): it advances on the first client frame of each 30 Hz
 * interval, like the original ceil(frame * 30 / target).
 */
static BOOL g_retail_started;
static u32 g_retail_client_frame;
static u32 g_retail_whole;
static u32 g_retail_remainder;
static BOOL g_retail_advanced;

static u32 retail_frame_value(void) {
    return g_retail_whole + (g_retail_remainder != 0u ? 1u : 0u);
}

static void sync_retail_frame(u32 client_frame) {
    u32 delta = client_frame - g_retail_client_frame;
    u32 before;
    u64 total;

    if (g_retail_started && delta == 0u) return;
    if (!g_retail_started || delta > 0x10000u) {
        total = (u64)client_frame * g_retail_visual_fps;
        g_retail_whole = (u32)(total / g_rate);
        g_retail_remainder = (u32)(total % g_rate);
        g_retail_client_frame = client_frame;
        g_retail_started = TRUE;
        g_retail_advanced = TRUE;
        return;
    }
    before = retail_frame_value();
    total = (u64)g_retail_remainder + (u64)delta * g_retail_visual_fps;
    g_retail_whole += (u32)(total / g_rate);
    g_retail_remainder = (u32)(total % g_rate);
    g_retail_client_frame = client_frame;
    g_retail_advanced = retail_frame_value() != before;
}

/* Position within the current 30 Hz interval, (0, 1]. */
static float retail_frame_fraction(void) {
    return g_retail_remainder == 0u ? 1.0f : (float)g_retail_remainder / (float)g_rate;
}

static u32 THISCALL get_retail_visual_frame(void *game_client) {
    if (game_client == NULL || g_config->target_fps < g_retail_visual_fps) return 0;
    sync_retail_frame(read_client_frame(game_client));
    return retail_frame_value();
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
    u32 actual_frame;
    u32 retail_frame;
    float alpha;
    u32 i;

    if (game_client == NULL) return g_original_calc_physics_xform(drawable, info);
    actual_frame = read_client_frame(game_client);
    log_motion_sample(drawable, actual_frame);
    sync_retail_frame(actual_frame);
    retail_frame = retail_frame_value();
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
    alpha = retail_frame_fraction();
    for (i = 0; i < PHYSICS_XFORM_FLOATS; ++i) {
        info[i] = entry->previous[i] + (entry->current[i] - entry->previous[i]) * alpha;
    }
    return 1;
}

/*
 * Clicking the minimap moves the camera there; dragging pans it. Each drag
 * move calls the radar's look-at, which snaps the view, but moves only arrive
 * with the UI's ~30 Hz update, so panning stepped. The click still snaps at
 * once. Each drag move only sets the target the game would have looked at;
 * every rendered frame the shown point closes 1 - exp(-dt / 18 ms) of the
 * remaining distance, so every step is lightly eased without feeling floaty.
 */
typedef void(THISCALL *MinimapConvertFn)(void *handler, float *world, const void *radar);
typedef void(THISCALL *ViewLookAtFn)(void *view, const float *world);

static const float MINIMAP_SMOOTH_SECONDS = 0.018f;
static const float MINIMAP_SETTLE_DISTANCE = 0.5f;

static MinimapConvertFn g_minimap_convert[MINIMAP_LOOK_AT_MAX];
static float g_minimap_target[3];
static float g_minimap_shown[3];
static BOOL g_minimap_has_shown;
static BOOL g_minimap_following;
static LARGE_INTEGER g_minimap_last_tick;
static LARGE_INTEGER g_minimap_frequency;

static void minimap_view_look_at(const float *world) {
    void *view = *(void **)game_address(g_game, g_game->minimap.view_pointer);
    if (view == NULL) return;
    ((ViewLookAtFn)(*(void ***)view)[VIEW_LOOK_AT_SLOT])(view, world);
}

static void minimap_look_at(u32 copy, BOOL click, void *drag_operation,
                            const void *radar_point) {
    void *handler = *(void **)((u8 *)drag_operation + MINIMAP_DRAG_HANDLER);
    float world[3];

    g_minimap_convert[copy](handler, world, radar_point);
    if (click || !g_minimap_has_shown || g_config->target_fps <= g_retail_visual_fps) {
        /* The click goes there at once, like retail. */
        memcpy(g_minimap_shown, world, sizeof(g_minimap_shown));
        g_minimap_has_shown = TRUE;
        g_minimap_following = FALSE;
        minimap_view_look_at(world);
        return;
    }
    memcpy(g_minimap_target, world, sizeof(g_minimap_target));
    if (!g_minimap_following) {
        if (g_minimap_frequency.QuadPart == 0) QueryPerformanceFrequency(&g_minimap_frequency);
        QueryPerformanceCounter(&g_minimap_last_tick);
        g_minimap_following = TRUE;
    }
}

static void THISCALL minimap_click_first(void *drag_operation, const void *radar_point) {
    minimap_look_at(0, TRUE, drag_operation, radar_point);
}

static void THISCALL minimap_move_first(void *drag_operation, const void *radar_point) {
    minimap_look_at(0, FALSE, drag_operation, radar_point);
}

static void THISCALL minimap_click_second(void *drag_operation, const void *radar_point) {
    minimap_look_at(1, TRUE, drag_operation, radar_point);
}

static void THISCALL minimap_move_second(void *drag_operation, const void *radar_point) {
    minimap_look_at(1, FALSE, drag_operation, radar_point);
}

/* Once per rendered frame (also while paused). */
static void advance_minimap_glide(void) {
    LARGE_INTEGER now;
    float dt;
    float keep;
    float distance_sq = 0.0f;
    u32 k;

    if (!g_minimap_following) return;
    QueryPerformanceCounter(&now);
    dt = (float)((double)(now.QuadPart - g_minimap_last_tick.QuadPart) /
                 (double)g_minimap_frequency.QuadPart);
    g_minimap_last_tick = now;
    if (dt > 0.25f) dt = 0.25f;
    keep = expf(-dt / MINIMAP_SMOOTH_SECONDS);
    for (k = 0; k < 3; ++k) {
        float remaining = (g_minimap_target[k] - g_minimap_shown[k]) * keep;
        g_minimap_shown[k] = g_minimap_target[k] - remaining;
        distance_sq += remaining * remaining;
    }
    if (distance_sq < MINIMAP_SETTLE_DISTANCE * MINIMAP_SETTLE_DISTANCE) {
        memcpy(g_minimap_shown, g_minimap_target, sizeof(g_minimap_shown));
        g_minimap_following = FALSE;
    }
    minimap_view_look_at(g_minimap_shown);
}

static BOOL particles_simulate(void) {
    const ParticleDrawLayout *draw = &g_game->particle_draw;
    const u8 *game_client = *(const u8 *const *)game_address(
        g_game, g_game->drawable_physics.game_client_pointer);
    const u8 *game_logic = *(const u8 *const *)game_address(g_game, draw->game_logic_pointer);

    return game_client != NULL && game_logic != NULL &&
           game_client[draw->client_simulates_offset] != 0 &&
           game_logic[draw->logic_paused_offset] == 0;
}

static void THISCALL update_particles_at_retail_rate(void *manager) {
    SubsystemUpdateFn update;

    if (manager == NULL) return;
    advance_minimap_glide();
    update = (SubsystemUpdateFn)game_address(
        g_game, g_game->visual.particle_simulation.target);
    if (g_config->target_fps <= g_retail_visual_fps) {
        update(manager);
        return;
    }
    /*
     * Bresenham-style rate conversion: exactly 30 calls per second of client
     * frames. Below 30 FPS (adaptive pacing) a frame runs more than one.
     */
    g_particle_update_accumulator += g_retail_visual_fps;
    while (g_particle_update_accumulator >= g_rate) {
        LARGE_INTEGER start;

        g_particle_update_accumulator -= g_rate;
        ++g_particle_sim_tick;
        LONG zone = profile_enter(PROFILE_ZONE_EFFECTS);

        QueryPerformanceCounter(&start);
        update(manager);
        g_frame_effects_us += microseconds_since(&start);
        ++g_frame_effects_steps;
        profile_leave(zone);
    }
    /*
     * While particles are not simulated (e.g. the game is paused), drawing
     * keeps the last fraction; a cycling one made particles oscillate.
     */
    if (particles_simulate()) {
        g_particle_blend_fraction = (float)g_particle_update_accumulator / (float)g_rate;
    }
}

/*
 * CPU particles are simulated at the retail 30 Hz, so drawn as-is they move,
 * grow, spin and fade in 30 Hz steps. While a system is drawn, each value the
 * draw module reads (position, colour, alpha, size, rotations) is temporarily
 * replaced by its value projected from the last two 30 Hz steps to the
 * current moment, then restored, so simulation never sees projected values. This covers the
 * Default, Streak, Lightning and RenderObject draw modules, which all read
 * particles through the storage module's virtual count/get.
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
    PARTICLE_DRAW_MAX = 4096,
    PARTICLE_BLEND_VALUES = 11, /* largest field table */
    PARTICLE_RESTORE_MAX = PARTICLE_DRAW_MAX * PARTICLE_BLEND_VALUES
};

/*
 * Per-particle module objects and the float fields the draw modules read.
 * A missing module (NULL pointer) simply leaves those values unblended.
 * Default/Streak/Lightning particles use DefaultUpdateParticleModule (size
 * +0x10, rotations +0x08/+0x24); RenderObject particles use
 * RenderObjectParticleUpdateModule (size vector +0x30..+0x38, rotation +0x08).
 */
typedef struct ParticleBlendField {
    u32 module;     /* particle offset of the module pointer, 0 = particle itself */
    u32 offset;     /* float field inside it */
    BOOL non_negative; /* sizes, colours and alpha never project below zero */
} ParticleBlendField;

static const ParticleBlendField g_default_particle_fields[] = {
    { 0, 0x10, FALSE }, { 0, 0x14, FALSE }, { 0, 0x18, FALSE },          /* position */
    { 0x5C, 0x94, TRUE }, { 0x5C, 0x98, TRUE }, { 0x5C, 0x9C, TRUE },    /* colour */
    { 0x60, 0x48, TRUE },                                                /* alpha */
    { 0x64, 0x10, TRUE },                                                /* size */
    { 0x64, 0x08, FALSE }, { 0x64, 0x24, FALSE }                         /* rotations */
};
static const ParticleBlendField g_render_object_particle_fields[] = {
    { 0, 0x10, FALSE }, { 0, 0x14, FALSE }, { 0, 0x18, FALSE },          /* position */
    { 0x5C, 0x94, TRUE }, { 0x5C, 0x98, TRUE }, { 0x5C, 0x9C, TRUE },    /* colour */
    { 0x60, 0x48, TRUE },                                                /* alpha */
    { 0x64, 0x30, TRUE }, { 0x64, 0x34, TRUE }, { 0x64, 0x38, TRUE },    /* size xyz */
    { 0x64, 0x08, FALSE }                                                /* rotation */
};

typedef struct ParticleCacheEntry {
    u8 *particle;
    u32 tick;
    u32 life;
    float previous[PARTICLE_BLEND_VALUES];
    float current[PARTICLE_BLEND_VALUES];
} ParticleCacheEntry;

static ParticleDrawFn g_original_particle_draw[PARTICLE_DRAW_KINDS];
static ParticleCacheEntry g_particle_cache[PARTICLE_CACHE_SIZE];
static float *g_particle_restore_field[PARTICLE_RESTORE_MAX];
static float g_particle_restore_value[PARTICLE_RESTORE_MAX];

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
                           const float *values, u32 life) {
    u32 tick = g_particle_sim_tick;
    /* Same particle: life unchanged within a step, or one less per step. */
    BOOL continues = !is_new && (entry->life == life || entry->life == life + 1u);

    if (!continues) {
        memcpy(entry->previous, values, sizeof(entry->previous));
    } else if (entry->tick != tick) {
        if (entry->tick + 1u == tick) {
            memcpy(entry->previous, entry->current, sizeof(entry->previous));
        } else {
            memcpy(entry->previous, values, sizeof(entry->previous));
        }
    }
    memcpy(entry->current, values, sizeof(entry->current));
    entry->tick = tick;
    entry->life = life;
}

static int draw_particles_interpolated(u32 kind, void *draw_module,
                                       void *a, void *b, void *c) {
    ParticleDrawFn original = g_original_particle_draw[kind];
    const ParticleBlendField *field_table = kind == PARTICLE_DRAW_RENDER_OBJECT
        ? g_render_object_particle_fields : g_default_particle_fields;
    u32 field_count = kind == PARTICLE_DRAW_RENDER_OBJECT
        ? (u32)ARRAY_COUNT(g_render_object_particle_fields)
        : (u32)ARRAY_COUNT(g_default_particle_fields);
    float value;
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
        return original(draw_module, a, b, c);
    }
    storage = *(void **)(system + PARTICLE_SYSTEM_STORAGE);
    if (storage == NULL) return original(draw_module, a, b, c);

    vtable = *(void ***)storage;
    count = ((ParticleStorageCountFn)vtable[PARTICLE_STORAGE_COUNT_SLOT])(storage);
    get_particle = (ParticleStorageGetFn)vtable[PARTICLE_STORAGE_GET_SLOT];
    if (count > PARTICLE_DRAW_MAX) count = PARTICLE_DRAW_MAX;
    /* Fraction of the current 30 Hz step that has elapsed. */
    alpha = g_particle_blend_fraction;

    for (i = 0; i < count; ++i) {
        u8 *particle = get_particle(storage, i);
        float *fields[PARTICLE_BLEND_VALUES];
        float values[PARTICLE_BLEND_VALUES];
        ParticleCacheEntry *entry;
        BOOL is_new;
        u32 k;

        if (particle == NULL) continue;
        for (k = 0; k < PARTICLE_BLEND_VALUES; ++k) {
            u8 *base = particle;
            fields[k] = NULL;
            values[k] = 0.0f;
            if (k >= field_count) continue;
            if (field_table[k].module != 0) {
                base = *(u8 **)(particle + field_table[k].module);
            }
            if (base == NULL) continue;
            fields[k] = (float *)(base + field_table[k].offset);
            values[k] = *fields[k];
        }
        entry = find_particle_entry(particle, &is_new);
        if (entry == NULL) continue;
        track_particle(entry, is_new, values,
                       *(u32 *)(particle + PARTICLE_LIFE_REMAINING));

        for (k = 0; k < PARTICLE_BLEND_VALUES; ++k) {
            if (fields[k] == NULL) continue;
            g_particle_restore_field[saved] = fields[k];
            g_particle_restore_value[saved] = values[k];
            ++saved;
            /*
             * Project forward from the latest step by the elapsed fraction
             * of the next one. Unlike blending between the last two steps,
             * this has no lag, so a particle fading out reaches its final
             * state as the simulation removes it instead of popping.
             */
            value = entry->current[k] + (entry->current[k] - entry->previous[k]) * alpha;
            if (field_table[k].non_negative && value < 0.0f) value = 0.0f;
            *fields[k] = value;
        }
    }

    result = original(draw_module, a, b, c);

    /* Reverse order also restores correctly if a module is shared. */
    while (saved != 0) {
        --saved;
        *g_particle_restore_field[saved] = g_particle_restore_value[saved];
    }
    return result;
}

static int THISCALL draw_default_particles(void *module, void *a, void *b, void *c) {
    return draw_particles_interpolated(PARTICLE_DRAW_DEFAULT, module, a, b, c);
}

static int THISCALL draw_streak_particles(void *module, void *a, void *b, void *c) {
    return draw_particles_interpolated(PARTICLE_DRAW_STREAK, module, a, b, c);
}

static int THISCALL draw_lightning_particles(void *module, void *a, void *b, void *c) {
    return draw_particles_interpolated(PARTICLE_DRAW_LIGHTNING, module, a, b, c);
}

static int THISCALL draw_render_object_particles(void *module, void *a, void *b, void *c) {
    return draw_particles_interpolated(PARTICLE_DRAW_RENDER_OBJECT, module, a, b, c);
}

/*
 * Swarm particles (Scrin Buzzers) keep plain records that the swarm draw turns
 * into vertices. Each 30 Hz step does "velocity += attraction; position +=
 * velocity", so while a swarm is drawn each position is temporarily moved
 * forward by velocity times the elapsed fraction of the next step.
 */
static int THISCALL draw_swarm_interpolated(void *module, void *a, void *b, void *c) {
    ParticleDrawFn original = g_original_particle_draw[PARTICLE_DRAW_SWARM];
    u8 *system = *(u8 **)((u8 *)module + PARTICLE_DRAW_MODULE_SYSTEM);
    u8 *storage;
    u8 *record;
    u8 *end;
    float alpha;
    u32 saved = 0;
    u32 drawn = 0;
    int result;

    if (system == NULL || g_config->target_fps <= g_retail_visual_fps) {
        return original(module, a, b, c);
    }
    storage = *(u8 **)(system + PARTICLE_SYSTEM_STORAGE);
    if (storage == NULL) return original(module, a, b, c);

    alpha = g_particle_blend_fraction;
    record = *(u8 **)(storage + SWARM_STORAGE_RECORDS_BEGIN);
    end = *(u8 **)(storage + SWARM_STORAGE_RECORDS_END);
    for (; record != NULL && record < end && drawn < SWARM_DRAW_MAX &&
           saved + 3u <= PARTICLE_RESTORE_MAX;
         record += SWARM_RECORD_SIZE, ++drawn) {
        float *position = (float *)record;
        const float *velocity = (const float *)(record + SWARM_RECORD_VELOCITY);
        u32 k;
        for (k = 0; k < 3; ++k) {
            g_particle_restore_field[saved] = &position[k];
            g_particle_restore_value[saved] = position[k];
            ++saved;
            position[k] += velocity[k] * alpha;
        }
    }

    result = original(module, a, b, c);

    while (saved != 0) {
        --saved;
        *g_particle_restore_field[saved] = g_particle_restore_value[saved];
    }
    return result;
}

/*
 * Tracers (infantry/defence bullets) move by "speed" per 30 Hz frame, and the
 * tracer manager only ran on synthetic 30 Hz frames, so bullets advanced in
 * 30 Hz jumps. The movement/expiry pass now also runs on the client frames in
 * between, with each tracer's speed scaled by 30/targetFPS during its update.
 * Emission and expiry still follow the 30 Hz frame, so authored counts and
 * lifetimes are unchanged; tracers move linearly, so this adds no lag.
 */
typedef void(THISCALL *TracerManagerUpdateFn)(void *manager);
typedef void(THISCALL *TracerMovementPassFn)(void *manager, u32 frame);
typedef u8(THISCALL *TracerUpdateFn)(void *tracer, u32 frame);
static TracerManagerUpdateFn g_original_tracer_manager_update;
static TracerMovementPassFn g_tracer_movement_pass;
static TracerUpdateFn g_original_tracer_update;

static void THISCALL tracer_manager_update_every_frame(void *manager) {
    void *game_client = *(void **)game_address(
        g_game, g_game->drawable_physics.game_client_pointer);
    u32 frame;

    if (game_client == NULL) {
        g_original_tracer_manager_update(manager);
        return;
    }
    frame = get_retail_visual_frame(game_client);
    if (frame != *(u32 *)((u8 *)manager + TRACER_MANAGER_LAST_FRAME)) {
        g_original_tracer_manager_update(manager);
    } else if (particles_simulate()) {
        /* Like particles, tracers hold still while the game is paused. */
        g_tracer_movement_pass(manager, frame);
    }
}

static u8 THISCALL tracer_update_scaled(void *tracer, u32 frame) {
    float *speed = (float *)((u8 *)tracer + TRACER_INSTANCE_SPEED);
    float retail_speed = *speed;
    u8 result;

    *speed = retail_speed * (float)g_retail_visual_fps / (float)g_rate;
    result = g_original_tracer_update(tracer, frame);
    *speed = retail_speed;
    return result;
}

/*
 * Colour flashes (the white selection flash, and the general tint) are
 * TintEnvelopes that Drawable::updateDrawable steps once per client frame:
 * attack/decay add a per-step colour rate, sustain counts steps down. At
 * 90 FPS the selection flash ran 3x too fast. During attack and decay the
 * rates are scaled by 30/targetFPS for the call (their end tests compare
 * against the rate lengths, so they still end within one smaller step), and
 * the sustain countdown only advances when the 30 Hz frame does.
 */
typedef void(THISCALL *TintEnvelopeUpdateFn)(void *envelope);
static TintEnvelopeUpdateFn g_original_tint_envelope_update;

static BOOL retail_frame_advanced(void) {
    void *game_client = *(void **)game_address(
        g_game, g_game->drawable_physics.game_client_pointer);
    u32 frame;

    if (game_client == NULL) return TRUE;
    frame = read_client_frame(game_client);
    sync_retail_frame(frame);
    return g_retail_advanced;
}

static void THISCALL update_tint_envelope_scaled(void *envelope) {
    u8 *bytes = (u8 *)envelope;
    float *rates = (float *)(bytes + TINT_ENVELOPE_RATES);
    float saved[TINT_ENVELOPE_RATE_FLOATS];
    float scale;
    u32 i;

    if (g_config->target_fps <= g_retail_visual_fps) {
        g_original_tint_envelope_update(envelope);
        return;
    }
    switch (bytes[TINT_ENVELOPE_STATE]) {
    case TINT_ENVELOPE_SUSTAIN:
        if (retail_frame_advanced()) g_original_tint_envelope_update(envelope);
        return;
    case TINT_ENVELOPE_ATTACK:
    case TINT_ENVELOPE_DECAY:
        scale = (float)g_retail_visual_fps / (float)g_rate;
        for (i = 0; i < TINT_ENVELOPE_RATE_FLOATS; ++i) {
            saved[i] = rates[i];
            rates[i] *= scale;
        }
        g_original_tint_envelope_update(envelope);
        memcpy(rates, saved, sizeof(saved));
        return;
    default:
        g_original_tint_envelope_update(envelope);
        return;
    }
}

/*
 * Trails gain one point per 30 Hz step (the head particle's new position is
 * pushed onto the back of the trail), so their front grew in 30 Hz jumps.
 * While a trail system is drawn, its newest point is moved from the previous
 * point toward its real position by the elapsed fraction of the step, so the
 * front segment grows continuously. Points are restored after the draw.
 */
typedef struct StlDequeIterator {
    u8 *cur;
    u8 *first;
    u8 *last;
    u8 **node;
} StlDequeIterator;

static u32 trail_point_count(const u8 *record) {
    const StlDequeIterator *start = (const StlDequeIterator *)record;
    const StlDequeIterator *finish = (const StlDequeIterator *)(record + sizeof(StlDequeIterator));
    i32 count = (i32)(finish->node - start->node) * TRAIL_POINTS_PER_BLOCK +
                (i32)(finish->cur - finish->first) / TRAIL_POINT_SIZE -
                TRAIL_POINTS_PER_BLOCK +
                (i32)(start->last - start->cur) / TRAIL_POINT_SIZE;
    return count > 0 ? (u32)count : 0u;
}

static float *trail_point(const u8 *record, u32 index) {
    const StlDequeIterator *start = (const StlDequeIterator *)record;
    i32 offset = (i32)index + (i32)(start->cur - start->first) / TRAIL_POINT_SIZE;
    i32 block = offset >= 0 ? offset / TRAIL_POINTS_PER_BLOCK
                            : -((-offset - 1) / TRAIL_POINTS_PER_BLOCK) - 1;
    return (float *)(start->node[block] +
                     (offset - block * TRAIL_POINTS_PER_BLOCK) * TRAIL_POINT_SIZE);
}

/*
 * The tail moves in 30 Hz steps too: each step a live trail gains a point and
 * the draw trims it back to TrailLife points, and a dead (or stopping) trail
 * loses its oldest point. Short trails such as repair-drone sparks
 * (TrailLife 3) therefore jumped as a whole. During the draw TrailLife is
 * raised by one, so the point about to be dropped is still there, and the
 * oldest point slides toward the next one by the elapsed fraction. Live
 * trails keep their visible length; the head only grows while alive.
 */
enum { TRAIL_COLOUR_RESTORE_MAX = 2 * 4096 };
static u32 *g_trail_colour_restore_field[TRAIL_COLOUR_RESTORE_MAX];
static u32 g_trail_colour_restore_value[TRAIL_COLOUR_RESTORE_MAX];

static u32 blend_packed_colour(u32 from, u32 to, float alpha) {
    u32 result = 0;
    u32 shift;

    for (shift = 0; shift < 32u; shift += 8u) {
        float a = (float)((from >> shift) & 0xFFu);
        float b = (float)((to >> shift) & 0xFFu);
        result |= (u32)(a + (b - a) * alpha + 0.5f) << shift;
    }
    return result;
}

/* Trail point: position, width, packed colour (TRAIL_POINT_SIZE bytes). */
typedef struct TrailPoint {
    float position[3];
    float width;
    u32 colour;
} TrailPoint;
_Static_assert(sizeof(TrailPoint) == TRAIL_POINT_SIZE, "trail point layout");

static void blend_trail_point(TrailPoint *point, const TrailPoint *from,
                              const TrailPoint *toward, float alpha,
                              u32 *saved, u32 *colours_saved) {
    float *fields[4];
    float from_values[4];
    float toward_values[4];
    u32 k;

    for (k = 0; k < 3; ++k) {
        fields[k] = &point->position[k];
        from_values[k] = from->position[k];
        toward_values[k] = toward->position[k];
    }
    fields[3] = &point->width;
    from_values[3] = from->width;
    toward_values[3] = toward->width;
    for (k = 0; k < 4; ++k) {
        g_particle_restore_field[*saved] = fields[k];
        g_particle_restore_value[*saved] = *fields[k];
        ++*saved;
        *fields[k] = from_values[k] + (toward_values[k] - from_values[k]) * alpha;
    }
    /* Colours are packed integers, so they are saved and blended as such. */
    g_trail_colour_restore_field[*colours_saved] = &point->colour;
    g_trail_colour_restore_value[*colours_saved] = point->colour;
    ++*colours_saved;
    point->colour = blend_packed_colour(from->colour, toward->colour, alpha);
}

/*
 * When a trail's particle dies, the step that kills it still adds the final
 * head point; afterwards the trail drains one point per step and is no longer
 * trimmed. Telling "died this step" from "draining" (and whether the trail
 * still holds the extra point kept for the live tail) needs history, so each
 * trail particle is tracked by pointer. Pool reuse is caught because a live
 * sighting clears the dead state.
 */
enum {
    TRAIL_HISTORY_SIZE = 4096, /* power of two */
    TRAIL_HISTORY_PROBES = 16
};

typedef struct TrailHistoryEntry {
    u8 *particle;
    u32 seen_tick;
    u32 death_tick;
    BOOL dead;
    /* Live: window is full (holds the extra point). Dead: still holds it. */
    BOOL full;
    BOOL extra;
} TrailHistoryEntry;

static TrailHistoryEntry g_trail_history[TRAIL_HISTORY_SIZE];

/* Snapshot of one trail's visible points while they are re-placed. */
enum { TRAIL_CURVE_MAX = 256 };
static TrailPoint g_trail_curve[TRAIL_CURVE_MAX];

static TrailHistoryEntry *track_trail(u8 *particle, BOOL alive, u32 window, u32 retail_life) {
    u32 hash = (u32)(((uintptr_t)particle >> 3) * 2654435761u);
    u32 tick = g_particle_sim_tick;
    TrailHistoryEntry *entry = NULL;
    TrailHistoryEntry *reusable = NULL;
    u32 probe;

    if (particle == NULL) return NULL;
    for (probe = 0; probe < TRAIL_HISTORY_PROBES; ++probe) {
        TrailHistoryEntry *candidate =
            &g_trail_history[(hash + probe) & (TRAIL_HISTORY_SIZE - 1u)];
        if (candidate->particle == particle) {
            entry = candidate;
            break;
        }
        if (reusable == NULL &&
            (candidate->particle == NULL || tick - candidate->seen_tick > 2u)) {
            reusable = candidate;
        }
    }
    if (entry == NULL) {
        if (reusable == NULL) return NULL;
        entry = reusable;
        memset(entry, 0, sizeof(*entry));
        entry->particle = particle;
        /* First seen already dead: only a full trail's death step adds a point. */
        if (!alive) {
            entry->dead = TRUE;
            entry->extra = window == retail_life + 2u;
            entry->death_tick = entry->extra ? tick : tick - 1u;
        }
    }
    if (alive) {
        entry->dead = FALSE;
        entry->full = window == retail_life + 1u;
    } else if (!entry->dead) {
        entry->dead = TRUE;
        entry->death_tick = tick;
        entry->extra = entry->full;
    }
    entry->seen_tick = tick;
    return entry;
}

/*
 * Diagnostic (debug_trail_draws > 0): one line per trail-system draw with
 * the client frame, 30 Hz step and blend fraction, the system's emitter
 * position (tells systems apart), and for up to four trails: point count,
 * whether the particle is alive, remaining life, and the head before/after
 * blending.
 */
static u32 g_trail_draws_logged;

static void log_trail_draw(const u8 *system, float alpha, u32 retail_life,
                           BOOL stopping, u32 records, const char *details) {
    void *game_client = *(void **)game_address(
        g_game, g_game->drawable_physics.game_client_pointer);
    const float *position = (const float *)(system + 0x14);

    log_printf("trail frame=%u tick=%u a=%.3f sys=%p at=(%.1f,%.1f,%.1f) life=%u "
               "stop=%d recs=%u%s",
               game_client != NULL ? read_client_frame(game_client) : 0u,
               g_particle_sim_tick, alpha, (const void *)system, position[0],
               position[1], position[2], retail_life, stopping ? 1 : 0, records, details);
}

static int THISCALL draw_trails_interpolated(void *module, void *a, void *b, void *c) {
    ParticleDrawFn original = g_original_particle_draw[PARTICLE_DRAW_TRAIL];
    u8 *system = *(u8 **)((u8 *)module + PARTICLE_DRAW_MODULE_SYSTEM);
    u8 *module_data = *(u8 **)((u8 *)module + TRAIL_DRAW_MODULE_DATA);
    u8 *storage;
    u8 **particles;
    u8 *record;
    u8 *end;
    u32 *trail_life;
    u32 retail_life;
    BOOL stopping;
    float alpha;
    u32 saved = 0;
    u32 colours_saved = 0;
    u32 index;
    int result;
    BOOL logging;
    char details[512];
    size_t details_length = 0;

    if (system == NULL || module_data == NULL ||
        g_config->target_fps <= g_retail_visual_fps) {
        return original(module, a, b, c);
    }
    storage = *(u8 **)(system + PARTICLE_SYSTEM_STORAGE);
    if (storage == NULL) return original(module, a, b, c);

    alpha = g_particle_blend_fraction;
    trail_life = (u32 *)(module_data + TRAIL_DATA_LIFE);
    retail_life = *trail_life;
    stopping = system[PARTICLE_SYSTEM_STOPPING] != 0;
    particles = *(u8 ***)(storage + TRAIL_STORAGE_PARTICLES);
    record = *(u8 **)(storage + TRAIL_STORAGE_RECORDS_BEGIN);
    end = *(u8 **)(storage + TRAIL_STORAGE_RECORDS_END);
    logging = g_config->debug_trail_draws != 0 &&
              g_trail_draws_logged < g_config->debug_trail_draws &&
              record != NULL && record < end;
    details[0] = '\0';
    for (index = 0; record != NULL && record < end && particles != NULL;
         record += TRAIL_RECORD_SIZE, ++index) {
        u8 *particle = particles[index];
        u32 count = trail_point_count(record);
        BOOL alive = particle != NULL && *(u32 *)(particle + PARTICLE_LIFE_REMAINING) != 0;
        TrailHistoryEntry *history;
        u32 drawn_first = 0;
        u32 drawn;
        u32 first;
        u32 points;
        float start;
        float finish;
        BOOL tail_slides;
        BOOL head_grows;
        u32 j;

        /* Live trails beyond TrailLife + 1 points are trimmed by the draw. */
        if (alive && count > retail_life + 1u) drawn_first = count - (retail_life + 1u);
        drawn = count - drawn_first;
        history = track_trail(particle, alive, drawn, retail_life);
        if (drawn < 2u) continue;
        /*
         * A dead trail that was full still holds the point the stock trim
         * would have removed; it is drawn but not part of the visible curve.
         */
        first = drawn_first;
        if (!alive && history != NULL && history->extra) ++first;
        points = count - first;
        if (points > TRAIL_CURVE_MAX) continue;
        if (saved + drawn * 4u > PARTICLE_RESTORE_MAX ||
            colours_saved + drawn > TRAIL_COLOUR_RESTORE_MAX) {
            break;
        }
        /* The oldest point goes next step: trimmed (live, full) or drained. */
        tail_slides = !alive || stopping || drawn == retail_life + 1u;
        head_grows = !stopping &&
                     (alive || (history != NULL && history->death_tick == g_particle_sim_tick));

        /*
         * Visible curve, in 30 Hz steps along the stored points: the tail
         * slides off its oldest point and the head grows into its newest by
         * the elapsed fraction. Every drawn point is re-placed evenly along
         * it, because the stock draw maps the texture by point index; moving
         * only the ends made the texture jump once per step.
         */
        for (j = 0; j < points; ++j) {
            memcpy(&g_trail_curve[j], trail_point(record, first + j), sizeof(TrailPoint));
        }
        start = tail_slides && points >= 2u ? alpha : 0.0f;
        finish = (float)(points - 1u);
        if (head_grows && points >= 2u) finish -= 1.0f - alpha;
        if (finish < start) finish = start;
        for (j = 0; j < drawn; ++j) {
            float u = start + (finish - start) * (float)j / (float)(drawn - 1u);
            u32 k = (u32)u;
            TrailPoint *point = (TrailPoint *)trail_point(record, drawn_first + j);
            if (points < 2u) {
                blend_trail_point(point, &g_trail_curve[0], &g_trail_curve[0], 0.0f,
                                  &saved, &colours_saved);
                continue;
            }
            if (k > points - 2u) k = points - 2u;
            blend_trail_point(point, &g_trail_curve[k], &g_trail_curve[k + 1u], u - (float)k,
                              &saved, &colours_saved);
        }
        if (logging && index < 4u && details_length < sizeof(details) - 128u) {
            const TrailPoint *head = (const TrailPoint *)trail_point(record, count - 1u);
            int written = snprintf(
                details + details_length, sizeof(details) - details_length,
                " | n=%u live=%d l=%u u=%.2f..%.2f head=(%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)",
                count, alive ? 1 : 0,
                particle != NULL ? *(u32 *)(particle + PARTICLE_LIFE_REMAINING) : 0u,
                start, finish, g_trail_curve[points - 1u].position[0],
                g_trail_curve[points - 1u].position[1], g_trail_curve[points - 1u].position[2],
                head->position[0], head->position[1], head->position[2]);
            if (written > 0) details_length += (size_t)written;
        }
    }
    if (logging) {
        log_trail_draw(system, alpha, retail_life, stopping,
                       (u32)((end - *(u8 **)(storage + TRAIL_STORAGE_RECORDS_BEGIN)) /
                             TRAIL_RECORD_SIZE),
                       details);
        ++g_trail_draws_logged;
    }

    *trail_life = retail_life + 1u;
    result = original(module, a, b, c);
    *trail_life = retail_life;

    while (saved != 0) {
        --saved;
        *g_particle_restore_field[saved] = g_particle_restore_value[saved];
    }
    while (colours_saved != 0) {
        --colours_saved;
        *g_trail_colour_restore_field[colours_saved] = g_trail_colour_restore_value[colours_saved];
    }
    return result;
}

/*
 * Fog of war. Two CPU-side cell textures, the shroud (16-bit cells) and the
 * fog (32-bit cells), are uploaded every client frame. Each upload first runs
 * a time-based fade that moves changed cells toward their target level, but
 * its step is ftol(elapsed_ms * rate) while its timestamp advances by the
 * whole elapsed time, so short frames lose the fraction: at 90 FPS the
 * shroud fade ran ~25% slow and the fog fade (0.064 levels/ms) truncated to 0
 * in nearly every frame. Holding back the unused milliseconds keeps the
 * authored rate at any frame rate.
 *
 * Logic also writes vision changes straight into the fog cells, bypassing its
 * fade, so revealed cells popped in at the logic rate. The fog rows are
 * uploaded from a smoothed copy whose channels move toward the game's cells
 * by at most a full range per logic step (66 ms), which turns those pops
 * into ramps. The game's own cells are never modified.
 *
 * The shroud texture is ARGB4444: its fog-to-clear fade spans only a handful
 * of 4-bit steps, so even a smooth fade showed ~30 coarse steps per second.
 * It is now created as A8R8G8B8, and its rows are filled with full-precision
 * texels converted from each cell's shown level the way the game converts
 * them, falling back to the game's 16-bit cell (widened) if they disagree.
 */
typedef void(THISCALL *ShroudFadeFn)(void *texture, void *arg);
typedef void *(__cdecl *RowCopyFn)(void *destination, const void *source, size_t size);
typedef u8 *(THISCALL *TextureLockFn)(void *holder, i32 *pitch, u32 flag);
typedef void(THISCALL *TextureUnlockFn)(void *holder);

enum {
    FOG_SMOOTH_MILLISECONDS = 1000 / 15,
    /* Longer gaps (loading, alt-tab) show the game's cells directly. */
    FOG_SNAP_MILLISECONDS = 250,
    FOG_MAX_DIMENSION = 4096
};

static ShroudFadeFn g_original_shroud_fades[SHROUD_TEXTURE_KINDS];
static RowCopyFn g_game_memcpy;
static TextureLockFn g_texture_lock;
static TextureUnlockFn g_texture_unlock;
/* Set by the shroud fade wrapper for the row copies of the same upload. */
static const u16 *g_shroud_cells;
static const u8 *g_shroud_shown_levels;
static u32 g_shroud_cell_count;
static u32 g_shroud_texels[256];
/* Set by the fog fade wrapper for the row copies of the same upload. */
static const u8 *g_fog_cells;
static u32 g_fog_cell_count;
static BOOL g_fog_snap;
static i32 g_fog_max_step;
static u32 *g_fog_smoothed;
static u32 g_fog_smoothed_capacity;
static const u8 *g_fog_smoothed_source;
static u32 g_fog_smoothed_width;
static u32 g_fog_smoothed_height;
static LARGE_INTEGER g_fog_previous_time;
static LARGE_INTEGER g_fog_frequency;

static void run_shroud_fade(u32 kind, void *texture, void *arg) {
    const ShroudFadeSite *site = &g_game->shroud.fades[kind];
    u32 *stamp = (u32 *)game_address(g_game, site->timestamp);
    float rate = *(const float *)game_address(g_game, site->rate);
    u32 before = *stamp;
    u32 elapsed;
    double exact;
    u32 step;
    u32 unused;

    g_original_shroud_fades[kind](texture, arg);
    elapsed = *stamp - before;
    /* The first call initialises the stamp; long gaps hit the step cap anyway. */
    if (before == 0 || elapsed == 0 || elapsed > 1000u) return;
    /* Same product as the game's x87 FMUL; exact in double for these inputs. */
    exact = (double)elapsed * (double)rate;
    step = (u32)exact;
    if (step >= site->step_cap) return;
    unused = (u32)((exact - (double)step) / (double)rate + 1e-6);
    if (unused > elapsed) unused = elapsed;
    *stamp -= unused;
}

static u8 *shroud_global_data(void) {
    return *(u8 **)game_address(g_game, g_game->timing.global_data_pointer);
}

/* The game's level -> ARGB4444 conversion, kept at 8 bits per channel. */
static u32 convert_shroud_level(const u8 *global_data, u32 level) {
    const ShroudLayout *shroud = &g_game->shroud;
    u32 channels[3];
    i32 alpha = (i32)level - 4;
    u32 i;

    if (level == 0xFFu) return 0xFFFFFFFFu;
    for (i = 0; i < 3; ++i) {
        float scale = *(const float *)(global_data + shroud->global_colour_scale[i]);
        /* ftol truncation; the 4-bit packing keeps only the low byte. */
        channels[i] = (u32)(i32)((double)level * (double)scale) & 0xFFu;
    }
    alpha = alpha > 0 ? alpha * 255 / 46 : 0;
    if (alpha > 255) alpha = 255;
    return (u32)alpha << 24 | channels[0] << 16 | channels[1] << 8 | channels[2];
}

static u16 pack_shroud_texel(u32 texel) {
    return (u16)((texel >> 16 & 0xF000u) | (texel >> 12 & 0x0F00u) |
                 (texel >> 8 & 0x00F0u) | (texel >> 4 & 0x000Fu));
}

static u32 widen_shroud_cell(u32 cell) {
    return ((cell >> 12 & 0xFu) * 0x11u) << 24 | ((cell >> 8 & 0xFu) * 0x11u) << 16 |
           ((cell >> 4 & 0xFu) * 0x11u) << 8 | (cell & 0xFu) * 0x11u;
}

static void refresh_shroud_texels(void) {
    const u8 *global_data = shroud_global_data();
    u32 minimum;
    u32 level;

    if (global_data == NULL) return;
    minimum = global_data[g_game->shroud.global_minimum_level];
    /* Cells are converted from the level clamped to GlobalData's minimum. */
    for (level = 0; level < 256u; ++level) {
        g_shroud_texels[level] =
            convert_shroud_level(global_data, level < minimum ? minimum : level);
    }
}

static void THISCALL fade_shroud_cells(void *texture, void *arg) {
    const u8 *shroud = (const u8 *)texture;
    u32 width = *(const u32 *)(shroud + SHROUD_WIDTH);
    u32 height = *(const u32 *)(shroud + SHROUD_HEIGHT);

    run_shroud_fade(SHROUD_TEXTURE_SHROUD, texture, arg);
    refresh_shroud_texels();
    g_shroud_cells = *(const u16 *const *)(shroud + SHROUD_CELLS);
    g_shroud_shown_levels = *(const u8 *const *)(shroud + SHROUD_SHOWN_LEVELS);
    g_shroud_cell_count = width <= FOG_MAX_DIMENSION && height <= FOG_MAX_DIMENSION
                              ? width * height : 0;
}

/* Replaces memcpy(dst, cells + row, width * 2) for the A8R8G8B8 texture. */
static void *__cdecl copy_shroud_row_precise(void *destination, const void *source,
                                             size_t size) {
    const u16 *cells = (const u16 *)source;
    u32 *texels = (u32 *)destination;
    u32 count = (u32)size / sizeof(u16);
    u32 first = 0;
    BOOL levels_known = g_shroud_cells != NULL && g_shroud_shown_levels != NULL &&
                        cells >= g_shroud_cells;
    u32 i;

    if (levels_known) {
        first = (u32)(cells - g_shroud_cells);
        levels_known = first <= g_shroud_cell_count && count <= g_shroud_cell_count - first;
    }
    for (i = 0; i < count; ++i) {
        u32 texel = widen_shroud_cell(cells[i]);
        if (levels_known) {
            u32 precise = g_shroud_texels[g_shroud_shown_levels[first + i]];
            /* Only trust the shown level if it explains the game's cell. */
            if (pack_shroud_texel(precise) == cells[i]) texel = precise;
        }
        texels[i] = texel;
    }
    return destination;
}

/* Replaces the whole-texture fill (border included) for the A8R8G8B8 texture. */
static void THISCALL fill_shroud_texture(void *texture, u32 level, void *holder) {
    const u8 *shroud = (const u8 *)texture;
    i32 width = *(const i32 *)(shroud + SHROUD_TEXTURE_WIDTH);
    i32 height = *(const i32 *)(shroud + SHROUD_TEXTURE_HEIGHT);
    i32 pitch = 0;
    u8 *bits;
    u32 texel;
    i32 x;
    i32 y;

    refresh_shroud_texels();
    texel = g_shroud_texels[level & 0xFFu];
    bits = g_texture_lock(holder, &pitch, 0);
    if (bits != NULL) {
        for (y = 0; y < height; ++y, bits += pitch) {
            for (x = 0; x < width; ++x) ((u32 *)bits)[x] = texel;
        }
    }
    g_texture_unlock(holder);
}

static void prepare_fog_smoothing(const u8 *texture) {
    const u8 *cells = *(const u8 *const *)(texture + SHROUD_CELLS);
    u32 width = *(const u32 *)(texture + SHROUD_WIDTH);
    u32 height = *(const u32 *)(texture + SHROUD_HEIGHT);
    LARGE_INTEGER now;
    double elapsed_ms;

    g_fog_cells = NULL;
    if (cells == NULL || width == 0 || height == 0 ||
        width > FOG_MAX_DIMENSION || height > FOG_MAX_DIMENSION) {
        return;
    }
    if (width * height > g_fog_smoothed_capacity) {
        u32 *grown = (u32 *)HeapAlloc(GetProcessHeap(), 0, width * height * sizeof(u32));
        if (grown == NULL) return;
        if (g_fog_smoothed != NULL) HeapFree(GetProcessHeap(), 0, g_fog_smoothed);
        g_fog_smoothed = grown;
        g_fog_smoothed_capacity = width * height;
        g_fog_smoothed_source = NULL;
    }

    if (g_fog_frequency.QuadPart == 0) QueryPerformanceFrequency(&g_fog_frequency);
    QueryPerformanceCounter(&now);
    elapsed_ms = (double)(now.QuadPart - g_fog_previous_time.QuadPart) * 1000.0 /
                 (double)g_fog_frequency.QuadPart;
    g_fog_previous_time = now;

    /* A new map (or a different grid) starts from the game's cells. */
    g_fog_snap = cells != g_fog_smoothed_source || width != g_fog_smoothed_width ||
                 height != g_fog_smoothed_height || elapsed_ms > FOG_SNAP_MILLISECONDS ||
                 g_config->target_fps <= g_retail_visual_fps;
    g_fog_max_step = (i32)(255.0 * elapsed_ms / FOG_SMOOTH_MILLISECONDS + 0.5);
    if (g_fog_max_step < 1) g_fog_max_step = 1;
    if (g_fog_max_step >= 255) g_fog_snap = TRUE;

    g_fog_smoothed_width = width;
    g_fog_smoothed_height = height;
    g_fog_cells = cells;
    g_fog_cell_count = width * height;
}

static void THISCALL fade_fog_cells(void *texture, void *arg) {
    run_shroud_fade(SHROUD_TEXTURE_FOG, texture, arg);
    prepare_fog_smoothing((const u8 *)texture);
}

static u32 approach_fog_cell(u32 shown, u32 target, i32 max_step) {
    u32 result = 0;
    u32 shift;

    if (shown == target) return shown;
    for (shift = 0; shift < 32u; shift += 8u) {
        i32 from = (i32)((shown >> shift) & 0xFFu);
        i32 to = (i32)((target >> shift) & 0xFFu);
        if (to > from + max_step) {
            from += max_step;
        } else if (to < from - max_step) {
            from -= max_step;
        } else {
            from = to;
        }
        result |= (u32)from << shift;
    }
    return result;
}

static void *__cdecl copy_fog_row_smoothed(void *destination, const void *source,
                                         size_t size) {
    const u8 *row = (const u8 *)source;
    const u32 *cells;
    u32 *smoothed;
    u32 first;
    u32 count;
    u32 i;

    if (g_fog_cells == NULL || row < g_fog_cells || (size & 3u) != 0 ||
        ((u32)(row - g_fog_cells) & 3u) != 0) {
        return g_game_memcpy(destination, source, size);
    }
    first = (u32)(row - g_fog_cells) / sizeof(u32);
    count = (u32)size / sizeof(u32);
    if (first > g_fog_cell_count || count > g_fog_cell_count - first) {
        return g_game_memcpy(destination, source, size);
    }

    /* The upload copies every row, so the copy is complete once row 0 is seen. */
    if (first == 0) g_fog_smoothed_source = g_fog_cells;
    cells = (const u32 *)row;
    smoothed = g_fog_smoothed + first;
    for (i = 0; i < count; ++i) {
        smoothed[i] = g_fog_snap ? cells[i]
                                 : approach_fog_cell(smoothed[i], cells[i], g_fog_max_step);
    }
    return g_game_memcpy(destination, smoothed, size);
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

/*
 * With adaptive pacing, the limiter keeps an absolute schedule: a frame that
 * overran its interval (a heavy logic phase) leaves a debt that the next
 * frames repay by waiting less, so the game keeps its real-time speed
 * whenever the second as a whole fits. The stock limiter waits the full
 * interval after every frame and never recovers lost time. Debt is capped at
 * 250 ms and loading stalls are ignored, so a hitch never fast-forwards long.
 */
enum { LIMITER_DEBT_MAX_MS = 250 };
static u32 g_limiter_debt_ms;

static u32 repay_limiter_debt(u32 interval) {
    u32 stamp;
    u32 work;
    u32 repay;

    if (!g_config->adaptive_fps || g_config->target_fps <= g_retail_visual_fps) return interval;
    stamp = *(volatile u32 *)game_address(g_game, g_game->pacing.previous_frame_time_ms);
    work = (u32)timeGetTime() - stamp;
    if (work > LIMITER_DEBT_MAX_MS) return interval;
    if (work > interval) {
        g_limiter_debt_ms += work - interval;
        if (g_limiter_debt_ms > LIMITER_DEBT_MAX_MS) g_limiter_debt_ms = LIMITER_DEBT_MAX_MS;
        return interval;
    }
    repay = interval - work;
    if (repay > g_limiter_debt_ms) repay = g_limiter_debt_ms;
    g_limiter_debt_ms -= repay;
    interval -= repay;
    return interval != 0u ? interval : 1u;
}

static u32 STDCALL get_retail_limiter_interval(void *engine_pointer) {
    const PacingLayout *pacing;
    i32 signed_multiplier;
    u32 multiplier;
    u32 divisor;
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

    /*
     * Divide in fifteenths of a client frame so fractional rates (e.g. 160 FPS
     * = 10 2/3 frames per logic tick) get their exact average interval. The
     * multiplier is the truncated ratio we set; adaptive pacing (multiplayer)
     * may lower it, and then it is used as is. Integer ratios are unchanged.
     */
    divisor = multiplier == client_frames_per_tick() ? g_rate : multiplier * LOGIC_FPS;
    if (g_limiter_interval_divisor != divisor) {
        if (g_limiter_interval_divisor == 0u) {
            g_limiter_interval_remainder = 0u;
        } else {
            /* Retain the same fractional phase when adaptive pacing changes. */
            g_limiter_interval_remainder =
                (g_limiter_interval_remainder * divisor +
                 g_limiter_interval_divisor / 2u) /
                g_limiter_interval_divisor;
        }
        g_limiter_interval_divisor = divisor;
    }

    scaled = g_limiter_interval_remainder + logic_cycle_budget_ms * LOGIC_FPS;
    interval = scaled / divisor;
    g_limiter_interval_remainder = scaled % divisor;
    if (interval == 0u) interval = 1u;
    log_frame_time_sample((u8 *)engine_pointer, interval, multiplier);
    profiler_note_frame(g_rate);
    monitor_frame_pacing(interval);
    return repay_limiter_debt(interval);
}

/*
 * Audio request admission, Limit accounting, playback completion, and queue
 * cleanup all run inside the manager's client update. Retain their retail
 * 30 Hz interleaving while the rest of the client advances at the configured
 * rate. ComputeClientFrameDeltaMilliseconds observes the skipped client-frame
 * span, so authored millisecond delays still advance by the correct amount.
 */
static u8 THISCALL update_audio_at_retail_rate(void *audio_manager) {
    u8 result = 0;

    if (g_original_audio_update == NULL) return 0;
    if (g_config == NULL || g_config->target_fps <= g_retail_visual_fps) {
        return g_original_audio_update(audio_manager);
    }
    /* Below 30 FPS (adaptive pacing) a frame services audio more than once. */
    g_audio_update_accumulator += g_retail_visual_fps;
    while (g_audio_update_accumulator >= g_rate) {
        LARGE_INTEGER start;

        g_audio_update_accumulator -= g_rate;
        LONG zone = profile_enter(PROFILE_ZONE_EFFECTS);

        QueryPerformanceCounter(&start);
        result = g_original_audio_update(audio_manager);
        g_frame_effects_us += microseconds_since(&start);
        profile_leave(zone);
    }
    return result;
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

/*
 * A camera ease-to-object controller integrates "progress += speed;
 * speed += accel (up to limit)" once per client frame. With dt = 30/targetFPS
 * the retail motion is kept by scaling each accel by dt^2 and each limit by
 * dt; the stock constants are only read by this controller.
 */
static float g_camera_ease_limits[2];
static float g_camera_ease_accels[2];
static float g_camera_ease_retail_limits[2];
static float g_camera_ease_retail_accels[2];

static void retime_camera_ease(void) {
    float dt = (float)g_retail_visual_fps / (float)g_rate;
    u32 i;

    for (i = 0; i < 2; ++i) {
        g_camera_ease_limits[i] = g_camera_ease_retail_limits[i] * dt;
        g_camera_ease_accels[i] = g_camera_ease_retail_accels[i] * dt * dt;
    }
}

static void install_camera_ease_timing(PatchInstaller *installer) {
    static const char *const limit_names[2] = {
        "camera ease phase A speed limit operand", "camera ease phase B speed limit operand"
    };
    static const char *const accel_names[2] = {
        "camera ease phase A acceleration operand", "camera ease phase B acceleration operand"
    };
    const CameraEaseLayout *ease = &g_game->camera_ease;
    u32 i;

    for (i = 0; i < 2; ++i) {
        float *retail_limit = (float *)(uintptr_t)load_u32(
            game_address(g_game, ease->limit_operands[i]));
        float *retail_accel = (float *)(uintptr_t)load_u32(
            game_address(g_game, ease->accel_operands[i]));
        g_camera_ease_retail_limits[i] = *retail_limit;
        g_camera_ease_retail_accels[i] = *retail_accel;
        retime_camera_ease();
        patch_pointer(installer, limit_names[i], ease->limit_operands[i],
                      retail_limit, &g_camera_ease_limits[i]);
        patch_pointer(installer, accel_names[i], ease->accel_operands[i],
                      retail_accel, &g_camera_ease_accels[i]);
    }
}

static void install_tracer_motion(PatchInstaller *installer) {
    const TracerLayout *tracers = &g_game->tracers;
    u8 jump_patch[5] = { 0xE9, 0, 0, 0, 0 };
    u8 call_patch[5] = { 0xE8, 0, 0, 0, 0 };

    g_original_tracer_manager_update = (TracerManagerUpdateFn)game_address(
        g_game, tracers->manager_update_function);
    g_tracer_movement_pass = (TracerMovementPassFn)game_address(
        g_game, tracers->movement_pass_function);
    g_original_tracer_update = (TracerUpdateFn)game_address(
        g_game, tracers->tracer_update_call.target);

    encode_rel32(&jump_patch[1],
                 game_address(g_game, tracers->manager_update_jump.instruction + 5u),
                 tracer_manager_update_every_frame);
    patch_branch(installer, "tracer manager update jump",
                 &tracers->manager_update_jump, 0xE9, jump_patch, sizeof(jump_patch));
    encode_rel32(&call_patch[1],
                 game_address(g_game, tracers->tracer_update_call.instruction + 5u),
                 tracer_update_scaled);
    patch_branch(installer, "tracer update call",
                 &tracers->tracer_update_call, 0xE8, call_patch, sizeof(call_patch));
}

/*
 * Queued-order ("waypoint") lines, shown with Alt, start at the unit. The
 * game reads that start from the Object's logic position, which only moves
 * when logic updates it, while the unit itself is drawn at its interpolated
 * position every frame; the line's start stepped behind the unit. During the
 * line build the Object's position is temporarily the Drawable's
 * interpolated position, then restored, so logic never sees it.
 */
typedef void(THISCALL *WaypointLineBuildFn)(void *buffer, u8 *object, u32 context);
typedef const float *(THISCALL *InterpolatedTransformFn)(u8 *drawable);
static WaypointLineBuildFn g_original_waypoint_line_build;
static InterpolatedTransformFn g_interpolated_transform;

static void THISCALL build_waypoint_line_from_drawn_unit(void *buffer, u8 *object,
                                                        u32 context) {
    const WaypointLineLayout *lines = &g_game->waypoint_lines;
    u8 *drawable;
    float *position;
    float saved[3];
    const float *transform;

    if (object == NULL || g_config->target_fps <= g_retail_visual_fps) {
        g_original_waypoint_line_build(buffer, object, context);
        return;
    }
    drawable = *(u8 **)(object + lines->object_drawable);
    if (drawable == NULL || *(u8 **)(drawable + lines->drawable_object) != object) {
        g_original_waypoint_line_build(buffer, object, context);
        return;
    }
    transform = g_interpolated_transform(drawable);
    position = (float *)(object + lines->object_position);
    memcpy(saved, position, sizeof(saved));
    /* Matrix3D rows: translation is the fourth float of each. */
    position[0] = transform[3];
    position[1] = transform[7];
    position[2] = transform[11];
    g_original_waypoint_line_build(buffer, object, context);
    memcpy(position, saved, sizeof(saved));
}

static void install_waypoint_lines(PatchInstaller *installer) {
    const WaypointLineLayout *lines = &g_game->waypoint_lines;
    u8 call_patch[5] = { 0xE8, 0, 0, 0, 0 };

    g_original_waypoint_line_build =
        (WaypointLineBuildFn)game_address(g_game, lines->line_call.target);
    g_interpolated_transform = (InterpolatedTransformFn)game_address(
        g_game, lines->interpolated_transform_function);
    encode_rel32(&call_patch[1], game_address(g_game, lines->line_call.instruction + 5u),
                 build_waypoint_line_from_drawn_unit);
    patch_branch(installer, "waypoint line build call", &lines->line_call, 0xE8, call_patch,
                 sizeof(call_patch));
}

/* Milliseconds from the high-resolution counter. */
static double precise_now_ms(void) {
    LARGE_INTEGER now;

    if (g_logic_timer_frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&g_logic_timer_frequency);
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)g_logic_timer_frequency.QuadPart;
}

/*
 * B14: summoned objects fading in. An ObjectCreationList CreateObject with
 * FADE_IN / FADE_OUT (e.g. the Scrin Mothership, FadeTime 15 s) converts its
 * FadeTime to logic frames and passes them to Drawable::fadeIn/fadeOut, which
 * count client frames: at 30 FPS 15 s became the familiar ~7.5 s fade, at
 * 156 FPS about 1.4 s. These calls now get the frame count the same fade
 * takes at the live rate (x rate / 30); the fade still advances every frame.
 */
typedef void(THISCALL *DrawableFadeFn)(void *drawable, u32 frames);
static DrawableFadeFn g_original_fade_in;
static DrawableFadeFn g_original_fade_out;

static u32 retail_frames_at_live_rate(u32 frames) {
    if (g_config->target_fps <= g_retail_visual_fps || frames == 0u) return frames;
    return (u32)(((u64)frames * g_rate + g_retail_visual_fps / 2u) / g_retail_visual_fps);
}

static void THISCALL summon_fade_in(void *drawable, u32 frames) {
    g_original_fade_in(drawable, retail_frames_at_live_rate(frames));
}

static void THISCALL summon_fade_out(void *drawable, u32 frames) {
    g_original_fade_out(drawable, retail_frames_at_live_rate(frames));
}

/*
 * B15: Drawable pulse tints (flag 0x20; e.g. the yellow highlight of the
 * Tiberium Vibration Scan). Their attack/decay/sustain frame counts are
 * computed with the live client FPS (seconds x clientFPS), but the
 * TintEnvelope fix (B9) treats envelope frame counts as retail 30 Hz frames,
 * like the game's other envelopes (e.g. 30-frame flashes): the scan's
 * highlight lasted ~5x too long. The pulse's counts are converted to 30 Hz
 * frames as it starts. Its heartbeat (colour x sin(logicFrame x f)) moved
 * only with the logic frame; it now uses a logic frame that advances smoothly
 * between logic updates (one update behind, pausing with the logic).
 */
typedef void(THISCALL *TintEnvelopePlayFn)(void *envelope, const float *colour, u32 attack,
                                           u32 decay, u32 sustain);
static TintEnvelopePlayFn g_original_tint_pulse_play;

enum { TINT_PULSE_AMPLITUDE = 0x3C, TINT_PULSE_FREQUENCY = 0x40, TINT_PULSE_OUTPUT = 0x44,
       TINT_ENVELOPE_COLOUR = 0x28 };

static u32 live_frames_to_retail(u32 frames, float scale) {
    float converted;

    if (frames == 0u || frames >= 0x7FFFFFFFu) return frames;
    converted = (float)frames * scale + 0.5f;
    return converted < 1.0f ? 1u : (u32)converted;
}

static void THISCALL play_tint_pulse_in_retail_frames(void *envelope, const float *colour,
                                                     u32 attack, u32 decay, u32 sustain) {
    float live_fps = *(float *)game_address(g_game, g_game->timing.client_fps_float);
    float scale;

    if (g_config->target_fps <= g_retail_visual_fps || !(live_fps > 0.0f)) {
        g_original_tint_pulse_play(envelope, colour, attack, decay, sustain);
        return;
    }
    scale = (float)g_retail_visual_fps / live_fps;
    g_original_tint_pulse_play(envelope, colour, live_frames_to_retail(attack, scale),
                               live_frames_to_retail(decay, scale),
                               live_frames_to_retail(sustain, scale));
}

/*
 * The pulse's caller reuses EDX (the colour pointer) after the call, as the
 * stock TintEnvelope::play leaves it untouched; C code may not. This stub
 * keeps EDX across the wrapper (ECX = envelope, four stack arguments).
 */
__attribute__((naked)) static void play_tint_pulse_entry(void) {
    __asm__ volatile(
        "pushl %%edx\n\t"
        "pushl 20(%%esp)\n\t"
        "pushl 20(%%esp)\n\t"
        "pushl 20(%%esp)\n\t"
        "pushl 20(%%esp)\n\t"
        "calll %c0\n\t"
        "popl %%edx\n\t"
        "retl $16"
        :
        : "i"(play_tint_pulse_in_retail_frames));
}

/* Logic frame advancing smoothly between updates, one update behind. */
static u32 g_pulse_logic_frame;
static double g_pulse_frame_ms;
static double g_pulse_tick_ms = 1000.0 / 15.0;
static BOOL g_pulse_frame_known;

static double smooth_logic_frame(u32 frame) {
    double now = precise_now_ms();
    double progress;

    if (!g_pulse_frame_known || frame != g_pulse_logic_frame) {
        if (g_pulse_frame_known && frame == g_pulse_logic_frame + 1u) {
            double interval = now - g_pulse_frame_ms;
            if (interval < 20.0) interval = 20.0;
            if (interval > 200.0) interval = 200.0;
            g_pulse_tick_ms = interval;
        }
        g_pulse_logic_frame = frame;
        g_pulse_frame_ms = now;
        g_pulse_frame_known = TRUE;
    }
    progress = (now - g_pulse_frame_ms) / g_pulse_tick_ms;
    if (progress > 1.0) progress = 1.0;
    return (double)frame - 1.0 + progress;
}

static float *THISCALL tint_pulse_colour(u8 *envelope) {
    const EffectTimingLayout *fx = &g_game->effect_timing;
    float amplitude = *(float *)(envelope + TINT_PULSE_AMPLITUDE);
    float frequency = *(float *)(envelope + TINT_PULSE_FREQUENCY);
    const float *colour = (const float *)(envelope + TINT_ENVELOPE_COLOUR);
    float *output = (float *)(envelope + TINT_PULSE_OUTPUT);
    u8 *logic;
    u32 frame;
    double time;
    float factor;

    if (frequency == 0.0f || amplitude == 0.0f) return (float *)(envelope + TINT_ENVELOPE_COLOUR);
    logic = *(u8 **)game_address(g_game, fx->game_logic_pointer);
    frame = logic != NULL ? *(u32 *)(logic + fx->logic_frame_offset) : 0u;
    time = g_config->target_fps <= g_retail_visual_fps ? (double)frame : smooth_logic_frame(frame);
    factor = (float)(0.5 + sin(time * (double)frequency) * (double)amplitude * 0.5);
    output[0] = colour[0] * factor;
    output[1] = colour[1] * factor;
    output[2] = colour[2] * factor;
    return output;
}

static void install_effect_timing(PatchInstaller *installer) {
    static const char *const fade_names[SUMMON_FADE_CALLS] = {
        "summon fade call 1", "summon fade call 2", "summon fade call 3"
    };
    static const char *const colour_names[TINT_PULSE_COLOR_CALLS] = {
        "tint pulse colour call 1", "tint pulse colour call 2"
    };
    const EffectTimingLayout *fx = &g_game->effect_timing;
    u8 call_patch[5] = { 0xE8, 0, 0, 0, 0 };
    u32 i;

    g_original_fade_in = (DrawableFadeFn)game_address(g_game, fx->fade_in_function);
    g_original_fade_out = (DrawableFadeFn)game_address(g_game, fx->fade_out_function);
    for (i = 0; i < SUMMON_FADE_CALLS; ++i) {
        const BranchSite *site = &fx->summon_fade_calls[i];
        encode_rel32(&call_patch[1], game_address(g_game, site->instruction + 5u),
                     site->target == fx->fade_in_function ? (void *)summon_fade_in
                                                          : (void *)summon_fade_out);
        patch_branch(installer, fade_names[i], site, 0xE8, call_patch, sizeof(call_patch));
    }

    g_original_tint_pulse_play =
        (TintEnvelopePlayFn)game_address(g_game, fx->pulse_play_call.target);
    encode_rel32(&call_patch[1], game_address(g_game, fx->pulse_play_call.instruction + 5u),
                 play_tint_pulse_entry);
    patch_branch(installer, "tint pulse play call", &fx->pulse_play_call, 0xE8, call_patch,
                 sizeof(call_patch));

    for (i = 0; i < TINT_PULSE_COLOR_CALLS; ++i) {
        encode_rel32(&call_patch[1],
                     game_address(g_game, fx->pulse_color_calls[i].instruction + 5u),
                     tint_pulse_colour);
        patch_branch(installer, colour_names[i], &fx->pulse_color_calls[i], 0xE8, call_patch,
                     sizeof(call_patch));
    }
}

/*
 * src_multicore M1: the particle manager's "add system" scan is replaced by
 * an index (particle_index.c). The scan's first 7 bytes become CALL stub;
 * JMP past the scan. The list's insert CALL and the manager's three erase
 * CALLs go through wrappers that keep the index exact.
 */
static void install_particle_index(PatchInstaller *installer) {
    static const char *const erase_names[PARTICLE_ERASE_CALLS] = {
        "particle erase call 1", "particle erase call 2", "particle erase call 3"
    };
    const ParticleListLayout *list = &g_game->particle_list;
    ParticleIndexSetup setup;
    u8 call_patch[5] = { 0xE8, 0, 0, 0, 0 };
    u8 scan_patch[7] = { 0xE8, 0, 0, 0, 0, 0xEB, 0 };
    u8 expected[7];
    u32 i;

    setup.list_offset = list->list_offset;
    setup.key_offset = list->key_offset;
    setup.insert = (ParticleListInsertFn)game_address(g_game, list->insert_call.target);
    setup.erase = (ParticleListEraseFn)game_address(g_game, list->erase_function);
    setup.enabled = g_config->multicore_particle_index;
    setup.verify = g_config->multicore_verify;
    particle_index_setup(&setup);

    memcpy(expected, game_address(g_game, list->scan_start), sizeof(expected));
    /* MOV ESI,[EAX+key]; LEA ECX,[EDI+list]: the resolver matched these. */
    if (expected[0] != 0x8B || expected[1] != 0xB0) {
        installer->ok = FALSE;
        return;
    }
    encode_rel32(&scan_patch[1], game_address(g_game, list->scan_start + 5u),
                 particle_index_scan_stub);
    scan_patch[6] = (u8)(list->scan_length - sizeof(scan_patch));
    patch_bytes(installer, "particle list scan", list->scan_start, expected, scan_patch,
                sizeof(scan_patch));

    encode_rel32(&call_patch[1], game_address(g_game, list->insert_call.instruction + 5u),
                 particle_index_insert);
    patch_branch(installer, "particle list insert call", &list->insert_call, 0xE8, call_patch,
                 sizeof(call_patch));
    for (i = 0; i < PARTICLE_ERASE_CALLS; ++i) {
        encode_rel32(&call_patch[1],
                     game_address(g_game, list->erase_calls[i].instruction + 5u),
                     particle_index_erase);
        patch_branch(installer, erase_names[i], &list->erase_calls[i], 0xE8, call_patch,
                     sizeof(call_patch));
    }

    /* M1b: the first-draw sibling search starts at the key's group and stops after it. */
    if (g_config->multicore_particle_index) {
        static const u8 setup_original[9] = { 0x8B, 0x03, 0x57, 0x8B, 0x38, 0x3B, 0xF8, 0x74, 0x54 };
        static const u8 mismatch_original[1] = { 0x27 };
        static const u8 mismatch_patch[1] = { 0x3B };
        u8 setup_patch[9] = { 0x57, 0xE8, 0, 0, 0, 0, 0x74, 0x55, 0x90 };

        encode_rel32(&setup_patch[2], game_address(g_game, list->sibling_setup + 6u),
                     particle_index_sibling_stub);
        patch_bytes(installer, "particle sibling search setup", list->sibling_setup,
                    setup_original, setup_patch, sizeof(setup_patch));
        patch_bytes(installer, "particle sibling search exit", list->sibling_mismatch,
                    mismatch_original, mismatch_patch, sizeof(mismatch_patch));
    }
}

/*
 * src_multicore M3a: the trail vertex builder's read-back of V from the
 * vertex buffer becomes a jump to a stub that recomputes V (render_fixes.c).
 */
static void install_trail_vertex_fix(PatchInstaller *installer) {
    static const u8 readback[7] = { 0xD9, 0x41, 0x04, 0xD9, 0x5C, 0x30, 0x04 };
    u32 rva = g_game->trail_vertices.readback;
    u8 jump[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };

    if (!g_config->multicore_trail_fix) return;
    trail_vertex_set_resume(game_address(g_game, rva + sizeof(readback)));
    encode_rel32(&jump[1], game_address(g_game, rva + 5u), trail_vertex_v_stub);
    patch_bytes(installer, "trail vertex read-back", rva, readback, jump, sizeof(jump));
}

/* The frame-boundary test follows our schedules (see is_client_frame_boundary). */
static void install_frame_boundary(PatchInstaller *installer) {
    u32 rva = g_game->frame_boundary.function;
    u8 expected[5];
    u8 jump[5] = { 0xE9, 0, 0, 0, 0 };

    memcpy(expected, game_address(g_game, rva), sizeof(expected));
    if (expected[0] != 0xA1) {
        installer->ok = FALSE;
        return;
    }
    encode_rel32(&jump[1], game_address(g_game, rva + 5u), is_client_frame_boundary_entry);
    patch_bytes(installer, "IsClientFrameBoundary entry", rva, expected, jump, sizeof(jump));
}

static void install_minimap_glide(PatchInstaller *installer) {
    static const char *const names[MINIMAP_LOOK_AT_MAX][MINIMAP_LOOK_AT_CALLS] = {
        { "minimap look-at call 1", "minimap look-at call 2" },
        { "minimap look-at call 3", "minimap look-at call 4" }
    };
    static void *const wrappers[MINIMAP_LOOK_AT_MAX][MINIMAP_LOOK_AT_CALLS] = {
        { (void *)minimap_click_first, (void *)minimap_move_first },
        { (void *)minimap_click_second, (void *)minimap_move_second }
    };
    const MinimapLayout *minimap = &g_game->minimap;
    u8 call_patch[5] = { 0xE8, 0, 0, 0, 0 };
    u32 copy;
    u32 i;

    for (copy = 0; copy < minimap->count; ++copy) {
        g_minimap_convert[copy] =
            (MinimapConvertFn)game_address(g_game, minimap->converters[copy]);
        for (i = 0; i < MINIMAP_LOOK_AT_CALLS; ++i) {
            const BranchSite *call = &minimap->calls[copy][i];
            encode_rel32(&call_patch[1], game_address(g_game, call->instruction + 5u),
                         wrappers[copy][i]);
            patch_branch(installer, names[copy][i], call, 0xE8, call_patch,
                         sizeof(call_patch));
        }
    }
}

static void install_tint_envelope_timing(PatchInstaller *installer) {
    static const char *const names[TINT_ENVELOPE_UPDATE_CALLS] = {
        "tint envelope update branch 1", "tint envelope update branch 2"
    };
    const TintEnvelopeLayout *tint = &g_game->tint_envelope;
    /* Repeated flashes every half second: retail 15 frames, now FPS / 2. */
    u32 retail_interval = g_retail_visual_fps / 2u;
    u32 interval = g_config->target_fps / 2u;
    u32 i;

    patch_bytes(installer, "repeated flash interval", tint->flash_interval,
                &retail_interval, &interval, sizeof(interval));
    g_original_tint_envelope_update =
        (TintEnvelopeUpdateFn)game_address(g_game, tint->update_function);
    for (i = 0; i < TINT_ENVELOPE_UPDATE_CALLS; ++i) {
        /* One CALL and one tail JMP; keep each site's opcode. */
        u8 opcode = *game_address(g_game, tint->update_calls[i].instruction);
        u8 branch_patch[5] = { opcode, 0, 0, 0, 0 };
        encode_rel32(&branch_patch[1],
                     game_address(g_game, tint->update_calls[i].instruction + 5u),
                     update_tint_envelope_scaled);
        patch_branch(installer, names[i], &tint->update_calls[i], opcode, branch_patch,
                     sizeof(branch_patch));
    }
}

static void install_shroud_timing(PatchInstaller *installer) {
    static const char *const fade_names[SHROUD_TEXTURE_KINDS] = {
        "shroud texture fade call", "fog texture fade call"
    };
    const ShroudLayout *shroud = &g_game->shroud;
    const struct {
        const char *name;
        const BranchSite *call;
        void *replacement;
    } calls[] = {
        { fade_names[SHROUD_TEXTURE_SHROUD], &shroud->fades[SHROUD_TEXTURE_SHROUD].fade_call,
          (void *)fade_shroud_cells },
        { fade_names[SHROUD_TEXTURE_FOG], &shroud->fades[SHROUD_TEXTURE_FOG].fade_call,
          (void *)fade_fog_cells },
        { "fog texture row copy call", &shroud->fog_row_copy_call,
          (void *)copy_fog_row_smoothed },
        { "shroud texture row copy call", &shroud->shroud_row_copy_call,
          (void *)copy_shroud_row_precise },
        { "shroud texture fill call", &shroud->shroud_fill_call,
          (void *)fill_shroud_texture },
    };
    static const u8 retail_format = 0x1A;  /* D3DFMT_A4R4G4B4 */
    static const u8 precise_format = 0x15; /* D3DFMT_A8R8G8B8 */
    u8 call_patch[5] = { 0xE8, 0, 0, 0, 0 };
    u32 kind;
    u32 i;

    for (kind = 0; kind < SHROUD_TEXTURE_KINDS; ++kind) {
        g_original_shroud_fades[kind] =
            (ShroudFadeFn)game_address(g_game, shroud->fades[kind].fade_call.target);
    }
    g_game_memcpy = (RowCopyFn)game_address(g_game, shroud->fog_row_copy_call.target);
    g_texture_lock = (TextureLockFn)game_address(g_game, shroud->texture_lock_function);
    g_texture_unlock = (TextureUnlockFn)game_address(g_game, shroud->texture_unlock_function);

    for (i = 0; i < ARRAY_COUNT(calls); ++i) {
        encode_rel32(&call_patch[1], game_address(g_game, calls[i].call->instruction + 5u),
                     calls[i].replacement);
        patch_branch(installer, calls[i].name, calls[i].call, 0xE8, call_patch,
                     sizeof(call_patch));
    }
    patch_bytes(installer, "shroud texture format", shroud->shroud_format_immediate,
                &retail_format, &precise_format, sizeof(precise_format));
}

/*
 * Adaptive client rate. The patch ties game speed to frame counts (frames per
 * logic tick, particle/audio steps per frame, W3D milliseconds per frame...),
 * so if the PC cannot hold the configured rate, the whole game slowed down.
 *
 * CnC3 is CPU-bound on one thread, and a frame's work is its drawing plus
 * whatever logic phases it ran. Logic costs the same per second at any client
 * rate (15 ticks of 6 phases); only drawing scales with the rate. So the
 * monitor records each frame's work (time since the limiter's previous wait
 * ended, the stock stamp), its period, and the time spent in logic dispatches
 * and in the 30 Hz particle/audio updates (timed directly). Over the last two
 * seconds it derives the fixed cost per second (logic and effects, the same
 * at any rate) and the drawing cost per frame. The highest rate whose second
 * still fits is (budget - fixed) / drawing. It drops when the rate doesn't
 * fit and the game really runs slow (frames under 97% of the rate) and rises
 * after a second of load under 75%. When even the minimum rate can't hold
 * full speed, it drops no lower than the highest rate losing at most 5%
 * speed against the best reachable, so a near-useless crawl to 5 FPS is
 * avoided. A rise that fails within 3 s leaves a ceiling that is
 * retried gently; a drop whose per-frame time shrinks far more than the
 * rate (the old rate waited on an outside cap, e.g. a driver frame limit or
 * the GPU) leaves a lasting ceiling 4% below that rate. Rate changes apply
 * at the start of a logic tick and re-time every rate-dependent value.
 */
enum {
    MIN_ADAPTIVE_FPS = 5,
    RATE_RAISE_MS = 1000,         /* low load needed this long before rising */
    PACING_SETTLE_MS = 300,       /* frames ignored after a switch */
    FAILED_RAISE_MS = 3000,       /* a drop this soon after a rise: the rise failed */
    CEILING_RETRY_MS = 10000,     /* first retry above a failed rate */
    CEILING_RETRY_MAX_MS = 60000,
    PACING_STABLE_MS = 60000,     /* this long without failures resets the retry */
    PACING_WINDOW_MAX = 512,      /* frames kept (over two seconds at 240 FPS) */
    PACING_WINDOW_MIN = 8,        /* frames needed before judging */
    PACING_WINDOW_MS = 2000,      /* real time judged */
    PACING_SKIP_MS = 250,
    PACING_STATUS_MS = 10000,     /* status line interval */
    PHASES_PER_SECOND = LOGIC_FPS * LOGIC_PHASE_COUNT
};

typedef struct PacingSample {
    u32 work_ms;
    u32 period_ms;
    u32 phases;
    u32 logic_us;
    u32 effects_us;
    u32 effects_steps;
} PacingSample;

static PacingSample g_pacing_samples[PACING_WINDOW_MAX];
static u32 g_pacing_sample_next;
static u32 g_pacing_sample_count;
static u32 g_pacing_headroom_frames;
static u32 g_pacing_settle_frames;
static u32 g_pacing_frames_since_raise;
static u32 g_pacing_rate_before_raise;
/* Rate a rise could not hold: stay at or below it until the retry is due. */
static u32 g_pacing_ceiling;
static u32 g_pacing_ceiling_frames;
static u32 g_pacing_retry_ms = CEILING_RETRY_MS;
static u32 g_pacing_previous_call_ms;
/* Ceiling below an outside frame limit (driver cap, GPU) this session; 0 = none. */
static u32 g_pacing_outside_ceiling;
enum { OUTSIDE_LIMIT_MIN_FPS = 60 };
/* The rate and per-frame drawing time before the last drop. */
static u32 g_pacing_drop_from;
static double g_pacing_drop_drawing;
static u32 g_pacing_epoch_ms;
static u32 g_pacing_next_status_ms;

static void clear_pacing_window(void) {
    g_pacing_sample_next = 0;
    g_pacing_sample_count = 0;
}

static void reset_pacing_monitor(void) {
    clear_pacing_window();
    g_pacing_headroom_frames = 0;
    g_pacing_settle_frames = 0;
    g_pacing_frames_since_raise = 0x40000000u;
    g_pacing_rate_before_raise = 0;
    g_pacing_ceiling = 0;
    g_pacing_ceiling_frames = 0;
    g_pacing_retry_ms = CEILING_RETRY_MS;
    g_pacing_previous_call_ms = 0;
    g_pacing_drop_from = 0;
    g_frame_phases = 0;
    g_frame_logic_us = 0;
    g_frame_effects_us = 0;
    g_frame_effects_steps = 0;
}

static u32 frames_for_ms(u32 ms, u32 rate) {
    return (u32)((u64)ms * rate / 1000u);
}

static void switch_pacing_rate(u32 rate) {
    g_rate_pending = rate;
    g_pacing_headroom_frames = 0;
    clear_pacing_window();
    g_pacing_settle_frames = frames_for_ms(PACING_SETTLE_MS, rate);
    if (g_pacing_settle_frames < 4u) g_pacing_settle_frames = 4u;
}

/* Highest supported rate not above `fps`. */
static u32 supported_rate_at_most(double fps) {
    u32 target = g_config->target_fps;
    u32 rate;

    if (fps >= (double)target) return target;
    /* Any whole rate (15..90 and above: our scheduler; below 15: catch-up). */
    rate = fps > 0.0 ? (u32)fps : 0u;
    if (rate < MIN_ADAPTIVE_FPS) rate = MIN_ADAPTIVE_FPS;
    if (rate > target) rate = target;
    return rate;
}

/* Highest rate whose second (fixed costs plus drawing) fits `fraction` of it. */
static double rate_fitting(double fraction, double drawing_ms, double fixed_ms) {
    double free_ms = 1000.0 * fraction - fixed_ms;
    if (free_ms <= 0.0) return 0.0;
    return free_ms / drawing_ms;
}

/*
 * Called once per limited frame, right before the limiter waits. The work of
 * this frame is the time since the stock stamp of the previous wait's end,
 * exactly what the limiter itself compares with the interval.
 */
static void monitor_frame_pacing(u32 interval_ms) {
    u32 now = (u32)timeGetTime();
    u32 stamp = *(volatile u32 *)game_address(g_game, g_game->pacing.previous_frame_time_ms);
    u32 work_ms = now - stamp;
    u32 period_ms = now - g_pacing_previous_call_ms;
    u32 phases = g_frame_phases;
    u32 logic_us = g_frame_logic_us;
    u32 effects_us = g_frame_effects_us;
    u32 effects_steps = g_frame_effects_steps;
    u32 window;
    u32 i;
    u32 seconds;
    double n;
    double sum_work = 0.0, sum_period = 0.0, sum_phases = 0.0, sum_logic_ms = 0.0;
    double sum_effects_ms = 0.0, sum_effects_steps = 0.0;
    double ticks;
    double logic_per_second;
    double effects_per_second;
    double fixed;
    double drawing;
    double load;
    double speed;
    double tick_speed;
    double best_speed;
    double fit_rate;
    u32 floor_rate;
    u32 target_rate;

    (void)interval_ms;
    g_frame_phases = 0;
    g_frame_logic_us = 0;
    g_frame_effects_us = 0;
    g_frame_effects_steps = 0;
    if (!g_config->adaptive_fps || g_config->target_fps <= g_retail_visual_fps) return;
    if (g_pacing_epoch_ms == 0u) {
        g_pacing_epoch_ms = now;
        g_pacing_next_status_ms = now + PACING_STATUS_MS;
    }
    g_pacing_previous_call_ms = now;
    seconds = (now - g_pacing_epoch_ms) / 1000u;
    /* Loading stalls say nothing about the frame rate the game can hold. */
    if (work_ms > PACING_SKIP_MS || period_ms > PACING_SKIP_MS) return;
    /* After a switch, let the new schedule settle before judging again. */
    if (g_pacing_settle_frames != 0u) {
        --g_pacing_settle_frames;
        return;
    }
    if (g_pacing_ceiling_frames != 0u && --g_pacing_ceiling_frames == 0u) {
        /* Probe gently above the failed rate: 15% (at least one step) per retry. */
        u32 next = supported_rate_at_most((double)g_pacing_ceiling * 1.15);
        if (next <= g_pacing_ceiling) next = supported_rate_at_most((double)g_pacing_ceiling + LOGIC_FPS);
        if (next >= g_config->target_fps) {
            g_pacing_ceiling = 0;
        } else {
            g_pacing_ceiling = next;
            g_pacing_ceiling_frames = frames_for_ms(g_pacing_retry_ms, next);
        }
    }
    g_pacing_samples[g_pacing_sample_next].work_ms = work_ms;
    g_pacing_samples[g_pacing_sample_next].period_ms = period_ms;
    g_pacing_samples[g_pacing_sample_next].phases = phases;
    g_pacing_samples[g_pacing_sample_next].logic_us = logic_us;
    g_pacing_samples[g_pacing_sample_next].effects_us = effects_us;
    g_pacing_samples[g_pacing_sample_next].effects_steps = effects_steps;
    g_pacing_sample_next = (g_pacing_sample_next + 1u) % PACING_WINDOW_MAX;
    if (g_pacing_sample_count < PACING_WINDOW_MAX) ++g_pacing_sample_count;
    ++g_pacing_frames_since_raise;

    /* Judge the last two seconds of real time, once one second is in. */
    for (i = 0; i < g_pacing_sample_count && sum_period < PACING_WINDOW_MS; ++i) {
        const PacingSample *sample = &g_pacing_samples[
            (g_pacing_sample_next + PACING_WINDOW_MAX - 1u - i) % PACING_WINDOW_MAX];
        sum_work += (double)sample->work_ms;
        sum_period += (double)sample->period_ms;
        sum_phases += (double)sample->phases;
        sum_logic_ms += (double)sample->logic_us / 1000.0;
        sum_effects_ms += (double)sample->effects_us / 1000.0;
        sum_effects_steps += (double)sample->effects_steps;
    }
    window = i;
    if (window < PACING_WINDOW_MIN || sum_period < PACING_WINDOW_MS / 2u) return;
    n = (double)window;
    /* Fixed costs per second, measured directly (0 while paused). */
    ticks = sum_phases / LOGIC_PHASE_COUNT;
    logic_per_second = ticks >= 1.0 ? sum_logic_ms / ticks * LOGIC_FPS : 0.0;
    effects_per_second = sum_effects_steps >= 1.0
                             ? sum_effects_ms / sum_effects_steps * g_retail_visual_fps
                             : 0.0;
    fixed = logic_per_second + effects_per_second;
    drawing = (sum_work - sum_logic_ms - sum_effects_ms) / n;
    if (drawing < 0.3) drawing = 0.3;
    /* Share of each second this rate uses (outside waits included). */
    load = sum_work / n * (double)g_rate / 1000.0;
    /* Real frame rate against the rate (the limiter aims ~1% fast). */
    speed = n * 1000.0 / (double)g_rate / sum_period;
    /* Logic ticks per real second against 15 (stalls and pauses lower it). */
    tick_speed = ticks / LOGIC_FPS * 1000.0 / sum_period;

    if ((i32)(now - g_pacing_next_status_ms) >= 0) {
        static u32 status_count;
        g_pacing_next_status_ms = now + PACING_STATUS_MS;
        if (++status_count % 6u == 0u) {
            ParticleIndexStats stats = particle_index_take_stats();
            log_printf("Particle index [%us]: finds %u, hits %u, misses %u, rebuilds %u, "
                       "sibling searches %u, verify mismatches %u, split groups %u, keys %u",
                       seconds, stats.finds, stats.hits, stats.misses, stats.rebuilds,
                       stats.sibling_searches, stats.verify_mismatches, stats.split_groups,
                       stats.entries);
        }
        log_printf("Adaptive pacing [%us]: %u FPS, frames %.0f%%, game ticks %.0f%%, "
                   "logic %.0f ms/s, effects %.0f ms/s, drawing %.2f ms/frame, load %.0f%%",
                   seconds, g_rate, speed * 100.0, tick_speed * 100.0, logic_per_second,
                   effects_per_second, drawing, load * 100.0);
    }

    /*
     * First judgement after a drop: if the old rate's per-frame time was far
     * above this one's, the old rate waited on something outside the game
     * (a driver frame cap, or the GPU). Stay 4% below it for the session.
     * Only high rates are judged: at low rates a heavy battle's per-frame
     * time swings by itself (it once pinned the rate at 7 FPS).
     */
    if (g_pacing_drop_from != 0u) {
        if (g_rate < g_pacing_drop_from && g_pacing_drop_from >= OUTSIDE_LIMIT_MIN_FPS &&
            g_pacing_drop_drawing > 1.3 * drawing + 0.5) {
            u32 ceiling = supported_rate_at_most((double)g_pacing_drop_from * 0.96);
            if (ceiling >= g_rate &&
                (g_pacing_outside_ceiling == 0u || ceiling < g_pacing_outside_ceiling)) {
                g_pacing_outside_ceiling = ceiling;
                log_printf("Adaptive pacing [%us]: %u FPS took %.2f ms/frame against %.2f "
                           "here; an outside limit (driver frame cap or GPU) holds near "
                           "%u FPS, staying at or below %u",
                           seconds, g_pacing_drop_from, g_pacing_drop_drawing, drawing,
                           g_pacing_drop_from, ceiling);
            }
        }
        g_pacing_drop_from = 0;
    }

    /*
     * Drop target: the rate whose second fits 90%. If none does, the rate
     * that still fits 98%. If even the minimum rate can't hold full speed
     * (logic or effects fill the CPU), the highest rate losing at most 5%
     * speed against the best reachable: a crawl to 5 FPS only when it buys
     * the speed back. Drops never go below that.
     */
    floor_rate = MIN_ADAPTIVE_FPS;
    fit_rate = rate_fitting(0.90, drawing, fixed);
    if (fit_rate < MIN_ADAPTIVE_FPS) fit_rate = rate_fitting(0.98, drawing, fixed);
    if (fit_rate < MIN_ADAPTIVE_FPS) {
        best_speed = 1000.0 / (fixed + MIN_ADAPTIVE_FPS * drawing);
        if (best_speed > 1.0) best_speed = 1.0;
        fit_rate = best_speed > 0.1
                       ? (1000.0 / (best_speed - 0.05) - fixed) / drawing
                       : (double)MIN_ADAPTIVE_FPS;
        floor_rate = supported_rate_at_most(fit_rate);
    }

    /*
     * Drop only when the rate doesn't fit (load over 100%) and frames really
     * arrive slow over two seconds: an outside wait such as a driver frame
     * cap raises the load but not the slowness, and short bursts are repaid
     * by the limiter. Never more than halve the rate in one step, and never
     * below the floor.
     */
    if (load > 1.00 && speed < 0.97) {
        target_rate = supported_rate_at_most(fit_rate);
        if (target_rate < g_rate / 2u) {
            target_rate = supported_rate_at_most((double)g_rate / 2.0);
        }
        if (target_rate < floor_rate) target_rate = floor_rate;
        if (g_pacing_outside_ceiling != 0u && target_rate > g_pacing_outside_ceiling) {
            target_rate = g_pacing_outside_ceiling;
        }
        if (target_rate < g_rate) {
            if (g_pacing_frames_since_raise < frames_for_ms(FAILED_RAISE_MS, g_rate) &&
                g_pacing_rate_before_raise != 0u && g_pacing_rate_before_raise < g_rate) {
                /* The last rise could not hold: go back and cap rises there. */
                g_pacing_ceiling = g_pacing_rate_before_raise > target_rate
                                       ? g_pacing_rate_before_raise : target_rate;
                g_pacing_ceiling_frames = frames_for_ms(g_pacing_retry_ms, g_pacing_ceiling);
                if (g_pacing_retry_ms < CEILING_RETRY_MAX_MS) g_pacing_retry_ms *= 2u;
                if (target_rate < g_pacing_ceiling) target_rate = g_pacing_ceiling;
                g_pacing_rate_before_raise = 0;
            }
            log_printf("Adaptive pacing [%us]: frames %.0f%%, logic %.0f ms/s, effects %.0f "
                       "ms/s, drawing %.2f ms/frame, load %.0f%% at %u FPS; drop to %u",
                       seconds, speed * 100.0, logic_per_second, effects_per_second,
                       drawing, load * 100.0, g_rate, target_rate);
            g_pacing_drop_from = g_rate;
            g_pacing_drop_drawing = drawing;
            switch_pacing_rate(target_rate);
            return;
        }
    }
    /* Rise after a second of clearly low load; at most 1.5x per step. */
    target_rate = supported_rate_at_most(rate_fitting(0.85, drawing, fixed));
    if (target_rate > (u32)(g_rate * 3u / 2u)) {
        target_rate = supported_rate_at_most((double)g_rate * 1.5);
    }
    if (g_pacing_ceiling != 0u && target_rate > g_pacing_ceiling) {
        target_rate = supported_rate_at_most((double)g_pacing_ceiling);
    }
    if (g_pacing_outside_ceiling != 0u && target_rate > g_pacing_outside_ceiling) {
        target_rate = g_pacing_outside_ceiling;
    }
    if (target_rate > g_rate && load < 0.75) {
        if (++g_pacing_headroom_frames >= frames_for_ms(RATE_RAISE_MS, g_rate)) {
            log_printf("Adaptive pacing [%us]: frames %.0f%%, logic %.0f ms/s, effects %.0f "
                       "ms/s, drawing %.2f ms/frame, load %.0f%% at %u FPS; raise to %u",
                       seconds, speed * 100.0, logic_per_second, effects_per_second,
                       drawing, load * 100.0, g_rate, target_rate);
            g_pacing_rate_before_raise = g_rate;
            g_pacing_frames_since_raise = 0;
            switch_pacing_rate(target_rate);
        }
    } else {
        g_pacing_headroom_frames = 0;
        /* Long stable stretches forget earlier failed rises. */
        if (g_pacing_frames_since_raise > frames_for_ms(PACING_STABLE_MS, g_rate)) {
            g_pacing_retry_ms = CEILING_RETRY_MS;
        }
    }
}

/* Rescale a fixed-point remainder (units of 1/old) to units of 1/new. */
static u32 rescale_remainder(u32 remainder, u32 old_rate, u32 new_rate, BOOL keep_nonzero) {
    u32 scaled;

    if (old_rate == 0u) return 0u;
    scaled = (u32)((u64)remainder * new_rate / old_rate);
    if (scaled >= new_rate) scaled = new_rate - 1u;
    if (keep_nonzero && remainder != 0u && scaled == 0u) scaled = 1u;
    return scaled;
}

static void write_code_byte(u32 rva, u8 value) {
    u8 *address = game_address(g_game, rva);
    DWORD old_protect;

    if (*address == value) return;
    if (!VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &old_protect)) return;
    *address = value;
    VirtualProtect(address, 1, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), address, 1);
}

/* Re-time every value derived from the client rate. */
static void retime_client_rate(u32 rate) {
    const TimingLayout *timing = &g_game->timing;
    u8 *engine = *(u8 **)game_address(g_game, timing->game_engine_pointer);
    u8 *global_data = *(u8 **)game_address(g_game, timing->global_data_pointer);
    u32 old_rate = g_rate;
    u32 game_rate = rate < LOGIC_FPS ? LOGIC_FPS : rate;
    u8 window = (u8)(3u * ((rate + LOGIC_FPS - 1u) / LOGIC_FPS));
    u32 i;

    g_particle_update_accumulator =
        rescale_remainder(g_particle_update_accumulator, old_rate, rate, FALSE);
    g_audio_update_accumulator =
        rescale_remainder(g_audio_update_accumulator, old_rate, rate, FALSE);
    g_w3d_time_remainder = rescale_remainder(g_w3d_time_remainder, old_rate, rate, FALSE);
    g_retail_remainder = rescale_remainder(g_retail_remainder, old_rate, rate, TRUE);
    g_rate = rate;
    g_rate_pending = rate;

    select_camera_rate_constants(rate);
    g_visual_client_step = 1.0f / (float)rate;
    g_client_frames_per_millisecond = (float)rate / 1000.0f;
    retime_camera_ease();
    *(u32 *)game_address(g_game, g_game->tint_envelope.flash_interval) =
        rate / 2u > 0u ? rate / 2u : 1u;
    for (i = 0; i < 2; ++i) {
        write_code_byte(g_game->drawable_interpolation.window_immediates[i], window);
    }

    /*
     * The game's own rate globals, as written at session start. Stock code
     * divides by clientFPS / 15, so its frame rate never goes below 15; the
     * per-frame time steps use the true rate.
     */
    *(u32 *)game_address(g_game, timing->client_fps) = game_rate;
    *(float *)game_address(g_game, timing->client_fps_float) = (float)game_rate;
    *(float *)game_address(g_game, timing->audio_milliseconds_per_frame) =
        1000.0f / (float)rate;
    *(float *)game_address(g_game, timing->visual_seconds_per_frame) = 1.0f / (float)rate;
    *(u32 *)game_address(g_game, timing->w3d_milliseconds_per_frame) =
        (g_w3d_time_remainder + W3D_RETAIL_MILLISECONDS_PER_SECOND) / rate;
    if (global_data != NULL) *(u32 *)(global_data + GLOBAL_DATA_FPS_LIMIT) = game_rate;
    if (engine != NULL) {
        *(u32 *)(engine + ENGINE_MAX_UPDATE_FPS) = game_rate;
        *(u32 *)(engine + ENGINE_NOMINAL_CLIENT_FRAMES_PER_LOGIC_TICK) = game_rate / LOGIC_FPS;
        *(u32 *)(engine + ENGINE_PACING_UPDATE_MULTIPLIER) = game_rate / LOGIC_FPS;
    }
}

static void apply_pending_rate(void) {
    if (g_rate_pending == g_rate) return;
    log_u32("Adaptive pacing: effective client FPS=", g_rate_pending);
    retime_client_rate(g_rate_pending);
    apply_live_camera_settings();
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
    /* Three logic ticks of the longest tick (fractional rates alternate). */
    u8 window = (u8)(3u * ((g_config->target_fps + LOGIC_FPS - 1u) / LOGIC_FPS));
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
        static const char *const names[PARTICLE_DRAW_KINDS] = {
            "particle DefaultDrawModule draw vtable entry",
            "particle StreakDrawModule draw vtable entry",
            "particle LightningDrawModule draw vtable entry",
            "particle RenderObjectDrawModule draw vtable entry",
            "particle TrailDrawModule draw vtable entry",
            "particle SwarmDrawModule draw vtable entry"
        };
        const ParticleDrawFn wrappers[PARTICLE_DRAW_KINDS] = {
            draw_default_particles, draw_streak_particles,
            draw_lightning_particles, draw_render_object_particles,
            draw_trails_interpolated, draw_swarm_interpolated
        };
        u32 kind;
        for (kind = 0; kind < PARTICLE_DRAW_KINDS; ++kind) {
            void *draw = game_address(g_game, g_game->particle_draw.draw_functions[kind]);
            g_original_particle_draw[kind] = (ParticleDrawFn)draw;
            patch_pointer(installer, names[kind],
                          g_game->particle_draw.draw_vtable_entries[kind], draw,
                          (const void *)wrappers[kind]);
        }
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
     * the stub's RET returns directly to the leaf's original caller.
     */
    encode_rel32(&interpolation_patch[1],
                 game_address(g_game, interpolation_rva + 5u),
                 update_phase_interpolation_entry);

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

    g_original_dispatch_logic_phase = (DispatchLogicPhaseFn)game_address(
        g_game, scheduler->dispatch_function);
    patch_pointer(installer, "GameEngine dispatch vtable entry",
                  scheduler->dispatch_vtable_entries[0],
                  (const void *)g_original_dispatch_logic_phase,
                  (const void *)dispatch_logic_phase_scheduled);
    patch_pointer(installer, "Win32GameEngine dispatch vtable entry",
                  scheduler->dispatch_vtable_entries[1],
                  (const void *)g_original_dispatch_logic_phase,
                  (const void *)dispatch_logic_phase_scheduled);
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
    g_rate = config->target_fps;
    g_rate_pending = g_rate;
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
    install_tracer_motion(&installer);
    install_camera_ease_timing(&installer);
    install_shroud_timing(&installer);
    install_tint_envelope_timing(&installer);
    install_minimap_glide(&installer);
    install_waypoint_lines(&installer);
    install_effect_timing(&installer);
    install_particle_index(&installer);
    install_trail_vertex_fix(&installer);
    install_frame_boundary(&installer);
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
        g_retail_camera_adjust_speed, g_rate);
    g_applied_keyboard_rotate_speed =
        g_retail_keyboard_rotate_speed * g_camera_scroll_scale;
    *camera_adjust = g_applied_camera_adjust_speed;
    *keyboard_rotate = g_applied_keyboard_rotate_speed;
    g_camera_settings_captured = TRUE;
}

void game_patches_reset_state(void) {
    if (g_config != NULL) {
        /* Each session starts at the configured rate; pacing lowers it if needed. */
        g_rate = g_config->target_fps;
        g_rate_pending = g_rate;
        g_pacing_outside_ceiling = 0;
        reset_pacing_monitor();
        g_retail_started = FALSE;
        g_catch_up_accumulator = 0u;
        /* Seed so particle simulation runs on the first client frame of a session. */
        g_particle_update_accumulator = g_config->target_fps - g_retail_visual_fps;
        g_particle_blend_fraction = 0.0f;
        g_tick_frames = 0u;
        g_tick_frame = 0u;
        g_tick_accumulator = 0u;
        g_minimap_following = FALSE;
        g_minimap_has_shown = FALSE;
        /* Seed so audio service runs before the first logic-phase slice. */
        g_audio_update_accumulator = g_config->target_fps - g_retail_visual_fps;
        g_w3d_time_remainder = 0;
        g_limiter_interval_remainder = 0;
        g_limiter_debt_ms = 0;
        g_limiter_interval_divisor = 0;
        /* Drawables from a previous session may share addresses with new ones. */
        memset(g_physics_cache, 0, sizeof(g_physics_cache));
        /* Each session may follow a new vehicle for the motion diagnostic. */
        g_motion_drawable = NULL;
        memset(g_particle_cache, 0, sizeof(g_particle_cache));
        /* A new session's fog starts from the game's cells. */
        g_fog_smoothed_source = NULL;
        g_shroud_cells = NULL;
        g_frame_times_logged = 0;
        g_trail_draws_logged = 0;
        memset(g_trail_history, 0, sizeof(g_trail_history));
        g_pulse_frame_known = FALSE;
        particle_index_reset();
        reset_keyboard_clock();
        /* First direct reader gets floor(990/FPS); the hook distributes remainder. */
        *(u32 *)game_address(g_game, g_game->timing.w3d_milliseconds_per_frame) =
            W3D_RETAIL_MILLISECONDS_PER_SECOND / g_config->target_fps;
        retime_client_rate(g_rate);
        apply_live_camera_settings();
    }
}
