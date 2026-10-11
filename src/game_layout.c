#include "game_layout.h"

#define PATTERN_ANY 0x100u

/*
 * Signatures are deliberately kept next to the resolver below. Each one is
 * scanned once and immediately assigned to the field that consumes it; there
 * is no parallel ID enum, registry table, or untyped matches array to keep in
 * sync.
 */
static const u16 g_kw_runtime_config_pattern[] = {
    0x01, 0x88, 0x04, 0x0C, 0x00, 0x00, 0x8B, 0x0D,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x5F, 0x5E, 0x5B, 0xC9, 0xC3
};
static const u16 g_kw_session_tail_pattern[] = {
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x5F, 0x5E, 0x5B,
    0xE9, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14
};
/* TW's late runtime call has a different ABI, so it has a separate signature. */
static const u16 g_tw_runtime_config_pattern[] = {
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x89, 0x81, 0x04, 0x0C, 0x00, 0x00,
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x01, 0x88, 0x04, 0x0C, 0x00, 0x00,
    0x5F, 0x5E, 0x5B, 0xC9, 0xC3
};
static const u16 g_tw_session_tail_pattern[] = {
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x5F, 0x5E, 0x5B,
    0xE9, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10
};

/*
 * TW 1.09/1.10 and all supported KW builds share this complete Keyboard
 * update leaf. The decoded tail jump reaches the event processor, where the
 * repeat call is validated at its fixed control-flow offset below.
 */
static const u16 g_keyboard_update_pattern[] = {
    0x56, 0x8B, 0xF1,
    0xFF, 0x86, 0x28, 0x0E, 0x00, 0x00,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xCE, 0x5E,
    0xE9, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};

/* The 111-byte stock repeat body; only its queue-insertion CALL relocates. */
static const u16 g_keyboard_repeat_body_pattern[] = {
    0x55, 0x8B, 0xEC, 0x51, 0x51, 0x53, 0x56, 0x8B, 0xF1, 0x57,
    0x32, 0xC0, 0x33, 0xDB, 0x8D, 0x4E, 0x2C,
    0xBF, 0x00, 0x01, 0x00, 0x00,
    0xF6, 0x41, 0xFE, 0x02, 0x74, 0x0D,
    0x8B, 0x96, 0x28, 0x0E, 0x00, 0x00, 0x2B, 0x11,
    0x83, 0xFA, 0x0A, 0x77, 0x0A,
    0x43, 0x83, 0xC1, 0x08, 0x3B, 0xDF, 0x7C, 0xE5, 0xEB, 0x37,
    0x8D, 0x45, 0xF8, 0x50, 0x8D, 0x4E, 0x1C, 0x88, 0x5D, 0xF8,
    0x66, 0xC7, 0x45, 0xFA, 0x02, 0x01, 0xC6, 0x45, 0xF9, 0x00,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8D, 0x86, 0x28, 0x0E, 0x00, 0x00, 0x8D, 0x4E, 0x2C,
    0x8B, 0x10, 0x89, 0x11, 0x83, 0xC1, 0x08, 0x4F, 0x75, 0xF6,
    0x8B, 0x00, 0x83, 0xE8, 0x0C, 0x89, 0x44, 0xDE, 0x2C, 0xB0, 0x01,
    0x5F, 0x5E, 0x5B, 0xC9, 0xC3
};
static const u8 g_keyboard_physical_event_stamp[] = {
    0x8B, 0x8E, 0x28, 0x0E, 0x00, 0x00,
    0x89, 0x4C, 0xC6, 0x2C
};
static const u16 g_keyboard_repeat_call_block[] = {
    0x3B, 0xFB, 0x75, 0x90, 0x8B, 0xCE,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x66, 0x83, 0x7E, 0x18, 0x00
};

/* Continuous visual integration sites that incorrectly share retail 1/30. */
static const u16 g_camera_pattern[] = {
    0x80, 0xBB, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x75, 0x6F,
    0xD9, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x51
};
static const u16 g_laser_pattern[] = {
    0x0F, 0x2F, 0xF1, 0xF3, 0x0F, 0x10, 0x1D,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x2A, 0xE0, 0x0F, 0x28, 0xEC, 0xF3, 0x0F, 0x59, 0xEB
};
static const u16 g_model_pattern[] = {
    0x8B, 0x01, 0xFF, 0x50, 0x78,
    0xF3, 0x0F, 0x10, 0x86, 0xF4, 0x02, 0x00, 0x00,
    0xF3, 0x0F, 0x58, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};

/*
 * TW and KW share this camera-input implementation byte-for-byte. It is
 * evaluated once per client frame, so scroll input, held zoom/rotation keys,
 * terrain-height settling, and legacy camera shake need separate scaling.
 */
static const u16 g_camera_zoom_behavior_pair_pattern[] = {
    0x80, 0x7C, 0x24, 0x08, 0x00, 0x74, 0x0E, 0x8B, 0x0D,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0xFF, 0x90, 0x14, 0x01, 0x00, 0x00, 0xC2, 0x10, 0x00,
    0x80, 0x7C, 0x24, 0x08, 0x00, 0x74, 0x0E, 0x8B, 0x0D,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0xFF, 0x90, 0x18, 0x01, 0x00, 0x00, 0xC2, 0x10, 0x00
};
static const u16 g_camera_rotate_behavior_pattern[] = {
    0x80, 0x7C, 0x24, 0x08, 0x00, 0x74, 0x1E, 0xA1,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x10, 0x80, 0x34, 0x0D, 0x00, 0x00,
    0x8B, 0x44, 0x24, 0x10, 0xF3, 0x0F, 0x59, 0x41, 0x04,
    0xF3, 0x0F, 0x58, 0x00, 0xF3, 0x0F, 0x11, 0x00, 0xC2, 0x10, 0x00
};
static const u16 g_camera_scroll_by_pattern[] = {
    0x55, 0x8B, 0xEC, 0xA1,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x83, 0xEC, 0x64, 0x80, 0xB8, 0xCC, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x8B, 0xD9, 0x74, 0x0D, 0x80, 0x7B, 0x44, 0x00, 0x74, 0x07,
    0x33, 0xC0, 0xE9,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x80, 0xBB, 0xF1, 0x24, 0x00, 0x00, 0x00, 0x75, 0xF0
};
static const u16 g_camera_adjust_pattern[] = {
    0xD9, 0x45, 0xF8, 0xD8, 0x65, 0xF4,
    0xD8, 0x8F, 0x14, 0x0B, 0x00, 0x00,
    0xD9, 0x5D, 0xF4, 0xD9, 0x45, 0xF4, 0xD9, 0xE1, 0xDD, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};
static const u16 g_camera_shake_pattern[] = {
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x80, 0xB8, 0x44, 0x0F, 0x00, 0x00, 0x00, 0x74, 0x09,
    0x80, 0xB8, 0x45, 0x0F, 0x00, 0x00, 0x00, 0x74, 0x22,
    0xF3, 0x0F, 0x59, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x11, 0x43, 0x78
};

/* Absolute client-frame consumers whose authored data remains in 30 Hz units. */
static const u16 g_tracer_reset_pattern[] = {
    0x8B, 0x01, 0xFF, 0x50, 0x78, 0x89, 0x86, 0x90, 0x00, 0x00, 0x00, 0x5E, 0xC3
};
static const u16 g_tracer_update_pattern[] = {
    0x8B, 0x01, 0x57, 0xFF, 0x50, 0x78, 0x3B, 0x86, 0x90, 0x00, 0x00, 0x00
};
static const u16 g_cloud_pattern[] = {
    0x8B, 0x01, 0xFF, 0x50, 0x78, 0x33, 0xDB, 0x32, 0xC9, 0x39, 0x46, 0x70
};
static const u16 g_anim2d_set_pattern[] = {
    0x66, 0x89, 0x46, 0x04, 0x8B, 0x0D,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0xFF, 0x50, 0x78, 0x89, 0x46, 0x08
};
static const u16 g_anim2d_update_pattern[] = {
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0xFF, 0x50, 0x78, 0x2B, 0x46, 0x08, 0x3B, 0x46, 0x18
};

/*
 * APT (Flash UI) advances movie frames on a real-time ~30 Hz accumulator.
 * Objective markers are new APT clips that receive their first screen
 * position from an OnInitialized callback on the next APT frame.
 */
static const u16 g_apt_player_singleton_pattern[] = {
    0x88, 0x9E, 0xE9, 0x01, 0x00, 0x00, 0x88, 0x9E, 0xEA, 0x01, 0x00, 0x00,
    0x39, 0x1D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x75, 0x06, 0x89, 0x35, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};
/* TacticalObjectiveIndicatorStageImpl::Add{Object,OffScreen,Waypoint}Indicator. */
static const u16 g_add_object_indicator_pattern[] = {
    0x55, 0x8B, 0xEC, 0x56, 0x57, 0x6A, 0x54, 0x8B, 0xF1,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x59, 0x74, 0x19, 0x8B, 0x56, 0x24, 0xFF, 0x75, 0x0C,
    0x8D, 0x4A, 0x01, 0x52, 0x89, 0x4E, 0x24, 0x56, 0x8B, 0xC8, 0xE8
};
static const u16 g_add_offscreen_indicator_pattern[] = {
    0x55, 0x8B, 0xEC, 0x56, 0x57, 0x6A, 0x60, 0x8B, 0xF1,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x59, 0x74, 0x19, 0x8B, 0x56, 0x30, 0xFF, 0x75, 0x0C,
    0x8D, 0x4A, 0x01, 0x52, 0x89, 0x4E, 0x30, 0x56, 0x8B, 0xC8, 0xE8
};
static const u16 g_add_waypoint_indicator_pattern[] = {
    0x55, 0x8B, 0xEC, 0x56, 0x57, 0x6A, 0x4C, 0x8B, 0xF1,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x59, 0x74, 0x19, 0x8B, 0x56, 0x3C, 0xFF, 0x75, 0x0C,
    0x8D, 0x4A, 0x01, 0x52, 0x89, 0x4E, 0x3C, 0x56, 0x8B, 0xC8, 0xE8
};

/*
 * Drawable::calcPhysicsXform body after its 7-byte prologue: Object->AI->
 * Locomotor, then "if (locomotor->lastFrame == GameClient frame) return 0".
 * The Object->AI offset differs (TW 0x2E8, KW 0x2F4).
 */
static const u16 g_physics_xform_dispatcher_pattern[] = {
    0x8B, 0x87, 0x40, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x08,
    0x8B, 0x80, PATTERN_ANY, 0x02, 0x00, 0x00, 0xEB, 0x02, 0x33, 0xC0, 0x85, 0xC0,
    0xC6, 0x45, 0xFF, 0x00, 0x0F, 0x84, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x56, 0x8B, 0xB0, 0xF4, 0x01, 0x00, 0x00, 0x85, 0xF6,
    0x0F, 0x84, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0x53, 0xFF, 0x50, 0x78, 0x8B, 0xD8,
    0x39, 0x9E, 0xC0, 0x00, 0x00, 0x00
};
/* Drawable::applyPhysicsXform zeroes the 4-float info record, then calls it. */
static const u16 g_physics_xform_apply_pattern[] = {
    0x8D, 0x45, 0xF0, 0x50, 0x8B, 0xCE,
    0xF3, 0x0F, 0x11, 0x45, 0xF0, 0xF3, 0x0F, 0x11, 0x45, 0xF4,
    0xF3, 0x0F, 0x11, 0x45, 0xF8, 0xF3, 0x0F, 0x11, 0x45, 0xFC,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x84, 0xC0, 0x0F, 0x84
};

