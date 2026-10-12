#ifndef FPS_PATCH_GAME_LAYOUT_H
#define FPS_PATCH_GAME_LAYOUT_H

#include "common.h"

typedef struct BranchSite {
    /* RVAs of the five-byte CALL/JMP and its decoded in-image destination. */
    u32 instruction;
    u32 target;
} BranchSite;

typedef enum RuntimeConfigHookKind {
    RUNTIME_CONFIG_HOOK_THISCALL,
    RUNTIME_CONFIG_HOOK_NOARG
} RuntimeConfigHookKind;

typedef struct BootstrapLayout {
    /* Late call whose ABI-specific wrapper performs first-time installation. */
    BranchSite runtime_config_call;
    /* Session-start tail call used to refresh mutable engine fields. */
    BranchSite start_session_tail;
    RuntimeConfigHookKind runtime_config_hook_kind;
} BootstrapLayout;

typedef struct TimingLayout {
    /* All fields are RVAs of globals or globals containing runtime pointers. */
    u32 logic_fps;
    u32 client_fps;
    u32 game_engine_pointer;
    u32 global_data_pointer;
    u32 w3d_milliseconds_per_frame;
    u32 w3d_accumulated_time_ms;
    u32 client_fps_float;
    u32 audio_milliseconds_per_frame;
    u32 visual_seconds_per_frame;
} TimingLayout;

typedef struct AudioLayout {
    /* Retail-rate subsystem update retained behind a vtable wrapper. */
    u32 update_function;
    u32 update_vtable_entry;
} AudioLayout;

typedef struct KeyboardLayout {
    /* Private frame increment immediately before the hardware-poll call. */
    u32 input_frame_increment;
    /* Original poll and repeat calls retained behind rate-converting wrappers. */
    BranchSite hardware_poll;
    BranchSite check_repeat;
} KeyboardLayout;

enum { RANGE_QUERY_CALLS = 3, RANGE_DISTANCE_TABLE_ENTRIES = 5 };

typedef struct RangeQueryLayout {
    /*
     * src_multicore M4 diagnostic. The partition manager's range query (TW
     * 1.10 VA 0x9E30ED, thiscall, 7 arguments, RET 0x1C) is called only from
     * three thin wrappers with EBP frames; its distance callbacks are a
     * 5-entry pointer table (TW 0xBDF528); the tree walk adds each accepted
     * object with one CALL (TW 0x9E2453 -> 0x9E1DE5).
     */
    u32 query_function;
    BranchSite query_calls[RANGE_QUERY_CALLS];
    u32 distance_table;
    BranchSite add_call;
} RangeQueryLayout;

enum { APT_CALL_FUNCTIONS = 5 };

typedef struct AptCallLayout {
    /*
     * Diagnostic (debug_apt_calls): the five "queue an ActionScript call"
     * variants (TW 1.10 VA 0x773A18 .. 0x773BD7, 0x773A92 among them), all
     * starting PUSH EBP; MOV EBP,ESP; SUB ESP,0x20. count is 0 when they
     * aren't found exactly (the diagnostic is then unavailable).
     */
    u32 functions[APT_CALL_FUNCTIONS];
    u32 count;
    /* Diagnostic: the "set an ActionScript variable" function (TW 1.10 0x773D27), or 0. */
    u32 variable_function;
} AptCallLayout;

typedef struct TurretFractionLayout {
    /*
     * B21: the W3D turret positioning (TW 1.10 VA 0x8580A2) loads the
     * engine's interpolation fraction (MOV EAX,[engine]; MOVSS XMM0,
     * [EAX+0x48]) and advances its per-turret angle history only when the
     * fraction is exactly 1.0. RVA of that 5-byte MOV EAX,[engine].
     */
    u32 engine_load;
    /*
     * B22: in the same function the pitch bone gets the raw logic pitch
     * (MOVSS XMM0,[-0.0]; SUBSS XMM0,[EBP-8], 13 bytes) although the pitch
     * history (cache +4 previous, +0xC current) is kept like the rotation's.
     */
    u32 pitch_site;
} TurretFractionLayout;

