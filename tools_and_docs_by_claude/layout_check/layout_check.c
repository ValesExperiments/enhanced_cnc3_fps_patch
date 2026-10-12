/*
 * Offline signature-resolver check.
 *
 * Maps a CnC3 game executable (cnc3game.dat / cnc3ep1.dat) read-only into this
 * process at its preferred base (0x00400000), the way the Windows loader
 * would, then runs the patch's real resolver stage by stage. It prints which
 * stages succeed and the resolved RVAs. Nothing is written to the input file.
 *
 * Why 64-bit: a 32-bit host already has WOW64/stack/heap mappings scattered
 * between 0x00700000 and 0x01A00000 at process creation, so 16-24 MB game
 * images (TW 1.10, KW 1.03) cannot be placed at 0x00400000. A 64-bit host
 * keeps its own allocations above 4 GB. The resolver only handles the image
 * as IMAGE_NT_HEADERS32 and RVAs, so it behaves identically while the image
 * stays below 4 GB.
 *
 * src_fps_patch/common.h refuses non-x86 builds, so its few definitions that the
 * resolver needs are supplied here, and the resolver source is included
 * directly so its static stage functions can be called one at a time.
 *
 * Build/run: tools_and_docs_by_claude/layout_check/run.sh <game .dat> [...]
 */
#define FPS_PATCH_COMMON_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;

u32 load_u32(const void *address) {
    u32 value;
    memcpy(&value, address, sizeof(value));
    return value;
}

#include "../../src_fps_patch/game_layout.c"

#define PREFERRED_BASE 0x00400000u

static u8 *read_file(const char *path, DWORD *out_size) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    u8 *data;
    if (file == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(file, NULL);
    data = (u8 *)malloc(size);
    if (data == NULL || !ReadFile(file, data, size, &got, NULL) || got != size) {
        free(data);
        CloseHandle(file);
        return NULL;
    }
    CloseHandle(file);
    *out_size = size;
    return data;
}

