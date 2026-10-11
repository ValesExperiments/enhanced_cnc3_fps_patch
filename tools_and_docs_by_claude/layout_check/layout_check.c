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
 * src/common.h refuses non-x86 builds, so its few definitions that the
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

#include "../../src/game_layout.c"

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
        printf("  objective UI: apt_player=0x%X add_indicator vtable=0x%X "
               "fns=0x%X/0x%X/0x%X\n",
               full.objective_ui.apt_player_pointer,
               full.objective_ui.add_indicator_vtable_entries[0],
               full.objective_ui.add_indicator_functions[0],
               full.objective_ui.add_indicator_functions[1],
               full.objective_ui.add_indicator_functions[2]);
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