/* Particle/cursor sites use a mix of client-frame and millisecond timebases. */
static const u16 g_particle_pattern[] = {
    0x8B, 0xF1, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x83, 0x66, 0x40, 0x00, 0x6A, 0x02, 0x8D, 0xBE, 0x8C, 0x00, 0x00, 0x00,
    0x5B, 0xFF, 0x77, 0x04, 0x8B, 0xCF
};
static const u16 g_gpu_pattern[] = {
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x0F, 0xAF, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x89, 0x45, 0xFC, 0x56, 0x8B, 0xF1
};
static const u16 g_radius_cursor_throb_pattern[] = {
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0x57, 0xFF, 0x50, 0x78, 0x8B, 0xF8, 0x8B, 0x06,
    0xD9, 0x40, 0x14, 0x51, 0xD8, 0x0D,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x51, 0xDD, 0x1C, 0x24
};

/* Independent presentation limiter and GameEngine pacing/history control flow. */
static const u16 g_display_pattern[] = {
    0x83, 0xF9, 0x1D, 0x7D, 0x13, 0xE8
};
static const u16 g_pacing_pattern[] = {
    0x3B, 0xC8, 0x76, 0x07, 0xC6, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x74, 0x69, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xDB, 0x86, 0x64, 0x01, 0x00, 0x00, 0x8B, 0xD8,
    0xD8, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD8, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};
/*
 * Confirmed KW virtual addresses. Absolute and relative operands are the only
 * wildcarded bytes in the signatures below.
 *
 *                              phase batch  interpolation  slice flush  W3D special  W3D normal
 * KW 1.02 EA/Origin 2009       0x00583DE7   0x0057149A     0x0051C892   0x007A4F53   0x007A4F8E
 * KW 1.02 Steam 2012           0x006560A6   0x0064333E     0x005EF10F   0x004A5A7C   0x004A5AB7
 * KW 1.03                      0x005C3C61   0x005B0F6D     0x0055C5A6   0x007F0F31   0x007F0F6C
 */
static const u16 g_phase_batch_pattern[] = {
    0x8D, 0x47, 0xFF, 0x0F, 0xAF, 0xC1, 0x99, 0x6A, 0x06, 0x5E, 0xF7,
    0xFE, 0x40, 0x6B, 0xC0, 0x06, 0x99, 0xF7, 0xF9, 0x8B, 0xF0, 0x46
};
static const u16 g_phase_interpolation_pattern[] = {
    0xF3, 0x0F, 0x2A, 0x41, 0x40, 0xF3, 0x0F, 0x59, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x0F, 0x57, 0xC9, 0x0F, 0x2F, 0xC8, 0xF3, 0x0F, 0x11, 0x41, 0x48,
    0x77, 0x0D, 0xF3, 0x0F, 0x10, 0x0D,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x0F, 0x2F, 0xC1, 0x76, 0x03, 0x0F, 0x28, 0xC1,
    0xF3, 0x0F, 0x11, 0x41, 0x48, 0xC3
};
static const u16 g_client_slice_flush_pattern[] = {
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x33, 0xD2, 0xF7, 0x35,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x6A, 0x06, 0x8B, 0xC8, 0x58, 0x99, 0xF7, 0xF9, 0x5F, 0x8B, 0xC8,
    0x8B, 0x45, 0x08, 0x99, 0xF7, 0xF9, 0x85, 0xD2, 0x75, 0x05,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};
static const u16 g_w3d_special_advance_pattern[] = {
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x03, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x50, 0xA3, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};
static const u16 g_w3d_normal_advance_pattern[] = {
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x0F, 0xAF, 0xC6, 0x01, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xFF, 0x35, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};

/* CRT initializers prove the identities and units of timing-cache globals. */
static const u16 g_w3d_initializer_pattern[] = {
    0xB8, 0xE8, 0x03, 0x00, 0x00, 0x33, 0xD2, 0xF7, 0x35,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xA3, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0xC3
};
static const u16 g_audio_initializer_pattern[] = {
    0x51, 0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xDB, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x7D, 0x06, 0xD8, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD8, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD9, 0x1D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x59, 0xC3
};

/*
 * Stable tail of SageAudioManager::UpdateForClientFrame. The function begins
 * 0x12A bytes before this block in every supported image. Its sole non-code
 * absolute xref is vtable slot 5.
 */
static const u16 g_audio_update_tail_pattern[] = {
    0x8B, 0x8E, 0x6C, 0x02, 0x00, 0x00,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x4E, 0x30, 0x3B, 0xCB, 0x74, PATTERN_ANY,
    0xF3, 0x0F, 0x2C, 0x46, 0x34, 0x50,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8D, 0x45, 0xF8, 0x50, 0x8B, 0xCE,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8D, 0x45, 0xF8, 0x50, 0x8B, 0xCE,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY
};
static const u16 g_float_initializer_pattern[] = {
    0x51, 0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xDB, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x7D, 0x06, 0xD8, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD9, 0x1D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x59, 0xC3
};
static const u16 g_frames_per_millisecond_initializer_pattern[] = {
    0x51, 0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xDB, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x7D, 0x06, 0xD8, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD8, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD9, 0x1D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x59, 0xC3
};
static const u16 g_seconds_initializer_pattern[] = {
    0xF3, 0x0F, 0x10, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x5E, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x11, 0x05,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0xC3
};

static BOOL get_nt_headers(u8 *module, IMAGE_NT_HEADERS32 **out_nt) {
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    if (module == NULL || out_nt == NULL) return FALSE;
    dos = (IMAGE_DOS_HEADER *)module;
    /* Bound e_lfanew before following a pointer supplied by an unknown image. */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        return FALSE;
    }
    nt = (IMAGE_NT_HEADERS32 *)(module + (u32)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage < 0x400000u ||
        nt->OptionalHeader.SizeOfImage > 0x04000000u) {
        return FALSE;
    }
    *out_nt = nt;
    return TRUE;
}

static BOOL pattern_matches(const u8 *bytes, const u16 *pattern, size_t size) {
    size_t i;
    for (i = 0; i < size; ++i) {
        if (pattern[i] != PATTERN_ANY && bytes[i] != (u8)pattern[i]) return FALSE;
    }
    return TRUE;
}

static BOOL find_unique_signature_filtered(
    u8 *module, IMAGE_NT_HEADERS32 *nt, const u16 *pattern, size_t pattern_size,
    size_t operand_offset, u32 expected_absolute, u32 *out_rva) {
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 found = 0;
    size_t matches = 0;
    u16 section_index;

    /*
     * Scan executable virtual ranges, where relocation has already fixed
     * absolute operands. Requiring exactly one match makes every signature a
     * semantic assertion rather than a best-effort version lookup.
     */
    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start;
        u32 size;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        start = section->VirtualAddress;
        size = section->Misc.VirtualSize;
        if (start >= nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - start || size < pattern_size) {
            continue;
        }
        for (offset = 0; offset <= size - (u32)pattern_size; ++offset) {
            u8 *candidate = module + start + offset;
            if (pattern_matches(candidate, pattern, pattern_size) &&
                (operand_offset == SIZE_MAX ||
                 load_u32(candidate + operand_offset) == expected_absolute)) {
                found = start + offset;
                if (++matches > 1) return FALSE;
            }
        }
    }
    if (matches != 1) return FALSE;
    *out_rva = found;
    return TRUE;
}

static BOOL find_unique_signature(u8 *module, IMAGE_NT_HEADERS32 *nt,
                                  const u16 *pattern, size_t pattern_size,
                                  u32 *out_rva) {
    return find_unique_signature_filtered(
        module, nt, pattern, pattern_size, SIZE_MAX, 0, out_rva);
}

static BOOL find_signature_using_absolute_operand(
    u8 *module, IMAGE_NT_HEADERS32 *nt, const u16 *pattern, size_t pattern_size,
    size_t input_operand_offset, u32 expected_absolute, u32 *out_rva) {
    return find_unique_signature_filtered(module, nt, pattern, pattern_size,
                                          input_operand_offset, expected_absolute, out_rva);
}

static BOOL find_unique_readonly_pointer(
    u8 *module, IMAGE_NT_HEADERS32 *nt, u32 expected_absolute, u32 *out_rva) {
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 found = 0;
    size_t matches = 0;
    u16 section_index;

    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start;
        u32 size;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            (section->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) != 0) {
            continue;
        }
        start = section->VirtualAddress;
        size = section->Misc.VirtualSize;
        if (start >= nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - start || size < sizeof(u32)) {
            continue;
        }
        for (offset = 0; offset <= size - sizeof(u32); offset += sizeof(u32)) {
            if (load_u32(module + start + offset) == expected_absolute) {
                found = start + offset;
                if (++matches > 1) return FALSE;
            }
        }
    }
    if (matches != 1) return FALSE;
    *out_rva = found;
    return TRUE;
}

static BOOL absolute_operand_to_rva(u8 *module, u32 image_size,
                                    u32 absolute, u32 *out_rva) {
    uintptr_t base = (uintptr_t)module;
    uintptr_t address = (uintptr_t)absolute;
    /* Reject imports, heap pointers, and any operand outside the main image. */
    if (address < base || address - base >= image_size) return FALSE;
    *out_rva = (u32)(address - base);
    return TRUE;
}

static BOOL read_absolute_rva(u8 *module, u32 image_size,
                              u32 operand_rva, u32 *out_rva) {
    if (operand_rva > image_size - sizeof(u32)) return FALSE;
    return absolute_operand_to_rva(module, image_size,
                                   load_u32(module + operand_rva), out_rva);
}

static BOOL decode_relative_target(u8 *module, u32 image_size,
                                   u32 instruction_rva, u8 opcode,
                                   u32 *out_target_rva) {
    i32 displacement;
    i32 target;
    /* BranchSite targets are stored as RVAs so ASLR never leaks past resolution. */
    if (instruction_rva > image_size - 5u || module[instruction_rva] != opcode) return FALSE;
    memcpy(&displacement, module + instruction_rva + 1u, sizeof(displacement));
    target = (i32)(instruction_rva + 5u) + displacement;
    if (target < 0 || (u32)target >= image_size) return FALSE;
    *out_target_rva = (u32)target;
    return TRUE;
}

#define FIND_UNIQUE(module, nt, pattern, out_rva) \
    find_unique_signature((module), (nt), (pattern), ARRAY_COUNT(pattern), (out_rva))

static BOOL resolve_kw_bootstrap_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    u32 match;
    BranchSite runtime_config_call;
    BranchSite start_session_tail;

    if (!FIND_UNIQUE(module, nt, g_kw_runtime_config_pattern, &match)) return FALSE;
    runtime_config_call.instruction = match + 12u;
    if (!decode_relative_target(module, game->pe_size_of_image,
                                runtime_config_call.instruction, 0xE8,
                                &runtime_config_call.target)) {
        return FALSE;
    }

    if (!FIND_UNIQUE(module, nt, g_kw_session_tail_pattern, &match)) return FALSE;
    start_session_tail.instruction = match + 13u;
    if (!decode_relative_target(module, game->pe_size_of_image,
                                start_session_tail.instruction, 0xE9,
                                &start_session_tail.target)) {
        return FALSE;
    }

    game->bootstrap.runtime_config_call = runtime_config_call;
    game->bootstrap.start_session_tail = start_session_tail;
    game->bootstrap.runtime_config_hook_kind = RUNTIME_CONFIG_HOOK_THISCALL;
    game->target_name = "Kane's Wrath (signature-resolved)";
    return TRUE;
}