typedef struct ButtonEnableLayout {
    /*
     * B20: gadgets push SetEnabled to their Flash clip through a direct call
     * helper (TW 1.10 VA 0x784E58, cdecl(path, name, &bool), first
     * instruction MOV EDX,[g_aptPlayer]) that runs the ActionScript function
     * immediately. RVAs of that helper and of the "SetEnabled" literal every
     * caller pushes.
     */
    u32 helper;
    u32 set_enabled_string;
} ButtonEnableLayout;

typedef struct RecoilLayout {
    /*
     * B23: W3D draw's client recoil handler (TW 1.10 VA 0x863B46) steps
     * every active weapon-recoil bone once per drawn frame: record +0x14
     * state (1/2 kick, 3 settle, 0 idle), +0x18 shift, +0x1C rate; module
     * data +0x5C MaxRecoilDistance, +0x60 RecoilDamping, +0x64
     * RecoilSettleSpeed (per 30 Hz frame). RVA of the 7-byte
     * CMP EAX,2 / JLE / CMP EAX,3 that starts the state switch; the switch
     * ends at step_site + 0x7D (XORPS XMM0,XMM0).
     */
    u32 step_site;
} RecoilLayout;

typedef struct HistoryRefreshLayout {
    /*
     * B19: Drawable::refreshInterpolationHistory (TW 1.10 VA 0x474EAF,
     * thiscall(snap), RET 4; KW 1.03 0x4BAB25, thiscall(snap, force), RET 8).
     * Normal mode builds the Catmull-Rom points p0..p3 (+0xC4/+0xD0/+0xDC/
     * +0xE8) from the Object's previous and current positions; snap mode
     * sets them all to the current position.
     */
    u32 function;
    BOOL kane;       /* two arguments, 5-byte prologue */
} HistoryRefreshLayout;

typedef struct FrameBoundaryLayout {
    /*
     * GameEngine::IsClientFrameBoundary (TW 1.10 VA 0x548131, 8 callers):
     * ratio = clientFPS / 15; below 6 "phase == 6 / ratio", else
     * "phase == 1". For every whole ratio that is "the phase counter equals
     * the last phase of the tick's first frame". Thiscall, result in AL,
     * keeps ECX.
     */
    u32 function;
} FrameBoundaryLayout;

typedef struct SchedulerLayout {
    /* Start of the 22-byte integer batching expression in DispatchLogicPhase. */
    u32 phase_batch_block;
    /* Entry of the 48-byte __thiscall interpolation leaf function. */
    u32 phase_interpolation_function;
    /* Original sub_6DE5B1 call at byte 34 of the 39-byte phase-end block. */
    BranchSite client_slice_flush;
    /*
     * GameEngine::DispatchLogicPhase(phase) (contains the batch block) and
     * its two vtable entries (GameEngine and Win32GameEngine, slot 0x94),
     * through which GameEngine::update dispatches once per client frame.
     */
    u32 dispatch_function;
    u32 dispatch_vtable_entries[2];
} SchedulerLayout;

typedef struct W3DClockLayout {
    /* Blocks immediately before the preserved W3D_SetCurrentTimeMs calls. */
    u32 special_advance_block;
    u32 normal_advance_block;
} W3DClockLayout;

typedef struct CameraInputLayout {
    /* SAGE 3 camera controls which are evaluated once per client frame. */
    u32 w3d_view_pointer;
    u32 scroll_by_function;
    u32 zoom_in_behavior_function;
    u32 zoom_out_behavior_function;
    u32 camera_adjust_instruction;
    u32 shake_decay_operand;
    u32 retail_shake_decay;
} CameraInputLayout;

