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

typedef struct SchedulerLayout {
    /* Start of the 22-byte integer batching expression in DispatchLogicPhase. */
    u32 phase_batch_block;
    /* Entry of the 48-byte __thiscall interpolation leaf function. */
    u32 phase_interpolation_function;
    /* Original sub_6DE5B1 call at byte 34 of the 39-byte phase-end block. */
    BranchSite client_slice_flush;
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
    PARTICLE_DRAW_KINDS
};

typedef struct ParticleDrawLayout {
    /*
     * FXParticleSystem::{Default,Streak,Lightning,RenderObject}DrawModule::Draw
     * (vtable slot 7) for CPU-simulated particles, and their vtable slots.
     */
    u32 draw_functions[PARTICLE_DRAW_KINDS];
    u32 draw_vtable_entries[PARTICLE_DRAW_KINDS];
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

    /* W3DTracerManager last processed frame; tracer speed per 30 Hz frame. */
    TRACER_MANAGER_LAST_FRAME = 0x90,
    TRACER_INSTANCE_SPEED = 0x58,

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