static BOOL resolve_tw_bootstrap_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    u32 match;
    BranchSite runtime_config_call;
    BranchSite start_session_tail;

    if (!FIND_UNIQUE(module, nt, g_tw_runtime_config_pattern, &match)) return FALSE;
    runtime_config_call.instruction = match;
    if (!decode_relative_target(module, game->pe_size_of_image,
                                runtime_config_call.instruction, 0xE8,
                                &runtime_config_call.target)) {
        return FALSE;
    }

    if (!FIND_UNIQUE(module, nt, g_tw_session_tail_pattern, &match)) return FALSE;
    start_session_tail.instruction = match + 13u;
    if (!decode_relative_target(module, game->pe_size_of_image,
                                start_session_tail.instruction, 0xE9,
                                &start_session_tail.target)) {
        return FALSE;
    }

    game->bootstrap.runtime_config_call = runtime_config_call;
    game->bootstrap.start_session_tail = start_session_tail;
    game->bootstrap.runtime_config_hook_kind = RUNTIME_CONFIG_HOOK_NOARG;
    game->target_name = "Tiberium Wars (signature-resolved)";
    return TRUE;
}

static BOOL resolve_bootstrap_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    GameLayout kw_game = *game;
    GameLayout tw_game = *game;
    BOOL kw_resolved = resolve_kw_bootstrap_sites(module, nt, &kw_game);
    BOOL tw_resolved = resolve_tw_bootstrap_sites(module, nt, &tw_game);

    /* Exactly one ABI-specific bootstrap must match; ambiguity is unsafe. */
    if (kw_resolved == tw_resolved) return FALSE;
    *game = kw_resolved ? kw_game : tw_game;
    return TRUE;
}

static BOOL resolve_keyboard_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    KeyboardLayout *keyboard = &game->keyboard;
    u32 update;
    u32 process_events;

    if (!FIND_UNIQUE(module, nt, g_keyboard_update_pattern, &update)) return FALSE;
    keyboard->input_frame_increment = update + 3u;
    keyboard->hardware_poll.instruction = update + 9u;
    if (!decode_relative_target(
            module, game->pe_size_of_image,
            keyboard->hardware_poll.instruction, 0xE8,
            &keyboard->hardware_poll.target) ||
        !decode_relative_target(
            module, game->pe_size_of_image, update + 17u, 0xE9,
            &process_events) ||
        process_events > game->pe_size_of_image - 0x89u ||
        memcmp(module + process_events + 0x26u,
               g_keyboard_physical_event_stamp,
               sizeof(g_keyboard_physical_event_stamp)) != 0 ||
        !pattern_matches(module + process_events + 0x79u,
                         g_keyboard_repeat_call_block,
                         ARRAY_COUNT(g_keyboard_repeat_call_block))) {
        return FALSE;
    }

    keyboard->check_repeat.instruction = process_events + 0x7Fu;
    if (!decode_relative_target(
            module, game->pe_size_of_image,
            keyboard->check_repeat.instruction, 0xE8,
            &keyboard->check_repeat.target) ||
        keyboard->check_repeat.target >
            game->pe_size_of_image - ARRAY_COUNT(g_keyboard_repeat_body_pattern) ||
        !pattern_matches(module + keyboard->check_repeat.target,
                         g_keyboard_repeat_body_pattern,
                         ARRAY_COUNT(g_keyboard_repeat_body_pattern))) {
        return FALSE;
    }
    return TRUE;
}

static BOOL resolve_camera_input_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    CameraInputLayout *camera = &game->camera_input;
    u32 match;
    u32 second_view_pointer;
    u32 rotate_behavior_function;
    u32 rotate_global_data_pointer;

    /*
     * The adjacent 24-byte held-zoom behaviors are an especially strong
     * anchor: both load the same W3DView singleton and call slots 0x45/0x46.
     * Mouse-wheel zoom has a separate caller and is intentionally not hooked.
     */
    if (!FIND_UNIQUE(module, nt, g_camera_zoom_behavior_pair_pattern, &match) ||
        !read_absolute_rva(module, game->pe_size_of_image, match + 9u,
                           &camera->w3d_view_pointer) ||
        !read_absolute_rva(module, game->pe_size_of_image, match + 33u,
                           &second_view_pointer) ||
        second_view_pointer != camera->w3d_view_pointer) {
        return FALSE;
    }
    camera->zoom_in_behavior_function = match;
    camera->zoom_out_behavior_function = match + 24u;

    if (!FIND_UNIQUE(module, nt, g_camera_rotate_behavior_pattern,
                     &rotate_behavior_function) ||
        !read_absolute_rva(module, game->pe_size_of_image,
                           rotate_behavior_function + 8u,
                           &rotate_global_data_pointer) ||
        rotate_global_data_pointer != game->timing.global_data_pointer ||
        !FIND_UNIQUE(module, nt, g_camera_scroll_by_pattern,
                     &camera->scroll_by_function) ||
        !FIND_UNIQUE(module, nt, g_camera_adjust_pattern, &match)) {
        return FALSE;
    }
    camera->camera_adjust_instruction = match + 6u;

    if (!FIND_UNIQUE(module, nt, g_camera_shake_pattern, &match)) return FALSE;
    camera->shake_decay_operand = match + 27u;
    if (!read_absolute_rva(module, game->pe_size_of_image,
                           camera->shake_decay_operand,
                           &camera->retail_shake_decay) ||
        load_u32(module + camera->retail_shake_decay) != 0x3F400000u) {
        return FALSE;
    }

    return TRUE;
}

static BOOL resolve_visual_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    VisualLayout *visual = &game->visual;
    u32 match;
    u32 retail_step_address;

    if (!FIND_UNIQUE(module, nt, g_camera_pattern, &match)) return FALSE;
    visual->camera_step_operand = match + 11u;
    if (!FIND_UNIQUE(module, nt, g_laser_pattern, &match)) return FALSE;
    visual->laser_step_operand = match + 7u;
    if (!FIND_UNIQUE(module, nt, g_model_pattern, &match)) return FALSE;
    visual->model_step_operand = match + 17u;

    if (!FIND_UNIQUE(module, nt, g_tracer_reset_pattern, &visual->tracer_reset_frame_call) ||
        !FIND_UNIQUE(module, nt, g_tracer_update_pattern, &visual->tracer_update_frame_call) ||
        !FIND_UNIQUE(module, nt, g_cloud_pattern, &visual->cloud_frame_call)) {
        return FALSE;
    }
    if (!FIND_UNIQUE(module, nt, g_anim2d_set_pattern, &match)) return FALSE;
    visual->anim2d_timestamp_frame_call = match + 10u;
    if (!FIND_UNIQUE(module, nt, g_anim2d_update_pattern, &match)) return FALSE;
    visual->anim2d_update_frame_call = match + 6u;

    if (!FIND_UNIQUE(module, nt, g_particle_pattern, &match)) return FALSE;
    visual->particle_simulation.instruction = match + 2u;
    if (!decode_relative_target(module, game->pe_size_of_image,
                                visual->particle_simulation.instruction, 0xE8,
                                &visual->particle_simulation.target)) {
        return FALSE;
    }

    if (!FIND_UNIQUE(module, nt, g_gpu_pattern, &match)) return FALSE;
    visual->gpu_particle_fps_instruction = match + 5u;
    visual->gpu_particle_fps_operand = match + 8u;
    if (!read_absolute_rva(module, game->pe_size_of_image,
                           visual->gpu_particle_fps_operand,
                           &game->timing.client_fps) ||
        game->timing.client_fps < 4u) {
        return FALSE;
    }
    /* The two authoritative integer rates are adjacent: logic FPS then client FPS. */
    game->timing.logic_fps = game->timing.client_fps - 4u;

    if (!FIND_UNIQUE(module, nt, g_radius_cursor_throb_pattern, &match)) return FALSE;
    visual->radius_cursor_fps_instruction = match + 20u;
    visual->radius_cursor_fps_operand = match + 22u;
    if (!read_absolute_rva(module, game->pe_size_of_image,
                           visual->radius_cursor_fps_operand,
                           &visual->retail_frames_per_millisecond) ||
        !read_absolute_rva(module, game->pe_size_of_image,
                           visual->camera_step_operand, &visual->retail_step)) {
        return FALSE;
    }

    /* Camera, laser, and model must all name the same relocated retail scalar. */
    retail_step_address = (u32)(uintptr_t)(module + visual->retail_step);
    return load_u32(module + visual->laser_step_operand) == retail_step_address &&
           load_u32(module + visual->model_step_operand) == retail_step_address;
}

static BOOL resolve_pacing_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    PacingLayout *pacing = &game->pacing;
    u32 match;

    if (!FIND_UNIQUE(module, nt, g_display_pattern, &match)) return FALSE;
    pacing->display_limiter_branch = match + 3u;

    if (!FIND_UNIQUE(module, nt, g_pacing_pattern, &match)) return FALSE;
    pacing->limiter_interval_block = match + 25u;
    if (!read_absolute_rva(module, game->pe_size_of_image, match + 13u,
                           &pacing->enforce_limit_flag) ||
        !read_absolute_rva(module, game->pe_size_of_image, match + 35u,
                           &pacing->network_scale) ||
        !read_absolute_rva(module, game->pe_size_of_image, match + 41u,
                           &pacing->milliseconds_per_logic_frame) ||
        !decode_relative_target(module, game->pe_size_of_image,
                                match + 45u, 0xE8,
                                &pacing->limiter_conversion_target)) {
        return FALSE;
    }

    /* The display wait reads GlobalData; prove it is the same resolved cluster. */
    if (!read_absolute_rva(module, game->pe_size_of_image,
                           pacing->display_limiter_branch + 11u,
                           &game->timing.global_data_pointer) ||
        game->timing.global_data_pointer != pacing->enforce_limit_flag + 0x47u ||
        pacing->enforce_limit_flag < 0x27u) {
        return FALSE;
    }

    /*
     * These globals form one fixed compiler-laid-out cluster in supported
     * builds. Derive them from the signature-proven flag instead of adding
     * several weak signatures for otherwise anonymous DWORDs.
     */
    game->timing.game_engine_pointer = pacing->enforce_limit_flag + 0x27u;
    pacing->previous_frame_time_ms = pacing->enforce_limit_flag + 0x1Fu;
    return TRUE;
}