typedef struct VisualLayout {
    /* Shared retail 1/30 scalar and selected instruction operands that read it. */
    u32 retail_step;
    u32 camera_step_operand;
    u32 laser_step_operand;
    u32 model_step_operand;

    u32 tracer_reset_frame_call;
    u32 tracer_update_frame_call;
    u32 cloud_frame_call;
    u32 anim2d_timestamp_frame_call;
    u32 anim2d_update_frame_call;
    BranchSite particle_simulation;

    u32 gpu_particle_fps_instruction;
    u32 gpu_particle_fps_operand;
    u32 radius_cursor_fps_instruction;
    u32 radius_cursor_fps_operand;
    u32 retail_frames_per_millisecond;
} VisualLayout;

typedef struct DrawablePhysicsLayout {
    /*
     * Drawable::calcPhysicsXform (locomotor pitch/roll/yaw/suspension springs)
     * guards itself with "once per GameClient frame". Its frame read and the
     * GameClient global it is read from.
     */
    u32 dispatcher_frame_call;
    u32 game_client_pointer;
    /* Drawable::applyPhysicsXform's call of that dispatcher. */
    BranchSite apply_call;
} DrawablePhysicsLayout;

typedef struct DrawableInterpolationLayout {
    /*
     * The two "clientFrame - Drawable::historyFrame < 6" tests (history
     * refresh and Catmull-Rom position evaluation). Stock 6 client frames is
     * three logic ticks at 30 FPS; these are the RVAs of the 0x06 immediates.
     */
    u32 window_immediates[2];
} DrawableInterpolationLayout;

enum ParticleDrawKind {
    PARTICLE_DRAW_DEFAULT,
    PARTICLE_DRAW_STREAK,
    PARTICLE_DRAW_LIGHTNING,
    PARTICLE_DRAW_RENDER_OBJECT,
    PARTICLE_DRAW_TRAIL,
    PARTICLE_DRAW_SWARM,
    PARTICLE_DRAW_KINDS
};

typedef struct ParticleDrawLayout {
    /*
     * FXParticleSystem::{Default,Streak,Lightning,RenderObject}DrawModule::Draw
     * (vtable slot 7) for CPU-simulated particles, and their vtable slots.
     */
    u32 draw_functions[PARTICLE_DRAW_KINDS];
    u32 draw_vtable_entries[PARTICLE_DRAW_KINDS];
    /*
     * ParticleSystemManager::UpdateForClientFrame only simulates systems
     * while "GameClient byte [client offset] != 0 && GameLogic byte [logic
     * offset] == 0" (the latter is the pause flag). The GameLogic global and
     * both byte offsets.
     */
    u32 game_logic_pointer;
    u32 client_simulates_offset;
    u32 logic_paused_offset;
} ParticleDrawLayout;

typedef struct TracerLayout {
    /*
     * W3DTracerManager per-client-frame update (gated on the frame number),
     * the thunk JMP that enters it, its tracer movement/expiry pass, and the
     * pass's call of the per-tracer update.
     */
    u32 manager_update_function;
    BranchSite manager_update_jump;
    u32 movement_pass_function;
    BranchSite tracer_update_call;
} TracerLayout;

typedef struct CameraEaseLayout {
    /*
     * Camera ease-to-object state machine (two phases): each phase does
     * "progress += speed; if (speed < limit) speed += accel" once per client
     * frame. RVAs of the limit and accel MOVSS operands, per phase.
     */
    u32 limit_operands[2];
    u32 accel_operands[2];
} CameraEaseLayout;

enum ShroudTextureKind {
    SHROUD_TEXTURE_SHROUD, /* 16-bit cells, client +0x198 */
    SHROUD_TEXTURE_FOG,    /* 32-bit cells, client +0x19C */
    SHROUD_TEXTURE_KINDS
};