static u8 *map_image(const char *path) {
    DWORD file_size;
    u8 *file = read_file(path, &file_size);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *section;
    u8 *image;

    if (file == NULL) {
        printf("  cannot read file\n");
        return NULL;
    }
    dos = (IMAGE_DOS_HEADER *)file;
    if (file_size < 0x1000 || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        (DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > file_size) {
        printf("  not a PE file\n");
        free(file);
        return NULL;
    }
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.ImageBase != PREFERRED_BASE) {
        printf("  not a 32-bit x86 PE with image base 0x%08X\n", PREFERRED_BASE);
        free(file);
        return NULL;
    }
    /* The game images have no relocations, so they must sit at the real base. */
    image = (u8 *)VirtualAlloc((void *)(uintptr_t)PREFERRED_BASE,
                               nt->OptionalHeader.SizeOfImage,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (image == NULL) {
        printf("  could not allocate 0x%X bytes at 0x%08X (error %lu)\n",
               (u32)nt->OptionalHeader.SizeOfImage, PREFERRED_BASE, GetLastError());
        free(file);
        return NULL;
    }
    memcpy(image, file, nt->OptionalHeader.SizeOfHeaders);
    section = IMAGE_FIRST_SECTION(nt);
    for (u32 i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        u32 raw = section[i].SizeOfRawData;
        if (section[i].Misc.VirtualSize != 0 && raw > section[i].Misc.VirtualSize) {
            raw = section[i].Misc.VirtualSize;
        }
        if (section[i].PointerToRawData + raw > file_size) {
            raw = file_size > section[i].PointerToRawData
                      ? file_size - section[i].PointerToRawData : 0;
        }
        memcpy(image + section[i].VirtualAddress,
               file + section[i].PointerToRawData, raw);
    }
    printf("  mapped at 0x%08X, SizeOfImage 0x%X, timestamp 0x%08X\n",
           PREFERRED_BASE, (u32)nt->OptionalHeader.SizeOfImage,
           (u32)nt->FileHeader.TimeDateStamp);
    free(file);
    return image;
}

typedef BOOL (*StageFn)(u8 *, IMAGE_NT_HEADERS32 *, GameLayout *);

static int check(const char *path) {
    /* Same order as resolve_game_layout; later stages depend on earlier ones. */
    static const struct { const char *name; StageFn fn; } stages[] = {
        { "bootstrap", resolve_bootstrap_sites },
        { "keyboard", resolve_keyboard_sites },
        { "visual", resolve_visual_sites },
        { "pacing", resolve_pacing_sites },
        { "camera_input", resolve_camera_input_sites },
        { "cached_timing_values", resolve_cached_timing_values },
        { "audio", resolve_audio_sites },
        { "scheduler", resolve_scheduler_sites },
        { "w3d_clock", resolve_w3d_clock_sites },
        { "objective_ui", resolve_objective_ui_sites },
        { "drawable_physics", resolve_drawable_physics_sites },
        { "drawable_interpolation", resolve_drawable_interpolation_sites },
        { "selection_panel", resolve_selection_panel_sites },
        { "particle_draw", resolve_particle_draw_sites },
        { "tracers", resolve_tracer_sites },
        { "camera_ease", resolve_camera_ease_sites },
        { "shroud", resolve_shroud_sites },
        { "tint_envelope", resolve_tint_envelope_sites },
        { "minimap", resolve_minimap_sites },
        { "waypoint_lines", resolve_waypoint_line_sites },
        { "effect_timing", resolve_effect_timing_sites },
        { "particle_list", resolve_particle_list_sites },
        { "trail_vertices", resolve_trail_vertex_sites },
        { "frame_boundary", resolve_frame_boundary_sites },
        { "range_query", resolve_range_query_sites },
        { "apt_calls", resolve_apt_call_sites },
        { "history_refresh", resolve_history_refresh_sites },
    };
    IMAGE_NT_HEADERS32 *nt;
    GameLayout game, full;
    GameResolveResult result;
    u8 *module;

    printf("%s\n", path);
    module = map_image(path);
    if (module == NULL) return 1;
    if (!get_nt_headers(module, &nt)) {
        printf("  resolver rejected PE headers\n");
        VirtualFree(module, 0, MEM_RELEASE);
        return 1;
    }
    memset(&game, 0, sizeof(game));
    game.module = module;
    game.pe_size_of_image = nt->OptionalHeader.SizeOfImage;
    for (size_t i = 0; i < ARRAY_COUNT(stages); ++i) {
        BOOL ok = stages[i].fn(module, nt, &game);
        printf("  stage %-22s %s\n", stages[i].name, ok ? "ok" : "FAILED");
        if (!ok) break;
    }

    result = resolve_game_layout(&full, module);
    printf("  RESULT: %s\n",
           result == GAME_RESOLVED ? "RESOLVED (supported)"
           : result == GAME_INVALID_PE ? "INVALID_PE" : "UNSUPPORTED_BUILD");
    if (result == GAME_RESOLVED) {
        printf("  target: %s\n", full.target_name);
        printf("  RVAs: client_fps=0x%X logic_fps=0x%X phase_batch=0x%X "
               "phase_interp=0x%X particle_sim_call=0x%X anim2d_ts=0x%X "
               "anim2d_upd=0x%X\n",
               full.timing.client_fps, full.timing.logic_fps,
               full.scheduler.phase_batch_block,
               full.scheduler.phase_interpolation_function,
               full.visual.particle_simulation.instruction,
               full.visual.anim2d_timestamp_frame_call,
               full.visual.anim2d_update_frame_call);
        printf("  timing: game_engine_ptr=0x%X global_data_ptr=0x%X client_fps=0x%X\n",
               full.timing.game_engine_pointer, full.timing.global_data_pointer,
               full.timing.client_fps);
        printf("  interpolation windows: imm RVAs 0x%X 0x%X\n",
               full.drawable_interpolation.window_immediates[0],
               full.drawable_interpolation.window_immediates[1]);
        printf("  objective UI: apt_player=0x%X add_indicator vtable=0x%X "
               "fns=0x%X/0x%X/0x%X\n",
               full.objective_ui.apt_player_pointer,
               full.objective_ui.add_indicator_vtable_entries[0],
               full.objective_ui.add_indicator_functions[0],
               full.objective_ui.add_indicator_functions[1],
               full.objective_ui.add_indicator_functions[2]);
        for (u32 k = 0; k < SHROUD_TEXTURE_KINDS; ++k) {
            const ShroudFadeSite *fade = &full.shroud.fades[k];
            float rate;
            memcpy(&rate, module + fade->rate, sizeof(rate));
            printf("  shroud %s: fade call 0x%X -> 0x%X stamp 0x%X rate %.3f/ms cap %u\n",
                   k == SHROUD_TEXTURE_FOG ? "fog   " : "shroud",
                   fade->fade_call.instruction, fade->fade_call.target,
                   fade->timestamp, rate, fade->step_cap);
        }
        printf("  tint envelope update 0x%X, calls 0x%X 0x%X, flash interval 0x%X\n",
               full.tint_envelope.update_function,
               full.tint_envelope.update_calls[0].instruction,
               full.tint_envelope.update_calls[1].instruction,
               full.tint_envelope.flash_interval);
        printf("  waypoint lines: call 0x%X, object pos +0x%X, drawable +0x%X/+0x%X\n",
               full.waypoint_lines.line_call.instruction, full.waypoint_lines.object_position,
               full.waypoint_lines.object_drawable, full.waypoint_lines.drawable_object);
        printf("  effect timing: fade in 0x%X out 0x%X, summon fades 0x%X->0x%X 0x%X->0x%X "
               "0x%X->0x%X, pulse play 0x%X, pulse colour 0x%X calls 0x%X 0x%X, logic 0x%X+0x%X\n",
               full.effect_timing.fade_in_function, full.effect_timing.fade_out_function,
               full.effect_timing.summon_fade_calls[0].instruction,
               full.effect_timing.summon_fade_calls[0].target,
               full.effect_timing.summon_fade_calls[1].instruction,
               full.effect_timing.summon_fade_calls[1].target,
               full.effect_timing.summon_fade_calls[2].instruction,
               full.effect_timing.summon_fade_calls[2].target,
               full.effect_timing.pulse_play_call.instruction,
               full.effect_timing.pulse_color_function,
               full.effect_timing.pulse_color_calls[0].instruction,
               full.effect_timing.pulse_color_calls[1].instruction,
               full.effect_timing.game_logic_pointer, full.effect_timing.logic_frame_offset);
        printf("  particle list: insert 0x%X scan 0x%X+%u insert call 0x%X->0x%X, erase 0x%X "
               "calls 0x%X 0x%X 0x%X, list +0x%X key +0x%X\n",
               full.particle_list.insert_function, full.particle_list.scan_start,
               full.particle_list.scan_length, full.particle_list.insert_call.instruction,
               full.particle_list.insert_call.target, full.particle_list.erase_function,
               full.particle_list.erase_calls[0].instruction,
               full.particle_list.erase_calls[1].instruction,
               full.particle_list.erase_calls[2].instruction,
               full.particle_list.list_offset, full.particle_list.key_offset);
        printf("  trail vertex read-back 0x%X\n", full.trail_vertices.readback);
        printf("  IsClientFrameBoundary 0x%X\n", full.frame_boundary.function);
        printf("  range query 0x%X calls 0x%X 0x%X 0x%X, distance table 0x%X, add call 0x%X->0x%X\n",
               full.range_query.query_function, full.range_query.query_calls[0].instruction,
               full.range_query.query_calls[1].instruction,
               full.range_query.query_calls[2].instruction, full.range_query.distance_table,
               full.range_query.add_call.instruction, full.range_query.add_call.target);
        printf("  apt variable function 0x%X\n", full.apt_calls.variable_function);
        printf("  history refresh 0x%X (%s)\n", full.history_refresh.function,
               full.history_refresh.kane ? "KW, 2 args" : "TW, 1 arg");
        printf("  apt call variants %u: 0x%X 0x%X 0x%X 0x%X 0x%X\n", full.apt_calls.count,
               full.apt_calls.functions[0], full.apt_calls.functions[1],
               full.apt_calls.functions[2], full.apt_calls.functions[3],
               full.apt_calls.functions[4]);
        printf("  particle sibling search setup 0x%X, mismatch jump 0x%X\n",
               full.particle_list.sibling_setup, full.particle_list.sibling_mismatch);
        printf("  minimap look-at copies %u, view 0x%X\n",
               full.minimap.count, full.minimap.view_pointer);
        printf("  shroud fog row copy call 0x%X -> 0x%X\n",
               full.shroud.fog_row_copy_call.instruction,
               full.shroud.fog_row_copy_call.target);
    }
    VirtualFree(module, 0, MEM_RELEASE);
    return result == GAME_RESOLVED ? 0 : 2;
}

int main(int argc, char **argv) {
    int failures = 0;
    if (argc < 2) {
        printf("usage: layout_check <game.dat> [...]\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) failures += check(argv[i]) != 0;
    printf("%d of %d executable(s) failed to resolve\n", failures, argc - 1);
    return failures;
}