static BOOL resolve_cached_timing_values(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    TimingLayout *timing = &game->timing;
    u32 initializer;
    u32 source_address = (u32)(uintptr_t)(module + timing->client_fps);

    /* Follow startup initializers from authoritative g_clientUpdateFPS. */
    if (!FIND_UNIQUE(module, nt, g_w3d_initializer_pattern, &initializer) ||
        load_u32(module + initializer + 9u) != source_address ||
        !read_absolute_rva(module, game->pe_size_of_image, initializer + 14u,
                           &timing->w3d_milliseconds_per_frame)) {
        return FALSE;
    }
    if (!find_signature_using_absolute_operand(
            module, nt, g_float_initializer_pattern,
            ARRAY_COUNT(g_float_initializer_pattern), 2u, source_address, &initializer) ||
        load_u32(module + initializer + 8u) != source_address ||
        !read_absolute_rva(module, game->pe_size_of_image, initializer + 24u,
                           &timing->client_fps_float)) {
        return FALSE;
    }
    if (!find_signature_using_absolute_operand(
            module, nt, g_audio_initializer_pattern,
            ARRAY_COUNT(g_audio_initializer_pattern), 2u, source_address, &initializer) ||
        load_u32(module + initializer + 8u) != source_address ||
        !read_absolute_rva(module, game->pe_size_of_image, initializer + 30u,
                           &timing->audio_milliseconds_per_frame)) {
        return FALSE;
    }
    /*
     * Prove the radius operand names the FPS-derived cache by following its
     * initializer. The cache's live value depends on initialization order and
     * is intentionally not used as build identity.
     */
    if (!find_signature_using_absolute_operand(
            module, nt, g_frames_per_millisecond_initializer_pattern,
            ARRAY_COUNT(g_frames_per_millisecond_initializer_pattern),
            2u, source_address, &initializer) ||
        load_u32(module + initializer + 8u) != source_address ||
        load_u32(module + initializer + 30u) !=
            (u32)(uintptr_t)(module + game->visual.retail_frames_per_millisecond)) {
        return FALSE;
    }

    source_address = (u32)(uintptr_t)(module + timing->client_fps_float);
    return find_signature_using_absolute_operand(
               module, nt, g_seconds_initializer_pattern,
               ARRAY_COUNT(g_seconds_initializer_pattern), 12u, source_address,
               &initializer) &&
           read_absolute_rva(module, game->pe_size_of_image, initializer + 20u,
                             &timing->visual_seconds_per_frame);
}

static BOOL resolve_audio_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    AudioLayout *audio = &game->audio;
    static const u8 update_entry[] = { 0x55, 0x8B, 0xEC, 0xA1 };
    u32 tail;
    u32 expected_update;

    if (!FIND_UNIQUE(module, nt, g_audio_update_tail_pattern, &tail) ||
        tail < 0x12Au) {
        return FALSE;
    }
    audio->update_function = tail - 0x12Au;
    if (memcmp(module + audio->update_function,
               update_entry, sizeof(update_entry)) != 0) {
        return FALSE;
    }
    expected_update = (u32)(uintptr_t)(module + audio->update_function);
    return find_unique_readonly_pointer(
        module, nt, expected_update, &audio->update_vtable_entry);
}

static BOOL resolve_drawable_physics_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    DrawablePhysicsLayout *physics = &game->drawable_physics;
    static const u8 dispatcher_prologue[] = { 0x55, 0x8B, 0xEC, 0x51, 0x57, 0x8B, 0xF9 };
    u32 body;
    u32 call;

    if (!FIND_UNIQUE(module, nt, g_physics_xform_dispatcher_pattern, &body) ||
        body < sizeof(dispatcher_prologue) ||
        memcmp(module + body - sizeof(dispatcher_prologue), dispatcher_prologue,
               sizeof(dispatcher_prologue)) != 0 ||
        !read_absolute_rva(module, game->pe_size_of_image, body + 0x31u,
                           &physics->game_client_pointer)) {
        return FALSE;
    }
    physics->dispatcher_frame_call = body + 0x35u;

    if (!FIND_UNIQUE(module, nt, g_physics_xform_apply_pattern, &call)) return FALSE;
    physics->apply_call.instruction = call + 26u;
    return decode_relative_target(module, game->pe_size_of_image,
                                  physics->apply_call.instruction, 0xE8,
                                  &physics->apply_call.target) &&
           physics->apply_call.target == body - sizeof(dispatcher_prologue);
}

/*
 * "MOV ECX,[g_theGameClient]; MOV EAX,[ECX]; CALL [EAX+78h]; SUB EAX,<history>;
 * CMP EAX,6". TW addresses Drawable+0x138 directly (disp32); KW first loads
 * its address into a register.
 */
static const u16 g_interpolation_window_disp32_pattern[] = {
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0xFF, 0x50, 0x78,
    0x2B, PATTERN_ANY, 0x38, 0x01, 0x00, 0x00, 0x83, 0xF8, 0x06
};
static const u16 g_interpolation_window_indirect_pattern[] = {
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0xFF, 0x50, 0x78,
    0x2B, PATTERN_ANY, 0x83, 0xF8, 0x06
};

static BOOL resolve_drawable_interpolation_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    DrawableInterpolationLayout *interp = &game->drawable_interpolation;
    const u16 *patterns[2] = {
        g_interpolation_window_disp32_pattern, g_interpolation_window_indirect_pattern
    };
    const size_t sizes[2] = {
        ARRAY_COUNT(g_interpolation_window_disp32_pattern),
        ARRAY_COUNT(g_interpolation_window_indirect_pattern)
    };
    u32 game_client = (u32)(uintptr_t)(module + game->drawable_physics.game_client_pointer);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 matches = 0;
    u16 section_index;
    u32 p;

    /* Exactly two windows must exist, both reading the same GameClient. */
    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start = section->VirtualAddress;
        u32 size = section->Misc.VirtualSize;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            start >= nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - start) {
            continue;
        }
        for (p = 0; p < 2; ++p) {
            if (size < sizes[p]) continue;
            for (offset = 0; offset <= size - (u32)sizes[p]; ++offset) {
                u8 *candidate = module + start + offset;
                if (!pattern_matches(candidate, patterns[p], sizes[p]) ||
                    load_u32(candidate + 2u) != game_client) {
                    continue;
                }
                if (matches == ARRAY_COUNT(interp->window_immediates)) return FALSE;
                interp->window_immediates[matches++] =
                    start + offset + (u32)sizes[p] - 1u;
            }
        }
    }
    return matches == ARRAY_COUNT(interp->window_immediates);
}

/*
 * TacticalSelectionDetailsPanelImpl::SetUnitName(name): stores the name at
 * +0x40 and, only if it changed and the gadget is initialized (+0x14), calls
 * the helper that pushes the "UnitName" APT variable.
 */
static const u16 g_set_unit_name_pattern[] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08, 0x57, 0x8D, 0x7E, 0x40, 0x57,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x74, 0x18, 0xFF, 0x74, 0x24, 0x0C, 0x8B, 0xCF,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x80, 0x7E, 0x14, 0x00, 0x74, 0x07, 0x8B, 0xCE,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x5F, 0x5E, 0xC2, 0x04, 0x00
};
/* SetState(state): returns early unless state differs from +0x3C. */
static const u8 g_set_state_prologue[] = {
    0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1, 0x3B, 0x7E, 0x3C, 0x74, 0x63,
    0x85, 0xFF, 0x75, 0x1A, 0x80, 0x7E, 0x70, 0x00, 0x74, 0x14
};
/* Push helper: LEA EAX,[ECX+40h]; PUSH EAX; LEA EAX,[ECX+0Ch]; PUSH "UnitName". */
static const u8 g_push_unit_name_prefix[] = {
    0x8D, 0x41, 0x40, 0x50, 0x8D, 0x41, 0x0C, 0x68
};

/*
 * SingleUnitModeHandler per-frame update: optional production/exit button
 * rebuild (+0x40 flag), then rank, portrait/ammo updates and button loops.
 */
static const u16 g_single_unit_update_pattern[] = {
    0x53, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x40, 0x00, 0x57, 0x74, 0x18,
    0x8D, 0x4E, 0x1C, 0xFF, 0x71, 0x04, 0xFF, 0x31,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xC6, 0x46, 0x40, 0x00,
    0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x5E, 0x20, 0x8B, 0x7E, 0x1C, 0xEB, 0x0A,
    0x8B, 0x0F, 0x8B, 0x01, 0xFF, 0x50, 0x04, 0x83, 0xC7, 0x04, 0x3B, 0xFB, 0x75, 0xF2
};

static BOOL resolve_selection_panel_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    static const char unit_name[] = "UnitName";
    SelectionPanelLayout *panel = &game->selection_panel;
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 image_size = game->pe_size_of_image;
    u32 matches = 0;
    u16 section_index;

    /* Similar setters exist; keep only the one whose helper names "UnitName". */
    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start = section->VirtualAddress;
        u32 size = section->Misc.VirtualSize;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            start >= image_size || size > image_size - start ||
            size < ARRAY_COUNT(g_set_unit_name_pattern)) {
            continue;
        }
        for (offset = 0; offset <= size - (u32)ARRAY_COUNT(g_set_unit_name_pattern);
             ++offset) {
            u32 function = start + offset;
            u32 helper;
            u32 string_rva;
            if (!pattern_matches(module + function, g_set_unit_name_pattern,
                                 ARRAY_COUNT(g_set_unit_name_pattern)) ||
                !decode_relative_target(module, image_size, function + 40u, 0xE8, &helper) ||
                helper > image_size - sizeof(g_push_unit_name_prefix) - 4u ||
                memcmp(module + helper, g_push_unit_name_prefix,
                       sizeof(g_push_unit_name_prefix)) != 0 ||
                !read_absolute_rva(module, image_size,
                                   helper + (u32)sizeof(g_push_unit_name_prefix),
                                   &string_rva) ||
                string_rva > image_size - sizeof(unit_name) ||
                memcmp(module + string_rva, unit_name, sizeof(unit_name)) != 0) {
                continue;
            }
            if (++matches > 1) return FALSE;
            panel->set_unit_name_function = function;
            panel->push_unit_name_function = helper;
        }
    }
    if (matches != 1 ||
        !find_unique_readonly_pointer(
            module, nt, (u32)(uintptr_t)(module + panel->set_unit_name_function),
            &panel->set_unit_name_vtable_entry) ||
        panel->set_unit_name_vtable_entry < 4u * sizeof(u32)) {
        return FALSE;
    }
    /* SetState is slot 1 of the same vtable (SetUnitName is slot 5). */
    panel->set_state_vtable_entry = panel->set_unit_name_vtable_entry - 4u * sizeof(u32);
    if (!absolute_operand_to_rva(module, image_size,
                                 load_u32(module + panel->set_state_vtable_entry),
                                 &panel->set_state_function) ||
        panel->set_state_function > image_size - sizeof(g_set_state_prologue) ||
        memcmp(module + panel->set_state_function, g_set_state_prologue,
               sizeof(g_set_state_prologue)) != 0) {
        return FALSE;
    }
    return
           FIND_UNIQUE(module, nt, g_single_unit_update_pattern,
                       &panel->single_unit_update_function) &&
           find_unique_readonly_pointer(
               module, nt, (u32)(uintptr_t)(module + panel->single_unit_update_function),
               &panel->single_unit_update_vtable_entry);
}

/*
 * FXParticleSystem::DefaultDrawModule::Draw: global "rendering disabled"
 * checks, then storage iteration reading each particle position at +0x10.
 */
static const u16 g_particle_default_draw_pattern[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x44,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x53, 0x56, 0x8B, 0xF1, 0x57, 0x89, 0x75, 0xBC,
    0x0F, 0x85, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x0F, 0x85, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x83, 0xB8, 0x68
};

/* Streak/Lightning draw: same "rendering disabled" checks, larger frames. */
static const u16 g_particle_streak_draw_pattern[] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x48, 0x08, 0x00, 0x00,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x56, 0x8B, 0xF1, 0x89, 0x75, 0xBC, 0x0F, 0x85, 0x0C, 0x03, 0x00, 0x00,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x0F, 0x85, 0xFF, 0x02, 0x00, 0x00
};
static const u16 g_particle_lightning_draw_pattern[] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x80, 0x08, 0x00, 0x00,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x56, 0x57, 0x8B, 0xF1, 0x0F, 0x85, 0x66, 0x05, 0x00, 0x00,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x0F, 0x85, 0x59, 0x05, 0x00, 0x00
};
/* RenderObject draw: frame setup, then the 0xBAADBEEF null-system sentinel. */
static const u16 g_particle_render_object_draw_pattern[] = {
    0x55, 0x8D, 0x6C, 0x24, 0x94, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00,
    0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04, 0x85, 0xC0, 0x89, 0x75, 0x08, 0x75, 0x05,
    0xB8, 0xEF, 0xBE, 0xAD, 0xBA, 0x8B, 0xC8,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x84, 0xC0, 0x74, 0x07
};