typedef struct ShroudFadeSite {
    /* The per-frame texture upload's call of its time-based cell fade. */
    BranchSite fade_call;
    /*
     * The fade's static last-timeGetTime stamp, its float levels-per-ms rate
     * and its per-call step cap. The step is truncated while the stamp
     * advances by the full elapsed time, so short frames lose the fraction.
     */
    u32 timestamp;
    u32 rate;
    u32 step_cap;
} ShroudFadeSite;

typedef struct ShroudLayout {
    ShroudFadeSite fades[SHROUD_TEXTURE_KINDS];
    /* The fog upload's per-row memcpy(dst, cells + row, width * 4). */
    BranchSite fog_row_copy_call;
    /*
     * The 16-bit shroud texture: the D3DFMT_A4R4G4B4 (0x1A) immediate in its
     * creation, the upload's per-row memcpy(dst, cells + row, width * 2), and
     * the upload's call of the whole-texture fill (level, lock holder), which
     * also sets the one-texel border the row copies never touch.
     */
    u32 shroud_format_immediate;
    BranchSite shroud_row_copy_call;
    BranchSite shroud_fill_call;
    /* Texture lock(holder, &pitch, flag) / unlock(holder), both __thiscall. */
    u32 texture_lock_function;
    u32 texture_unlock_function;
    /*
     * GlobalData fields read by the level -> ARGB4444 cell converter: the
     * minimum shroud level and the red/green/blue level multipliers.
     */
    u32 global_minimum_level;
    u32 global_colour_scale[3];
} ShroudLayout;

enum { TINT_ENVELOPE_UPDATE_CALLS = 2 };

typedef struct TintEnvelopeLayout {
    /*
     * TintEnvelope::update (attack/sustain/decay colour flash, one step per
     * call) and its two calls, both once per client frame from
     * Drawable::updateDrawable: the Drawable +0x190 tint and the +0x194
     * selection flash.
     */
    u32 update_function;
    BranchSite update_calls[TINT_ENVELOPE_UPDATE_CALLS];
    /*
     * Repeated-flash interval: Drawable::updateDrawable flashes again while
     * its flash count (+0x150) > 0 whenever "client frame % interval == 0".
     * The interval global is set once at startup to client FPS / 2 (15).
     */
    u32 flash_interval;
} TintEnvelopeLayout;

enum {
    MINIMAP_LOOK_AT_MAX = 2,
    MINIMAP_LOOK_AT_CALLS = 2,
    MINIMAP_CLICK_CALL = 0, /* in the drag operation's constructor */
    MINIMAP_MOVE_CALL = 1   /* in its mouse-move handler */
};

typedef struct MinimapLayout {
    /*
     * Radar "look at" (TW 1.10 VA 0x7A6325; Kane's Wrath has two radar
     * implementations): convert a radar point to a world point, then
     * view->lookAt(world) (virtual +0x48). Called on the click that starts a
     * minimap drag and on every drag move; moves arrive with the 30 Hz UI.
     */
    u32 count;
    u32 converters[MINIMAP_LOOK_AT_MAX];
    BranchSite calls[MINIMAP_LOOK_AT_MAX][MINIMAP_LOOK_AT_CALLS];
    u32 view_pointer;
} MinimapLayout;

typedef struct WaypointLineLayout {
    /*
     * Queued-order ("waypoint") lines: the per-object draw callback's call of
     * the line builder (TW 1.10 VA 0x833924 -> 0x833699), which starts the
     * line at the Object's logic position. Offsets of that position in the
     * Object, of the Object's Drawable, of the Drawable's Object back-pointer,
     * and Drawable::getInterpolatedTransform (thiscall, returns Matrix3D*).
     */
    BranchSite line_call;
    u32 object_position;
    u32 object_drawable;
    u32 drawable_object;
    u32 interpolated_transform_function;
} WaypointLineLayout;

enum { SUMMON_FADE_CALLS = 3, TINT_PULSE_COLOR_CALLS = 2 };