/* Trail draw: global "trails enabled" flag, then the 0xBAADBEEF sentinel. */
static const u16 g_particle_trail_draw_pattern[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x34,
    0x80, 0x3D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x53, 0x8B, 0xD9, 0x75, 0x07, 0x33, 0xC0, 0xE9, 0xF7, 0x00, 0x00, 0x00,
    0x8B, 0x43, 0x04, 0x85, 0xC0, 0x75, 0x05, 0xB8, 0xEF, 0xBE, 0xAD, 0xBA,
    0x8B, 0x88, 0x84, 0x00, 0x00, 0x00, 0x8B, 0x01, 0x56, 0x57, 0xFF
};

/*
 * Particle manager update: MOV EAX,[GameClient]; CMP byte [EAX+d32],0; JZ;
 * MOV EAX,[GameLogic]; CMP byte [EAX+d32],0; MOV byte [EBP-1],1; JZ; MOV 0.
 */
static const u16 g_particle_simulates_pattern[] = {
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x80, 0xB8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00, 0x74, 0x12,
    0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x80, 0xB8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0xC6, 0x45, 0xFF, 0x01, 0x74, 0x04, 0xC6, 0x45, 0xFF, 0x00
};

/* Trail draw +0x34: MOV EAX,[EBX+10h]; PUSH [EAX+4] (TrailLife); MOV ECX,ESI; CALL trim. */
static const u8 g_trail_draw_trim_call[] = {
    0x8B, 0x43, 0x10, 0xFF, 0x70, 0x04, 0x8B, 0xCE, 0xE8
};
/* TrailSystemStorage::update: system +0x170 stopping flag, records, particles. */
static const u16 g_trail_storage_update_pattern[] = {
    0x8B, 0x46, 0x04, 0x85, 0xC0, 0x57, 0x75, 0x05, 0xB8, 0xEF, 0xBE, 0xAD, 0xBA,
    0x80, 0xB8, 0x70, 0x01, 0x00, 0x00, 0x00, 0x8B, 0x6E, 0x1C, 0x8D, 0x5E, 0x10, 0x8B, 0x3B
};

/* Swarm draw: "swarms enabled" flag, one-time init, then vertex build. */
static const u16 g_particle_swarm_draw_pattern[] = {
    0x53, 0x33, 0xDB, 0x38, 0x1D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x56, 0x8B, 0xF1, 0x75, 0x04, 0x33, 0xC0, 0xEB, 0x77, 0x38, 0x5E, 0x18, 0x75, 0x0C,
    0x8D, 0x4E, 0x14, 0x8B, 0x01, 0xFF, 0x50, 0x04, 0xC6, 0x46, 0x18, 0x01,
    0x39, 0x5E, 0x20, 0x74, 0xE6
};

static BOOL resolve_particle_draw_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    static const struct {
        const u16 *pattern;
        size_t size;
        /* "RET 0Ch" closing each three-argument draw (2012 builds differ). */
        u32 ret_offset;
        u32 ret_offset_alt;
    } kinds[PARTICLE_DRAW_KINDS] = {
        { g_particle_default_draw_pattern, ARRAY_COUNT(g_particle_default_draw_pattern), 0x379u, 0x379u },
        { g_particle_streak_draw_pattern, ARRAY_COUNT(g_particle_streak_draw_pattern), 0x32Cu, 0x32Cu },
        { g_particle_lightning_draw_pattern, ARRAY_COUNT(g_particle_lightning_draw_pattern), 0x585u, 0x585u },
        { g_particle_render_object_draw_pattern,
          ARRAY_COUNT(g_particle_render_object_draw_pattern), 0x4F3u, 0x4F0u },
        { g_particle_trail_draw_pattern, ARRAY_COUNT(g_particle_trail_draw_pattern), 0x112u, 0x112u },
        { g_particle_swarm_draw_pattern, ARRAY_COUNT(g_particle_swarm_draw_pattern), 0x8Bu, 0x8Bu }
    };
    static const u8 ret_0c[] = { 0xC2, 0x0C, 0x00 };
    ParticleDrawLayout *draw = &game->particle_draw;
    u32 i;

    for (i = 0; i < PARTICLE_DRAW_KINDS; ++i) {
        u32 function;
        if (!find_unique_signature(module, nt, kinds[i].pattern, kinds[i].size, &function) ||
            function > game->pe_size_of_image - kinds[i].ret_offset - sizeof(ret_0c) ||
            (memcmp(module + function + kinds[i].ret_offset, ret_0c, sizeof(ret_0c)) != 0 &&
             memcmp(module + function + kinds[i].ret_offset_alt, ret_0c, sizeof(ret_0c)) != 0) ||
            !find_unique_readonly_pointer(module, nt, (u32)(uintptr_t)(module + function),
                                          &draw->draw_vtable_entries[i])) {
            return FALSE;
        }
        draw->draw_functions[i] = function;
    }
    /*
     * The trail wrapper relies on: the draw trimming each live trail to its
     * module data's TrailLife (+0x10 -> +4) before drawing, and the storage
     * update's stopping flag (system +0x170), particle list (+0x10) and
     * records (+0x1C).
     */
    if (memcmp(module + draw->draw_functions[PARTICLE_DRAW_TRAIL] + 0x34u,
               g_trail_draw_trim_call, sizeof(g_trail_draw_trim_call)) != 0 ||
        !FIND_UNIQUE(module, nt, g_trail_storage_update_pattern, &i) ||
        !FIND_UNIQUE(module, nt, g_particle_simulates_pattern, &i) ||
        load_u32(module + i + 1u) !=
            (u32)(uintptr_t)(module + game->drawable_physics.game_client_pointer) ||
        !read_absolute_rva(module, game->pe_size_of_image, i + 15u,
                           &draw->game_logic_pointer)) {
        return FALSE;
    }
    draw->client_simulates_offset = load_u32(module + i + 7u);
    draw->logic_paused_offset = load_u32(module + i + 21u);
    return draw->client_simulates_offset < 0x1000u && draw->logic_paused_offset < 0x1000u;
}

/* W3DTracerManager update: "if (GameClient frame == +0x90) return". */
static const u16 g_tracer_manager_update_pattern[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x56, 0x8B, 0xF1,
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0x57, 0xFF, 0x50, 0x78, 0x3B, 0x86, 0x90, 0x00, 0x00, 0x00, 0x0F, 0x84
};
/* Tracer movement/expiry pass over the instance list at manager +0x3C. */
static const u16 g_tracer_movement_pass_pattern[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x56, 0x8B, 0xF1, 0x57,
    0x8D, 0x45, 0xF8, 0x8D, 0x5E, 0x3C, 0x50, 0x8B, 0xCB, 0xE8
};
/* PUSH frame; LEA ECX,[EDI+8]; CALL tracer update; TEST AL,AL; JNZ +15h. */
static const u16 g_tracer_update_call_pattern[] = {
    0xFF, 0x75, 0x08, 0x8D, 0x4F, 0x08,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x84, 0xC0, 0x75, 0x15, 0x8B, 0x47, 0x78
};
/* Per-tracer update: "if (frame < expiry(+0x6C))" then move by speed (+0x58). */
static const u8 g_tracer_update_prologue[] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xEC, 0x48, 0x56, 0x8B, 0xF1,
    0x3B, 0x46, 0x6C, 0x0F, 0x83
};

static BOOL resolve_tracer_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    static const u16 thunk_pattern[] = { 0x8B, 0x49, 0x1C, 0xE9 };
    TracerLayout *tracers = &game->tracers;
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 image_size = game->pe_size_of_image;
    u32 call;
    u32 matches = 0;
    u16 section_index;

    if (!FIND_UNIQUE(module, nt, g_tracer_manager_update_pattern,
                     &tracers->manager_update_function) ||
        !FIND_UNIQUE(module, nt, g_tracer_movement_pass_pattern,
                     &tracers->movement_pass_function) ||
        !FIND_UNIQUE(module, nt, g_tracer_update_call_pattern, &call) ||
        call - tracers->movement_pass_function != 0x2Bu) {
        return FALSE;
    }
    tracers->tracer_update_call.instruction = call + 6u;
    if (!decode_relative_target(module, image_size, tracers->tracer_update_call.instruction,
                                0xE8, &tracers->tracer_update_call.target) ||
        tracers->tracer_update_call.target > image_size - sizeof(g_tracer_update_prologue) ||
        memcmp(module + tracers->tracer_update_call.target, g_tracer_update_prologue,
               sizeof(g_tracer_update_prologue)) != 0) {
        return FALSE;
    }

    /* "MOV ECX,[ECX+1Ch]; JMP update": exactly one such thunk enters it. */
    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start = section->VirtualAddress;
        u32 size = section->Misc.VirtualSize;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            start >= image_size || size > image_size - start || size < 8u) {
            continue;
        }
        for (offset = 0; offset <= size - 8u; ++offset) {
            u32 target;
            if (!pattern_matches(module + start + offset, thunk_pattern,
                                 ARRAY_COUNT(thunk_pattern)) ||
                !decode_relative_target(module, image_size, start + offset + 3u, 0xE9, &target) ||
                target != tracers->manager_update_function) {
                continue;
            }
            if (++matches > 1) return FALSE;
            tracers->manager_update_jump.instruction = start + offset + 3u;
            tracers->manager_update_jump.target = target;
        }
    }
    return matches == 1;
}

/*
 * MOVSS XMM2,[limit]; COMISS; MOVAPS; ADDSS r,[progress]; MOVSS [progress],r;
 * JBE +14h; MOVSS XMM2,[accel]; ADDSS. The two phases differ in registers.
 */
static const u16 g_camera_ease_phase_a_pattern[] = {
    0xF3, 0x0F, 0x10, 0x15, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x0F, 0x2F, 0xD0, 0x0F, 0x28, 0xC8,
    0xF3, 0x0F, 0x58, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x11, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x76, 0x14, 0xF3, 0x0F, 0x10, 0x15, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x58, 0xD0
};
static const u16 g_camera_ease_phase_b_pattern[] = {
    0xF3, 0x0F, 0x10, 0x15, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x0F, 0x2F, 0xD1, 0x0F, 0x28, 0xC1,
    0xF3, 0x0F, 0x58, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x11, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x76, 0x14, 0xF3, 0x0F, 0x10, 0x15, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xF3, 0x0F, 0x58, 0xD1
};

static BOOL resolve_camera_ease_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    CameraEaseLayout *ease = &game->camera_ease;
    u32 blocks[2];
    u32 i;

    if (!FIND_UNIQUE(module, nt, g_camera_ease_phase_a_pattern, &blocks[0]) ||
        !FIND_UNIQUE(module, nt, g_camera_ease_phase_b_pattern, &blocks[1])) {
        return FALSE;
    }
    for (i = 0; i < 2; ++i) {
        u32 limit;
        u32 accel;
        /* Both progress operands must name one global; constants in the image. */
        if (load_u32(module + blocks[i] + 18u) != load_u32(module + blocks[i] + 26u) ||
            !read_absolute_rva(module, game->pe_size_of_image, blocks[i] + 4u, &limit) ||
            !read_absolute_rva(module, game->pe_size_of_image, blocks[i] + 36u, &accel)) {
            return FALSE;
        }
        ease->limit_operands[i] = blocks[i] + 4u;
        ease->accel_operands[i] = blocks[i] + 36u;
    }
    return TRUE;
}

/*
 * Per-frame shroud texture uploads (TW 1.10 VA 0x834BF4 / 0x834D47): call
 * the time-based cell fade, then lock the texture and copy the cell rows.
 * The two share everything up to the row copy, whose scale (2 or 4 bytes
 * per cell) tells them apart. The fade CALL is at +3; the copy CALL at +86.
 */
#define SHROUD_UPLOAD_PREFIX                                                       \
    0x50, 0x8B, 0xCE, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,   \
    0x80, 0x7E, 0x35, 0x00, 0x74, 0x14, 0x8D, 0x45, PATTERN_ANY, 0x50,             \
    0x0F, 0xB6, 0x46, 0x36, 0x50, 0x8B, 0xCE, 0xC6, 0x46, 0x35, 0x00,             \
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,                     \
    0x8B, 0x45, PATTERN_ANY, 0x8B, 0x7E, 0x18, 0x40, 0x50, 0x43, 0x53,             \
    0x6A, 0x01, 0x6A, 0x01, 0x8D, 0x45, PATTERN_ANY, 0x50, 0x8D, 0x4D, PATTERN_ANY, \
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,                     \
    0x8B, 0xD8, 0x8B, 0x45, PATTERN_ANY, 0x85, 0xC0, 0x89, 0x45, PATTERN_ANY,     \
    0x7E, PATTERN_ANY, 0x8B, 0x45, PATTERN_ANY
static const u16 g_shroud_upload_pattern[] = {
    SHROUD_UPLOAD_PREFIX,
    0x03, 0xC0, 0x89, 0x45, PATTERN_ANY, 0xFF, 0x75, PATTERN_ANY, 0x57, 0x53,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x03, 0x5D, PATTERN_ANY, 0x8B, 0x06, 0x83, 0xC4, 0x0C, 0xFF, 0x4D, PATTERN_ANY,
    0x83, 0x7D, PATTERN_ANY, 0x00, 0x8D, 0x3C, 0x47, 0x7F, 0xE2
};
static const u16 g_fog_upload_pattern[] = {
    SHROUD_UPLOAD_PREFIX,
    0xC1, 0xE0, 0x02, 0x89, 0x45, PATTERN_ANY, 0xFF, 0x75, PATTERN_ANY, 0x57, 0x53,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x03, 0x5D, PATTERN_ANY, 0x8B, 0x06, 0x83, 0xC4, 0x0C, 0xFF, 0x4D, PATTERN_ANY,
    0x83, 0x7D, PATTERN_ANY, 0x00, 0x8D, 0x3C, 0x87, 0x7F, 0xE2
};
#undef SHROUD_UPLOAD_PREFIX

/*
 * Inside each fade: elapsed = timeGetTime() - stamp; stamp += elapsed;
 * step = ftol(elapsed * rate); if (step > cap) step = cap.
 * Stamp operands at +7/+19, rate at +41, cap at +54. Then the fade loads
 * the shown (+0x3C) and target (+0x38) level arrays.
 */
static const u16 g_shroud_fade_timing_pattern[] = {
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x2B, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x0F, 0x84, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x01, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xC0, 0x89, 0x45, PATTERN_ANY, 0xDB, 0x45, PATTERN_ANY, 0x7D, 0x06,
    0xD8, 0x05, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD8, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x89, 0x45, PATTERN_ANY, 0xB8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x39, 0x45, PATTERN_ANY, 0x7E, 0x03, 0x89, 0x45, PATTERN_ANY,
    0x80, 0x7F, PATTERN_ANY, 0x00, 0x8B, 0x4F, SHROUD_SHOWN_LEVELS,
    0x8B, 0x47, SHROUD_TARGET_LEVELS
};

/* Shroud texture creation: PUSH 0; 1; 1; PUSH 1Ah (A4R4G4B4); height; width. */
static const u16 g_shroud_texture_format_pattern[] = {
    0x6A, 0x00, 0x33, 0xDB, 0x43, 0x53, 0x53, 0x6A, 0x1A, 0xFF, 0x76, 0x24,
    0x8D, 0x7E, 0x1C, 0xFF, 0x76, 0x20, 0x8B, 0xCF, 0xE8,
    PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x8B, 0xCF, 0xE8
};

/*
 * Whole shroud texture fill (TW 1.10 VA 0x813D90), entered from the shroud
 * upload: clamp the level to GlobalData's minimum (+10), convert it (+30),
 * lock (+49), store one 16-bit value per texel, unlock (+92), RET 8.
 */
static const u16 g_shroud_fill_pattern[] = {
    0x55, 0x8B, 0xEC, 0xA1, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8A, 0x80, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x38, 0x45, 0x08, 0x53, 0x56, 0x57, 0x8B, 0xF1, 0x73, 0x03, 0x88, 0x45, 0x08,
    0xFF, 0x75, 0x08, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x59, 0x8B, 0x4D, 0x0C, 0x0F, 0xB7, 0xF8, 0x33, 0xDB, 0x53, 0x8D, 0x45, 0x08, 0x50,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x39, 0x5E, 0x24, 0x7E, 0x1E, 0x8B, 0x56, 0x20, 0x33, 0xC9, 0x85, 0xD2, 0x7E, 0x0C,
    0x66, 0x89, 0x3C, 0x48, 0x8B, 0x56, 0x20, 0x41, 0x3B, 0xCA, 0x7C, 0xF4,
    0x03, 0x45, 0x08, 0x43, 0x3B, 0x5E, 0x24, 0x7C, 0xE5, 0x8B, 0x4D, 0x0C,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x5F, 0x5E, 0x5B, 0x5D, 0xC2, 0x08, 0x00
};

/*
 * Level -> ARGB4444 converter (TW 1.10 VA 0x813CDD): blue/green/red =
 * ftol(level * GlobalData float at +27/+40/+55); alpha = min(255,
 * max(0, level - 4) * 255 / 46); level 255 gives 0xFFFF.
 */
static const u16 g_shroud_cell_converter_pattern[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x53, 0x56,
    0x8B, 0x35, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x57, 0x0F, 0xB6, 0x7D, 0x08, 0x89, 0x7D, 0xFC, 0xDB, 0x45, 0xFC,
    0xD9, 0x86, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0xD8, 0xC9,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD9, 0x86, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0xD8, 0xC9, 0x8B, 0xD8,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xD8, 0x8E, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x89, 0x45, 0xF0,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x83, 0x65, 0xF8, 0x00, 0x8B, 0xF0, 0x8D, 0x47, 0xFC, 0xB9, 0xFF, 0x00, 0x00, 0x00,
    0x89, 0x45, 0xFC, 0x85, 0xC0, 0x89, 0x4D, 0xF4, 0x8D, 0x45, 0xFC, 0x7F, 0x03,
    0x8D, 0x45, 0xF8, 0x8B, 0x00, 0x69, 0xC0, 0xFF, 0x00, 0x00, 0x00, 0x99, 0x6A, 0x2E
};

static BOOL resolve_shroud_fade(u8 *module, u32 image_size, u32 upload,
                                ShroudFadeSite *fade) {
    u32 offset;
    u32 timing = 0;
    u32 matches = 0;
    float rate;

    fade->fade_call.instruction = upload + 3u;
    if (!decode_relative_target(module, image_size, fade->fade_call.instruction,
                                0xE8, &fade->fade_call.target) ||
        fade->fade_call.target > image_size - 0x100u) {
        return FALSE;
    }
    /* The timing block sits right after the fade's first-call initialisation. */
    for (offset = 0; offset < 0x60u; ++offset) {
        if (pattern_matches(module + fade->fade_call.target + offset,
                            g_shroud_fade_timing_pattern,
                            ARRAY_COUNT(g_shroud_fade_timing_pattern))) {
            timing = fade->fade_call.target + offset;
            ++matches;
        }
    }
    if (matches != 1 ||
        load_u32(module + timing + 7u) != load_u32(module + timing + 19u) ||
        !read_absolute_rva(module, image_size, timing + 7u, &fade->timestamp) ||
        !read_absolute_rva(module, image_size, timing + 41u, &fade->rate)) {
        return FALSE;
    }
    fade->step_cap = load_u32(module + timing + 54u);
    memcpy(&rate, module + fade->rate, sizeof(rate));
    return fade->step_cap >= 1u && fade->step_cap <= 0xFFu &&
           rate > 0.001f && rate < 10.0f;
}

static BOOL resolve_shroud_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    ShroudLayout *shroud = &game->shroud;
    u32 image_size = game->pe_size_of_image;
    u32 global_data = (u32)(uintptr_t)(module + game->timing.global_data_pointer);
    u32 shroud_upload;
    u32 fog_upload;
    u32 match;
    u32 fill;
    u32 converter;
    u32 i;

    if (!FIND_UNIQUE(module, nt, g_shroud_upload_pattern, &shroud_upload) ||
        !FIND_UNIQUE(module, nt, g_fog_upload_pattern, &fog_upload) ||
        !resolve_shroud_fade(module, image_size, shroud_upload,
                             &shroud->fades[SHROUD_TEXTURE_SHROUD]) ||
        !resolve_shroud_fade(module, image_size, fog_upload,
                             &shroud->fades[SHROUD_TEXTURE_FOG]) ||
        shroud->fades[SHROUD_TEXTURE_SHROUD].timestamp ==
            shroud->fades[SHROUD_TEXTURE_FOG].timestamp) {
        return FALSE;
    }
    /* Both row copies call the same memcpy. */
    shroud->fog_row_copy_call.instruction = fog_upload + 86u;
    shroud->shroud_row_copy_call.instruction = shroud_upload + 85u;
    if (!decode_relative_target(module, image_size,
                                shroud->shroud_row_copy_call.instruction, 0xE8,
                                &shroud->shroud_row_copy_call.target) ||
        !decode_relative_target(module, image_size,
                                shroud->fog_row_copy_call.instruction, 0xE8,
                                &shroud->fog_row_copy_call.target) ||
        shroud->shroud_row_copy_call.target != shroud->fog_row_copy_call.target) {
        return FALSE;
    }

    if (!FIND_UNIQUE(module, nt, g_shroud_texture_format_pattern, &match)) return FALSE;
    shroud->shroud_format_immediate = match + 8u;

    /* The shroud upload's "+0x35 dirty" call is the whole-texture fill. */
    shroud->shroud_fill_call.instruction = shroud_upload + 29u;
    if (!decode_relative_target(module, image_size, shroud->shroud_fill_call.instruction,
                                0xE8, &shroud->shroud_fill_call.target)) {
        return FALSE;
    }
    fill = shroud->shroud_fill_call.target;
    if (fill > image_size - ARRAY_COUNT(g_shroud_fill_pattern) ||
        !pattern_matches(module + fill, g_shroud_fill_pattern,
                         ARRAY_COUNT(g_shroud_fill_pattern)) ||
        load_u32(module + fill + 4u) != global_data ||
        !decode_relative_target(module, image_size, fill + 30u, 0xE8, &converter) ||
        !decode_relative_target(module, image_size, fill + 49u, 0xE8,
                                &shroud->texture_lock_function) ||
        !decode_relative_target(module, image_size, fill + 92u, 0xE8,
                                &shroud->texture_unlock_function) ||
        converter > image_size - ARRAY_COUNT(g_shroud_cell_converter_pattern) ||
        !pattern_matches(module + converter, g_shroud_cell_converter_pattern,
                         ARRAY_COUNT(g_shroud_cell_converter_pattern)) ||
        load_u32(module + converter + 10u) != global_data) {
        return FALSE;
    }
    shroud->global_minimum_level = load_u32(module + fill + 10u);
    shroud->global_colour_scale[0] = load_u32(module + converter + 55u); /* red */
    shroud->global_colour_scale[1] = load_u32(module + converter + 40u); /* green */
    shroud->global_colour_scale[2] = load_u32(module + converter + 27u); /* blue */
    for (i = 0; i < 3; ++i) {
        if (shroud->global_colour_scale[i] > 0x4000u) return FALSE;
    }
    return shroud->global_minimum_level <= 0x4000u;
}