typedef struct EffectTimingLayout {
    /*
     * Drawable::fadeIn / fadeOut(frames) (TW 1.10 VAs 0x46E933 / 0x46E8E1):
     * opacity = elapsed / frames, elapsed counting client frames.
     */
    u32 fade_in_function;
    u32 fade_out_function;
    /*
     * ObjectCreationList CreateObject with FADE_IN / FADE_OUT (e.g. the
     * summoned Mothership, FadeTime 15 s): "PUSH [EBP+0x58]" (FadeTime in
     * logic frames); MOV ECX,[obj+Drawable]; CALL fadeIn/fadeOut. Two fade-in
     * calls and one fade-out call (TW 0x63DF48, 0x63E061, 0x63DFBA).
     */
    BranchSite summon_fade_calls[SUMMON_FADE_CALLS];
    /*
     * Drawable "pulse" tint (flag 0x20, e.g. Tiberium Vibration Scan):
     * Drawable::updateDrawable's helper plays its TintEnvelope (+0x194) with
     * TintEnvelope::play(colour*, attack, decay, sustain) (TW 0x478D38), the
     * frame counts having been computed with the live client FPS.
     */
    BranchSite pulse_play_call;
    /*
     * TintEnvelope colour getter (TW 0x46F478): with a pulse (+0x3C amplitude,
     * +0x40 frequency) it scales the colour by 0.5 + sin(logicFrame * f) *
     * a / 2, so the pulse stepped with the logic. Both calls (TW 0x472F97,
     * 0x472FD7) and its GameLogic pointer and frame offset.
     */
    u32 pulse_color_function;
    BranchSite pulse_color_calls[TINT_PULSE_COLOR_CALLS];
    u32 game_logic_pointer;
    u32 logic_frame_offset;
} EffectTimingLayout;

enum { PARTICLE_ERASE_CALLS = 3 };

typedef struct ParticleListLayout {
    /*
     * src_multicore M1. The particle-system manager keeps its systems in a
     * std::list (node: next, prev, system) at manager + list_offset. Adding
     * a system (TW 1.10 VA 0x5B7D9D, thiscall(manager, RefPtr*)) scans the
     * list for the first system with the same key (system + key_offset) and
     * inserts before it (or at the end). The scan is the 50 bytes at
     * insert_function + 26 (in: EAX = system, EDI = manager; out: EAX =
     * position node, ECX = &list). The list's only insert is the CALL at
     * insert_call; its only erase is the manager's erase(iterator*) (TW
     * 0x5B7E14), called from three places.
     */
    u32 insert_function;
    u32 scan_start;
    u32 scan_length;
    u32 resume;
    BranchSite insert_call;
    u32 erase_function;
    BranchSite erase_calls[PARTICLE_ERASE_CALLS];
    u32 list_offset;
    u32 key_offset;
    /*
     * M1b. A particle system's first draw (TW 1.10 VA 0x5BF76B) walks the
     * whole list for another system with the same key whose render data is
     * set up, to share it. `sibling_setup` is its "MOV EAX,[EBX]; PUSH EDI;
     * MOV EDI,[EAX]; CMP EDI,EAX; JZ end" (9 bytes; EBX = list, [EBP-4] =
     * key); `sibling_mismatch` is the rel8 of its "key differs: next node"
     * JNZ (0x27), which becomes 0x3B (leave the loop).
     */
    u32 sibling_setup;
    u32 sibling_mismatch;
} ParticleListLayout;

typedef struct TrailVertexLayout {
    /*
     * src_multicore M3a. The trail vertex builder (TW 1.10 VA 0x5BA171,
     * called by TrailDrawModule_Draw) writes two vertices per trail point
     * into the locked GPU vertex buffer and copies the first vertex's V
     * texture coordinate into the second by reading it back from the buffer:
     * "FLD [ECX+4]; FSTP [EAX+ESI+4]" (7 bytes at `readback`). Reading
     * GPU-visible memory is extremely slow; all of the builder's profile
     * samples landed right after this load.
     */
    u32 readback;
} TrailVertexLayout;