/*
 * TintEnvelope::update (TW 1.10 VA 0x474D1F): switch on the state byte
 * (+0x38); sustain decrements its countdown (+0x34) once per call.
 */
static const u16 g_tint_envelope_update_pattern[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x8B, 0xF1,
    0x0F, 0xBE, 0x46, 0x38, 0x83, 0xE8, 0x00, 0x57,
    0x0F, 0x84, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x48, 0x0F, 0x84, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x48, 0x74, 0x20, 0x48, 0x0F, 0x85, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x46, 0x34, 0x85, 0xC0, 0x76, 0x09, 0x48, 0x89, 0x46, 0x34
};

/*
 * Drawable::updateDrawable's repeated flash: GameClient frame (virtual +0x78)
 * DIV [interval]; flash only on a zero remainder. Interval operand at +9.
 */
static const u16 g_flash_interval_pattern[] = {
    0x8B, 0x01, 0xFF, 0x50, 0x78, 0x33, 0xD2,
    0xF7, 0x35, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x85, 0xD2, 0x75, PATTERN_ANY, 0xFF, 0xB6
};

static BOOL resolve_tint_envelope_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    TintEnvelopeLayout *tint = &game->tint_envelope;
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 image_size = game->pe_size_of_image;
    u16 initializer[12];
    u32 client_fps = (u32)(uintptr_t)(module + game->timing.client_fps);
    u32 interval;
    u32 match;
    u32 calls = 0;
    u16 section_index;
    u32 i;

    if (!FIND_UNIQUE(module, nt, g_tint_envelope_update_pattern, &tint->update_function) ||
        !FIND_UNIQUE(module, nt, g_flash_interval_pattern, &match) ||
        !read_absolute_rva(module, image_size, match + 9u, &tint->flash_interval)) {
        return FALSE;
    }
    /* Its only writer: MOV EAX,[client FPS]; SHR EAX,1; MOV [interval],EAX. */
    interval = load_u32(module + match + 9u);
    initializer[0] = 0xA1;
    initializer[5] = 0xD1;
    initializer[6] = 0xE8;
    initializer[7] = 0xA3;
    for (i = 0; i < 4; ++i) {
        initializer[1 + i] = (u16)((client_fps >> (8u * i)) & 0xFFu);
        initializer[8 + i] = (u16)((interval >> (8u * i)) & 0xFFu);
    }
    if (!FIND_UNIQUE(module, nt, initializer, &match)) return FALSE;
    /*
     * Exactly two direct branches, both from Drawable::updateDrawable's path:
     * a CALL (+0x190 tint) and a tail JMP (+0x194 selection flash).
     */
    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start = section->VirtualAddress;
        u32 size = section->Misc.VirtualSize;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            start >= image_size || size > image_size - start || size < 5u) {
            continue;
        }
        for (offset = 0; offset <= size - 5u; ++offset) {
            u8 opcode = module[start + offset];
            u32 target;
            if ((opcode != 0xE8 && opcode != 0xE9) ||
                !decode_relative_target(module, image_size, start + offset, opcode, &target) ||
                target != tint->update_function) {
                continue;
            }
            if (calls >= TINT_ENVELOPE_UPDATE_CALLS) return FALSE;
            tint->update_calls[calls].instruction = start + offset;
            tint->update_calls[calls].target = target;
            ++calls;
        }
    }
    return calls == TINT_ENVELOPE_UPDATE_CALLS;
}

/* Radar look-at: ECX = [this+4]; convert(&world, radar); [view]->lookAt(&world). */
static const u16 g_minimap_look_at_pattern[] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x49, 0x04, 0x83, 0xEC, 0x0C, 0xFF, 0x75, 0x08,
    0x8D, 0x45, 0xF4, 0x50, 0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x01, 0x8D, 0x55, 0xF4, 0x52, 0xFF, 0x50, 0x48, 0xC9, 0xC2, 0x04, 0x00
};

/*
 * LookAtDragOperation mouse move: "if (point != stored point (+0x0C))" then
 * look at it; the look-at CALL is at +0x26.
 */
static const u8 g_minimap_move_prologue[] = {
    0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0xF3, 0x0F, 0x10, 0x07, 0x8B, 0xF1,
    0x0F, 0x2E, 0x46, 0x0C
};

/* Every direct CALL/JMP rel32 to `target` in executable sections (at most `max`). */
static BOOL find_branches_to(u8 *module, IMAGE_NT_HEADERS32 *nt, u32 image_size, u32 target,
                             BranchSite *out, u32 max, u32 *out_count) {
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 count = 0;
    u16 section_index;

    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start = section->VirtualAddress;
        u32 size = section->Misc.VirtualSize;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            start >= image_size || size > image_size - start || size < 5u) {
            continue;
        }
        for (offset = 0; offset <= size - 5u; ++offset) {
            u8 opcode = module[start + offset];
            u32 decoded;
            if ((opcode != 0xE8 && opcode != 0xE9) ||
                !decode_relative_target(module, image_size, start + offset, opcode, &decoded) ||
                decoded != target) {
                continue;
            }
            if (count >= max) return FALSE;
            out[count].instruction = start + offset;
            out[count].target = target;
            ++count;
        }
    }
    *out_count = count;
    return TRUE;
}

static BOOL is_minimap_move_call(const u8 *module, BranchSite call) {
    return call.instruction >= 0x26u &&
           memcmp(module + call.instruction - 0x26u, g_minimap_move_prologue,
                  sizeof(g_minimap_move_prologue)) == 0;
}

static BOOL resolve_minimap_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    MinimapLayout *minimap = &game->minimap;
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 image_size = game->pe_size_of_image;
    u16 section_index;

    minimap->count = 0;
    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start = section->VirtualAddress;
        u32 size = section->Misc.VirtualSize;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            start >= image_size || size > image_size - start ||
            size < ARRAY_COUNT(g_minimap_look_at_pattern)) {
            continue;
        }
        for (offset = 0; offset <= size - ARRAY_COUNT(g_minimap_look_at_pattern); ++offset) {
            u32 function = start + offset;
            u32 index = minimap->count;
            u32 view;
            u32 calls;
            if (!pattern_matches(module + function, g_minimap_look_at_pattern,
                                 ARRAY_COUNT(g_minimap_look_at_pattern))) {
                continue;
            }
            /* Two direct CALLs each: the drag-starting click and every drag move. */
            if (index >= MINIMAP_LOOK_AT_MAX ||
                !decode_relative_target(module, image_size, function + 16u, 0xE8,
                                        &minimap->converters[index]) ||
                !read_absolute_rva(module, image_size, function + 23u, &view) ||
                (index > 0 && view != minimap->view_pointer) ||
                !find_branches_to(module, nt, image_size, function, minimap->calls[index],
                                  MINIMAP_LOOK_AT_CALLS, &calls) ||
                calls != MINIMAP_LOOK_AT_CALLS ||
                module[minimap->calls[index][0].instruction] != 0xE8 ||
                module[minimap->calls[index][1].instruction] != 0xE8) {
                return FALSE;
            }
            /* Exactly one call is the drag move; order them click, move. */
            {
                BOOL first_is_move = is_minimap_move_call(module, minimap->calls[index][0]);
                BOOL second_is_move = is_minimap_move_call(module, minimap->calls[index][1]);
                if (first_is_move == second_is_move) return FALSE;
                if (first_is_move) {
                    BranchSite swap = minimap->calls[index][0];
                    minimap->calls[index][0] = minimap->calls[index][1];
                    minimap->calls[index][1] = swap;
                }
            }
            minimap->view_pointer = view;
            ++minimap->count;
        }
    }
    return minimap->count >= 1u;
}

/* Per-object waypoint-line callback: skip hidden objects, then build its line. */
static const u16 g_waypoint_line_site_pattern[] = {
    0x8B, 0x4C, 0x24, 0x04, 0x85, 0xC9, 0x74, 0x1F, 0x8B, 0x41, 0x04,
    0x8B, 0x80, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xC1, 0xE8, 0x11, 0xA8, 0x01, 0x75, 0x0F, 0x8B, 0x44, 0x24, 0x08,
    0xFF, 0x70, 0x04, 0x51, 0x8B, 0x08, 0xE8
};
/* Line builder: ... MOVSS XMM0,[ESI+pos] / +4 / +8 copies the start point. */
static const u16 g_waypoint_line_builder_pattern[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C, 0x53, 0x56, 0x8B, 0x75, 0x08, 0x85, 0xF6,
    0x8B, 0xD9, 0x0F, 0x84, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x57,
    0xFF, 0xB6, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0xE8, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY,
    0x8B, 0xF8, 0x85, 0xFF, 0x89, 0x7D, 0xEC, 0x0F, 0x84, PATTERN_ANY, PATTERN_ANY,
    PATTERN_ANY, PATTERN_ANY, 0x8B, 0x47, 0x04, 0x39, 0x00,
    0x0F, 0x84, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x83, 0x63, 0x1C, 0x00,
    0xF3, 0x0F, 0x10, 0x46, PATTERN_ANY, 0xF3, 0x0F, 0x11, 0x45, 0xD8,
    0xF3, 0x0F, 0x10, 0x46, PATTERN_ANY, 0xF3, 0x0F, 0x11, 0x45, 0xDC,
    0xF3, 0x0F, 0x10, 0x46, PATTERN_ANY
};
/* Drawable::getInterpolatedTransform: no Object (+off) -> own matrix (+8). */
static const u16 g_interpolated_transform_pattern[] = {
    0x56, 0x8B, 0xF1, 0x83, 0xBE, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x00,
    0x75, 0x05, 0x8D, 0x46, 0x08, 0x5E, 0xC3, 0x8B, 0x0D, PATTERN_ANY, PATTERN_ANY,
    PATTERN_ANY, PATTERN_ANY, 0x8B, 0x01, 0x53, 0xFF, 0x50, 0x78
};
/* Particle attach: EDI = Object's Drawable (+off); ...; getInterpolatedTransform(EDI). */
static const u16 g_object_drawable_pattern[] = {
    0x8B, 0xBE, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x83, 0xF8, 0x03,
    0x0F, 0x9D, 0x45, 0xFF, 0x85, 0xFF, 0x74, PATTERN_ANY,
    0xF6, 0x87, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, PATTERN_ANY, 0x20, 0x75, 0x04,
    0xC6, 0x45, 0xFE, 0x01, 0x8B, 0xCF, 0xE8
};

static BOOL resolve_waypoint_line_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    WaypointLineLayout *lines = &game->waypoint_lines;
    u32 image_size = game->pe_size_of_image;
    u32 match;
    u32 builder;
    u32 interp_call;

    if (!FIND_UNIQUE(module, nt, g_waypoint_line_site_pattern, &match)) return FALSE;
    lines->line_call.instruction = match + 34u;
    if (!decode_relative_target(module, image_size, lines->line_call.instruction, 0xE8,
                                &lines->line_call.target)) {
        return FALSE;
    }
    builder = lines->line_call.target;
    if (builder > image_size - ARRAY_COUNT(g_waypoint_line_builder_pattern) ||
        !pattern_matches(module + builder, g_waypoint_line_builder_pattern,
                         ARRAY_COUNT(g_waypoint_line_builder_pattern))) {
        return FALSE;
    }
    /* The three MOVSS loads must read consecutive floats. */
    lines->object_position = module[builder + 71u];
    if (module[builder + 81u] != lines->object_position + 4u ||
        module[builder + 91u] != lines->object_position + 8u) {
        return FALSE;
    }

    if (!FIND_UNIQUE(module, nt, g_interpolated_transform_pattern,
                     &lines->interpolated_transform_function) ||
        !FIND_UNIQUE(module, nt, g_object_drawable_pattern, &match) ||
        !decode_relative_target(module, image_size, match + 32u, 0xE8, &interp_call) ||
        interp_call != lines->interpolated_transform_function) {
        return FALSE;
    }
    lines->drawable_object = load_u32(module + lines->interpolated_transform_function + 5u);
    lines->object_drawable = load_u32(module + match + 2u);
    return lines->drawable_object < 0x1000u && lines->object_drawable < 0x1000u &&
           lines->object_position < 0x100u;
}

static BOOL resolve_objective_ui_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    ObjectiveUiLayout *ui = &game->objective_ui;
    static const u8 thiscall_ret8_tail[] = { 0x5F, 0x8B, 0xC6, 0x5E, 0x5D, 0xC2, 0x08, 0x00 };
    const u16 *patterns[ADD_INDICATOR_COUNT] = {
        g_add_object_indicator_pattern,
        g_add_offscreen_indicator_pattern,
        g_add_waypoint_indicator_pattern
    };
    u32 match;
    u32 slot;
    u32 i;

    /* CMP [g_aptPlayer],EBX / MOV [g_aptPlayer],ESI must name one global. */
    if (!FIND_UNIQUE(module, nt, g_apt_player_singleton_pattern, &match) ||
        load_u32(module + match + 14u) != load_u32(module + match + 22u) ||
        !read_absolute_rva(module, game->pe_size_of_image, match + 14u,
                           &ui->apt_player_pointer)) {
        return FALSE;
    }

    for (i = 0; i < ADD_INDICATOR_COUNT; ++i) {
        /* All three patterns share one length; only immediates differ. */
        if (!find_unique_signature(module, nt, patterns[i],
                                   ARRAY_COUNT(g_add_object_indicator_pattern),
                                   &ui->add_indicator_functions[i]) ||
            memcmp(module + ui->add_indicator_functions[i] + 115u - sizeof(thiscall_ret8_tail),
                   thiscall_ret8_tail, sizeof(thiscall_ret8_tail)) != 0) {
            return FALSE;
        }
    }

    /* The stage vtable stores the three methods in consecutive slots 0..2. */
    if (!find_unique_readonly_pointer(
            module, nt, (u32)(uintptr_t)(module + ui->add_indicator_functions[0]),
            &slot)) {
        return FALSE;
    }
    for (i = 0; i < ADD_INDICATOR_COUNT; ++i) {
        ui->add_indicator_vtable_entries[i] = slot + i * sizeof(u32);
        if (load_u32(module + ui->add_indicator_vtable_entries[i]) !=
            (u32)(uintptr_t)(module + ui->add_indicator_functions[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * DispatchLogicPhase: push registers; EDI = phase; CMP EDI,1; MOV EBP,ECX; JNZ.
 * Tiberium Wars saves EBP/ESI/EDI; Kane's Wrath also EBX (and sets BL = 1).
 */
static const u16 g_dispatch_logic_phase_tw_pattern[] = {
    0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x83, 0xFF, 0x01, 0x8B, 0xE9, 0x0F, 0x85
};
static const u16 g_dispatch_logic_phase_kw_pattern[] = {
    0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x83, 0xFF, 0x01, 0x8B, 0xE9,
    0xB3, 0x01, 0x0F, 0x85
};

/* Every read-only dword equal to `value`; FALSE if there are more than `max`. */
static BOOL find_readonly_pointers(u8 *module, IMAGE_NT_HEADERS32 *nt, u32 value,
                                   u32 *out_rvas, u32 max, u32 *out_count) {
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    u32 count = 0;
    u16 section_index;

    for (section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        u32 start = section->VirtualAddress;
        u32 size = section->Misc.VirtualSize;
        u32 offset;
        if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            (section->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) != 0 ||
            start >= nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - start || size < sizeof(u32)) {
            continue;
        }
        for (offset = 0; offset <= size - sizeof(u32); offset += sizeof(u32)) {
            if (load_u32(module + start + offset) != value) continue;
            if (count >= max) return FALSE;
            out_rvas[count++] = start + offset;
        }
    }
    *out_count = count;
    return TRUE;
}

static BOOL resolve_scheduler_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    SchedulerLayout *scheduler = &game->scheduler;
    u32 match;
    u32 client_fps_address = (u32)(uintptr_t)(module + game->timing.client_fps);
    u32 logic_fps_address = (u32)(uintptr_t)(module + game->timing.logic_fps);

    if (!FIND_UNIQUE(module, nt, g_phase_batch_pattern,
                     &scheduler->phase_batch_block) ||
        !FIND_UNIQUE(module, nt, g_phase_interpolation_pattern,
                     &scheduler->phase_interpolation_function) ||
        !FIND_UNIQUE(module, nt, g_client_slice_flush_pattern, &match) ||
        load_u32(module + match + 1u) != client_fps_address ||
        load_u32(module + match + 9u) != logic_fps_address) {
        return FALSE;
    }

    /*
     * GameLogic_UpdatePhase block layout (KW 1.02 Steam VA 0x005EF10F):
     * +1  = &g_clientUpdateFPS, +9 = &g_logicFPS, +34 = CALL sub_6DE5B1.
     * Proving both operands prevents an unrelated pair of integer divisions
     * from being accepted as the phase-end flush.
     */
    scheduler->client_slice_flush.instruction = match + 34u;
    if (!decode_relative_target(module, game->pe_size_of_image,
                                scheduler->client_slice_flush.instruction, 0xE8,
                                &scheduler->client_slice_flush.target)) {
        return FALSE;
    }

    /* The batch block lies inside DispatchLogicPhase, a short way past its entry. */
    if ((!FIND_UNIQUE(module, nt, g_dispatch_logic_phase_tw_pattern,
                      &scheduler->dispatch_function) &&
         !FIND_UNIQUE(module, nt, g_dispatch_logic_phase_kw_pattern,
                      &scheduler->dispatch_function)) ||
        scheduler->phase_batch_block <= scheduler->dispatch_function ||
        scheduler->phase_batch_block - scheduler->dispatch_function > 0x200u ||
        !find_readonly_pointers(module, nt,
                                (u32)(uintptr_t)(module + scheduler->dispatch_function),
                                scheduler->dispatch_vtable_entries, 2u, &match)) {
        return FALSE;
    }
    return match == 2u;
}

static BOOL resolve_w3d_clock_sites(
    u8 *module, IMAGE_NT_HEADERS32 *nt, GameLayout *game) {
    W3DClockLayout *clock = &game->w3d_clock;
    TimingLayout *timing = &game->timing;
    u32 special;
    u32 normal;
    u32 accumulated;
    u32 expected_step = (u32)(uintptr_t)(module + timing->w3d_milliseconds_per_frame);
    u32 expected_accumulated;

    if (!FIND_UNIQUE(module, nt, g_w3d_special_advance_pattern, &special) ||
        !FIND_UNIQUE(module, nt, g_w3d_normal_advance_pattern, &normal) ||
        !read_absolute_rva(module, game->pe_size_of_image, special + 1u,
                           &accumulated)) {
        return FALSE;
    }

    /*
     * Special block: MOV EAX,[accum]; ADD EAX,[step]; PUSH EAX; MOV [accum],EAX.
     * Normal block:  MOV EAX,[step]; IMUL EAX,ESI; ADD [accum],EAX; PUSH [accum].
     * ESI is the client-frame delta in W3DDisplay_RenderAndPresentFrame.
     */
    expected_accumulated = (u32)(uintptr_t)(module + accumulated);
    if (load_u32(module + special + 7u) != expected_step ||
        load_u32(module + special + 13u) != expected_accumulated ||
        load_u32(module + normal + 1u) != expected_step ||
        load_u32(module + normal + 10u) != expected_accumulated ||
        load_u32(module + normal + 16u) != expected_accumulated) {
        return FALSE;
    }

    clock->special_advance_block = special;
    clock->normal_advance_block = normal;
    timing->w3d_accumulated_time_ms = accumulated;
    return TRUE;
}

GameResolveResult resolve_game_layout(GameLayout *out_game, u8 *module) {
    IMAGE_NT_HEADERS32 *nt;
    GameLayout game;

    if (out_game == NULL || !get_nt_headers(module, &nt)) return GAME_INVALID_PE;
    memset(&game, 0, sizeof(game));
    game.module = module;
    game.pe_timestamp = nt->FileHeader.TimeDateStamp;
    game.pe_entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
    game.pe_size_of_image = nt->OptionalHeader.SizeOfImage;

    /*
     * Resolver order is intentional: later subsystems validate operands
     * against globals found by earlier ones. Timestamp and entry point are
     * diagnostic only; instruction semantics decide compatibility.
     */
    if (!resolve_bootstrap_sites(module, nt, &game)) {
        return GAME_UNSUPPORTED_BUILD;
    }
    if (!resolve_keyboard_sites(module, nt, &game) ||
        !resolve_visual_sites(module, nt, &game) ||
        !resolve_pacing_sites(module, nt, &game)) {
        return GAME_UNSUPPORTED_BUILD;
    }
    /*
     * Resolve camera input after GlobalData so the rotation operand can be
     * proved to reference the same singleton used by the live timing code.
     * TW 1.09/1.10 and all supported KW builds share these exact functions.
     */
    if (!resolve_camera_input_sites(module, nt, &game)) {
        return GAME_UNSUPPORTED_BUILD;
    }
    if (!resolve_cached_timing_values(module, nt, &game) ||
        !resolve_audio_sites(module, nt, &game) ||
        !resolve_scheduler_sites(module, nt, &game) ||
        !resolve_w3d_clock_sites(module, nt, &game) ||
        !resolve_objective_ui_sites(module, nt, &game) ||
        !resolve_drawable_physics_sites(module, nt, &game) ||
        !resolve_drawable_interpolation_sites(module, nt, &game) ||
        !resolve_selection_panel_sites(module, nt, &game) ||
        !resolve_particle_draw_sites(module, nt, &game) ||
        !resolve_tracer_sites(module, nt, &game) ||
        !resolve_camera_ease_sites(module, nt, &game) ||
        !resolve_shroud_sites(module, nt, &game) ||
        !resolve_tint_envelope_sites(module, nt, &game) ||
        !resolve_minimap_sites(module, nt, &game) ||
        !resolve_waypoint_line_sites(module, nt, &game) ||
        load_u32(module + game.timing.logic_fps) != 15u ||
        load_u32(module + game.timing.client_fps) != 30u ||
        module[game.pacing.display_limiter_branch] != 0x7D ||
        module[game.pacing.display_limiter_branch + 1u] != 0x13) {
        return GAME_UNSUPPORTED_BUILD;
    }

    *out_game = game;
    return GAME_RESOLVED;
}

#undef FIND_UNIQUE