typedef struct SelectionPanelLayout {
    /*
     * TacticalSelectionDetailsPanelImpl::SetUnitName (forwards only changed
     * names), its vtable entry, and the helper that pushes "UnitName" to APT.
     */
    u32 set_unit_name_function;
    u32 set_unit_name_vtable_entry;
    u32 push_unit_name_function;
    /* TacticalSelectionDetailsPanelImpl::SetState (vtable slot 1). */
    u32 set_state_function;
    u32 set_state_vtable_entry;
    /* SelectionDetails::PanelImpl::SingleUnitModeHandler per-frame update. */
    u32 single_unit_update_function;
    u32 single_unit_update_vtable_entry;
} SelectionPanelLayout;

typedef struct ObjectiveUiLayout {
    /* Global AptPlayer* stored by the AptPlayer constructor. */
    u32 apt_player_pointer;
    /*
     * TacticalObjectiveIndicatorStageImpl vtable slots 0..2 and their stock
     * targets: AddObjectIndicator, AddOffScreenIndicator, AddWaypointIndicator.
     */
    u32 add_indicator_vtable_entries[3];
    u32 add_indicator_functions[3];
} ObjectiveUiLayout;

typedef struct PacingLayout {
    /* Stock 29 ms presentation wait and GameEngine limiter interval block. */
    u32 display_limiter_branch;
    u32 limiter_interval_block;
    u32 limiter_conversion_target;
    u32 network_scale;
    u32 enforce_limit_flag;
    u32 previous_frame_time_ms;
    u32 milliseconds_per_logic_frame;
} PacingLayout;

/*
 * Every address is an RVA resolved from the loaded executable. Grouping sites
 * by subsystem keeps the runtime code explicit without mirroring every field
 * through a second layer of preprocessor aliases.
 */
typedef struct GameLayout {
    u8 *module;
    const char *target_name;
    u32 pe_timestamp;
    u32 pe_entry_rva;
    u32 pe_size_of_image;
    BootstrapLayout bootstrap;
    TimingLayout timing;
    AudioLayout audio;
    KeyboardLayout keyboard;
    SchedulerLayout scheduler;
    W3DClockLayout w3d_clock;
    CameraInputLayout camera_input;
    VisualLayout visual;
    PacingLayout pacing;
    ObjectiveUiLayout objective_ui;
    DrawablePhysicsLayout drawable_physics;
    DrawableInterpolationLayout drawable_interpolation;
    SelectionPanelLayout selection_panel;
    ParticleDrawLayout particle_draw;
    TracerLayout tracers;
    CameraEaseLayout camera_ease;
    ShroudLayout shroud;
    TintEnvelopeLayout tint_envelope;
    MinimapLayout minimap;
    WaypointLineLayout waypoint_lines;
    EffectTimingLayout effect_timing;
    ParticleListLayout particle_list;
    TrailVertexLayout trail_vertices;
    FrameBoundaryLayout frame_boundary;
    RangeQueryLayout range_query;
    AptCallLayout apt_calls;
    HistoryRefreshLayout history_refresh;
    TurretFractionLayout turret_fraction;
    RecoilLayout recoil;
    ButtonEnableLayout button_enable;
} GameLayout;

typedef enum GameResolveResult {
    GAME_RESOLVED,
    GAME_INVALID_PE,
    GAME_UNSUPPORTED_BUILD
} GameResolveResult;

enum {
    /* GameEngine offsets recovered from KW 1.02 RTTI-backed class analysis. */
    ENGINE_MAX_UPDATE_FPS = 0x18,
    ENGINE_CURRENT_LOGIC_PHASE = 0x40,
    ENGINE_NOMINAL_CLIENT_FRAMES_PER_LOGIC_TICK = 0x44,
    ENGINE_LOGIC_PHASE_INTERPOLATION = 0x48,
    ENGINE_FRAME_DURATION_HISTORY_MS = 0x5C,
    ENGINE_FRAME_DURATION_HISTORY_COUNT = 64,
    ENGINE_FRAME_DURATION_HISTORY_SUM_MS = 0x15C,
    ENGINE_FRAME_DURATION_HISTORY_INDEX = 0x160,
    ENGINE_PACING_UPDATE_MULTIPLIER = 0x164,

    /* GlobalData fields reached through the resolved g_theWriteableGlobalData pointer. */
    GLOBAL_DATA_USE_FPS_LIMIT = 0x52,
    GLOBAL_DATA_FPS_LIMIT = 0x54,

    /* Camera settings consumed as fixed per-client-frame coefficients. */
    GLOBAL_DATA_CAMERA_ADJUST_SPEED = 0x0B14,
    GLOBAL_DATA_KEYBOARD_CAMERA_ROTATE_SPEED = 0x0D34,

    /* Keyboard queue/key-state layout proven by the shared repeat flow. */
    KEYBOARD_EVENT_BEGIN = 0x001C,
    KEYBOARD_EVENT_END = 0x0020,
    KEYBOARD_KEY_SEQUENCE_BASE = 0x002C,
    KEYBOARD_KEY_STATE_STRIDE = 8,
    KEYBOARD_INPUT_FRAME = 0x0E28,

    /*
     * AptPlayer flag: when set, the APT render path first runs one APT update
     * with at least 34 ms elapsed, so pending ActionScript (OnInitialized)
     * executes before the frame is drawn.
     */
    APT_PLAYER_FORCE_TICK = 0x01E9,
    /* APT gadget "OnInitialized has run" flag, e.g. in the selection panel. */
    APT_GADGET_INITIALIZED = 0x14,
    /*
     * FXParticleSystem: draw module -> ParticleSystem (+0x04) -> storage
     * module (+0x84, virtual count/get at slots 5/6). Particle position and
     * remaining life (decremented once per 30 Hz simulation step).
     */
    PARTICLE_DRAW_MODULE_SYSTEM = 0x04,
    PARTICLE_SYSTEM_STORAGE = 0x84,
    PARTICLE_STORAGE_COUNT_SLOT = 5,
    PARTICLE_STORAGE_GET_SLOT = 6,
    /*
     * Storage casts: slot 10 (+0x28) returns the storage if it is a swarm
     * storage, slot 11 (+0x2C) if it is a trail storage, else NULL. A draw
     * module can sit on another storage kind (a mod's Ion Cannon pairs a trail
     * draw with a non-trail storage); the stock draws then draw nothing.
     */
    PARTICLE_STORAGE_AS_SWARM_SLOT = 10,
    PARTICLE_STORAGE_AS_TRAIL_SLOT = 11,
    /*
     * The system's world transform (3x4 at +0xCC), recomputed in each 30 Hz
     * step from the attached drawable's interpolated transform. Particles
     * attached to the bone are set to its translation (+0xD8, +0xE8, +0xF8).
     */
    PARTICLE_SYSTEM_WORLD_X = 0xD8,
    PARTICLE_SYSTEM_WORLD_Y = 0xE8,
    PARTICLE_SYSTEM_WORLD_Z = 0xF8,
    PARTICLE_POSITION = 0x10,
    PARTICLE_LIFE_REMAINING = 0x40,
    /*
     * Trail storage: per-trail records (0x28 bytes, at storage +0x1C..+0x20)
     * holding an STLport deque of 0x14-byte points, 6 per block; the newest
     * point (the head particle's latest 30 Hz position) is at the back.
     */
    TRAIL_STORAGE_RECORDS_BEGIN = 0x1C,
    TRAIL_STORAGE_RECORDS_END = 0x20,
    TRAIL_RECORD_SIZE = 0x28,
    TRAIL_POINT_SIZE = 0x14,
    TRAIL_POINTS_PER_BLOCK = 6,
    /*
     * Point: position, width (+0x0C), packed colour (+0x10). Trail draw module data
     * (+0x10) holds TrailLife (+4): live trails are trimmed to that many
     * points at the start of each draw. Storage +0x10 lists each record's
     * particle; system +0x170 set means the system is stopping (all trails
     * drain one point per step).
     */
    TRAIL_DRAW_MODULE_DATA = 0x10,
    TRAIL_DATA_LIFE = 0x04,
    TRAIL_STORAGE_PARTICLES = 0x10,
    PARTICLE_SYSTEM_STOPPING = 0x170,
    /*
     * Swarm storage (Scrin Buzzers): 0x1C-byte records at storage +0x10..+0x14:
     * position, velocity (+0x0C), life. Each 30 Hz step: velocity +=
     * attraction; position += velocity.
     */
    SWARM_STORAGE_RECORDS_BEGIN = 0x10,
    SWARM_STORAGE_RECORDS_END = 0x14,
    SWARM_RECORD_SIZE = 0x1C,
    SWARM_RECORD_VELOCITY = 0x0C,
    SWARM_DRAW_MAX = 0x100,

    /* W3DTracerManager last processed frame; tracer speed per 30 Hz frame. */
    TRACER_MANAGER_LAST_FRAME = 0x90,
    TRACER_INSTANCE_SPEED = 0x58,

    /*
     * Shroud texture object (both kinds): cell grid width/height and the
     * CPU-side cell buffer the per-frame upload copies row by row.
     */
    SHROUD_WIDTH = 0x00,
    SHROUD_HEIGHT = 0x04,
    SHROUD_CELLS = 0x18,
    /* Texture size (cells plus a one-texel border), target and shown levels. */
    SHROUD_TEXTURE_WIDTH = 0x20,
    SHROUD_TEXTURE_HEIGHT = 0x24,
    SHROUD_TARGET_LEVELS = 0x38,
    SHROUD_SHOWN_LEVELS = 0x3C,

    /*
     * TintEnvelope: attack rate (+0x04) and decay rate (+0x10) colour steps
     * per update, sustain countdown (+0x34), state (+0x38: 0 off, 1 attack,
     * 2 decay, 3 sustain).
     */
    TINT_ENVELOPE_RATES = 0x04,
    TINT_ENVELOPE_RATE_FLOATS = 6,
    TINT_ENVELOPE_STATE = 0x38,
    TINT_ENVELOPE_ATTACK = 1,
    TINT_ENVELOPE_DECAY = 2,
    TINT_ENVELOPE_SUSTAIN = 3,

    /* Radar drag operation -> its input handler; view lookAt virtual slot. */
    MINIMAP_DRAG_HANDLER = 0x04,
    VIEW_LOOK_AT_SLOT = 0x48 / 4,

    /* SingleUnitModeHandler -> SelectionDetails::PanelImpl -> panel gadget. */
    SELECTION_HANDLER_PANEL = 0x14,
    SELECTION_PANEL_GADGET = 0x14,
    /* Panel gadget's current state, and the handler's "rebuild buttons" flag. */
    SELECTION_PANEL_STATE = 0x3C,
    SELECTION_HANDLER_REBUILD_BUTTONS = 0x40,
    ADD_INDICATOR_COUNT = 3,

    /* W3DView/CNC3_View virtual slots used by the camera wrappers. */
    W3D_VIEW_SCROLL_BY_SLOT = 0x14,
    W3D_VIEW_GET_ZOOM_SLOT = 0x43,
    W3D_VIEW_SET_ZOOM_SLOT = 0x44
};

static inline u8 *game_address(const GameLayout *game, u32 rva) {
    return game->module + rva;
}

GameResolveResult resolve_game_layout(GameLayout *out_game, u8 *module);

#endif
