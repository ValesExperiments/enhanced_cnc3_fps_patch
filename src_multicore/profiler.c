#include "profiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

volatile LONG g_profile_zone;

enum {
    SAMPLE_STACK_BYTES = 4096,      /* stack copied above ESP per sample */
    MAX_SAMPLE_RETURNS = 64,        /* game return addresses kept per sample */
    EXCLUSIVE_SLOTS = 1u << 15,     /* per zone */
    INCLUSIVE_SLOTS = 1u << 16,
    EXTERNAL_SLOTS = 1u << 14,
    MAX_MODULES = 48,
    MAX_THREADS = 192,
    TOP_EXCLUSIVE = 120,            /* lines per zone in a report */
    TOP_INCLUSIVE = 300,
    TOP_EXTERNAL = 100,
    REPORT_LINE = 512
};

typedef struct Counter {
    u32 key;
    u32 count;
} Counter;

typedef struct CounterMap {
    Counter *slots;
    u32 mask;
    u32 used;
    u32 dropped;
} CounterMap;

typedef struct ModuleRange {
    uintptr_t base;
    uintptr_t end;
    char name[40];
    u32 samples[PROFILE_ZONE_COUNT];
} ModuleRange;

typedef struct ThreadTimes {
    DWORD id;
    u64 cpu_100ns;
    BOOL seen;
} ThreadTimes;

typedef LONG(WINAPI *NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);

typedef struct Profiler {
    HANDLE main_thread;
    DWORD main_thread_id;
    uintptr_t stack_base;
    u8 *game;
    u32 game_size;
    u32 report_ms;
    char target_name[96];
    u32 pe_timestamp;
    HANDLE output;
    HANDLE timer;
    NtQueryInformationThreadFn query_thread;

    /* Window counters (reset after each report). */
    CounterMap exclusive[PROFILE_ZONE_COUNT];
    CounterMap inclusive;
    CounterMap external;
    u32 zone_samples[PROFILE_ZONE_COUNT];
    u32 game_samples[PROFILE_ZONE_COUNT];
    u32 unknown_samples[PROFILE_ZONE_COUNT];
    u32 samples;
    u32 failed_samples;
    ModuleRange modules[MAX_MODULES];
    u32 module_count;
    ThreadTimes threads[MAX_THREADS];
    u32 thread_count;
    Counter *sort_scratch;
    u32 window;
    u64 window_start_ms;
    u64 session_start_ms;
    u32 frames_at_window_start;
} Profiler;

static Profiler g_profiler;
static volatile LONG g_profiler_frames;
static volatile LONG g_profiler_rate;
static volatile LONG g_profiler_started;

/* ---- counters -------------------------------------------------------------- */

static BOOL map_init(CounterMap *map, u32 slots) {
    map->slots = (Counter *)VirtualAlloc(NULL, slots * sizeof(Counter), MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
    map->mask = slots - 1u;
    map->used = 0;
    map->dropped = 0;
    return map->slots != NULL;
}

static void map_clear(CounterMap *map) {
    memset(map->slots, 0, (map->mask + 1u) * sizeof(Counter));
    map->used = 0;
    map->dropped = 0;
}

static void map_add(CounterMap *map, u32 key) {
    u32 slot;

    if (key == 0u) return;
    slot = (key * 2654435761u) & map->mask;
    for (;;) {
        Counter *counter = &map->slots[slot];
        if (counter->key == key) {
            ++counter->count;
            return;
        }
        if (counter->key == 0u) {
            /* Keep a quarter free so probes stay short. */
            if (map->used >= (map->mask + 1u) / 4u * 3u) {
                ++map->dropped;
                return;
            }
            counter->key = key;
            counter->count = 1;
            ++map->used;
            return;
        }
        slot = (slot + 1u) & map->mask;
    }
}

static int compare_counters(const void *a, const void *b) {
    const Counter *left = (const Counter *)a;
    const Counter *right = (const Counter *)b;
    if (left->count != right->count) return left->count < right->count ? 1 : -1;
    return left->key < right->key ? -1 : (left->key > right->key ? 1 : 0);
}

/* ---- output ---------------------------------------------------------------- */

static void report(const char *format, ...) {
    char line[REPORT_LINE];
    va_list args;
    int length;
    DWORD written;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line) - 2u, format, args);
    va_end(args);
    if (length < 0) return;
    if ((size_t)length > sizeof(line) - 3u) length = (int)sizeof(line) - 3;
    line[length++] = '\r';
    line[length++] = '\n';
    WriteFile(g_profiler.output, line, (DWORD)length, &written, NULL);
}

static void report_top(const char *tag, const CounterMap *map, u32 limit) {
    u32 count = 0;
    u32 i;

    for (i = 0; i <= map->mask; ++i) {
        if (map->slots[i].key != 0u) g_profiler.sort_scratch[count++] = map->slots[i];
    }
    qsort(g_profiler.sort_scratch, count, sizeof(Counter), compare_counters);
    if (count > limit) count = limit;
    for (i = 0; i < count; ++i) {
        report("%s 0x%06X %u", tag, g_profiler.sort_scratch[i].key,
               g_profiler.sort_scratch[i].count);
    }
    if (map->dropped != 0u) report("%s dropped %u", tag, map->dropped);
}

/* ---- modules --------------------------------------------------------------- */

static ModuleRange *module_for(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS32 *nt;
    ModuleRange *module;
    char path[MAX_PATH];
    const char *name;
    u32 i;

    for (i = 0; i < g_profiler.module_count; ++i) {
        if (address >= g_profiler.modules[i].base && address < g_profiler.modules[i].end) {
            return &g_profiler.modules[i];
        }
    }
    if (g_profiler.module_count >= MAX_MODULES ||
        VirtualQuery((const void *)address, &info, sizeof(info)) == 0 ||
        info.AllocationBase == NULL || info.Type != MEM_IMAGE) {
        return NULL;
    }
    dos = (const IMAGE_DOS_HEADER *)info.AllocationBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (const IMAGE_NT_HEADERS32 *)((const u8 *)dos + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    module = &g_profiler.modules[g_profiler.module_count++];
    memset(module, 0, sizeof(*module));
    module->base = (uintptr_t)dos;
    module->end = module->base + nt->OptionalHeader.SizeOfImage;
    if (GetModuleFileNameA((HMODULE)dos, path, sizeof(path)) == 0) {
        strcpy(path, "?");
    }
    name = strrchr(path, '\\');
    name = name != NULL ? name + 1 : path;
    strncpy(module->name, name, sizeof(module->name) - 1u);
    return module;
}

/* ---- sampling -------------------------------------------------------------- */

/* Is `value` a return address into the game image (preceded by a CALL)? */
static BOOL is_game_return(uintptr_t value) {
    uintptr_t base = (uintptr_t)g_profiler.game;
    const u8 *code;

    if (value < base + 0x1000u + 6u || value >= base + g_profiler.game_size) return FALSE;
    code = (const u8 *)value;
    if (code[-5] == 0xE8) return TRUE;                                   /* CALL rel32 */
    if (code[-6] == 0xFF && (code[-5] & 0x38u) == 0x10u &&
        ((code[-5] & 0xC0u) == 0x80u || code[-5] == 0x15)) return TRUE;  /* CALL [r+d32]/[abs] */
    if (code[-3] == 0xFF && (code[-2] & 0x38u) == 0x10u && (code[-2] & 0xC0u) == 0x40u) {
        return TRUE;                                                     /* CALL [r+d8] */
    }
    if (code[-2] == 0xFF && (code[-1] & 0xF8u) == 0xD0u) return TRUE;    /* CALL reg */
    if (code[-2] == 0xFF && (code[-1] & 0xF8u) == 0x10u && code[-1] != 0x14 &&
        code[-1] != 0x15) return TRUE;                                   /* CALL [reg] */
    return FALSE;
}

static void take_sample(void) {
    static u8 stack_copy[SAMPLE_STACK_BYTES];
    CONTEXT context;
    uintptr_t base = (uintptr_t)g_profiler.game;
    u32 returns[MAX_SAMPLE_RETURNS];
    u32 return_count = 0;
    u32 stack_bytes = 0;
    uintptr_t eip;
    LONG zone;
    u32 i;

    if (SuspendThread(g_profiler.main_thread) == (DWORD)-1) {
        ++g_profiler.failed_samples;
        return;
    }
    /* Nothing below may allocate or take a lock while the game thread is held. */
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(g_profiler.main_thread, &context)) {
        ResumeThread(g_profiler.main_thread);
        ++g_profiler.failed_samples;
        return;
    }
    zone = g_profile_zone;
    if (context.Esp != 0u && context.Esp < g_profiler.stack_base) {
        stack_bytes = (u32)(g_profiler.stack_base - context.Esp);
        if (stack_bytes > SAMPLE_STACK_BYTES) stack_bytes = SAMPLE_STACK_BYTES;
        stack_bytes &= ~3u;
        memcpy(stack_copy, (const void *)(uintptr_t)context.Esp, stack_bytes);
    }
    ResumeThread(g_profiler.main_thread);

    if (zone < 0 || zone >= PROFILE_ZONE_COUNT) zone = PROFILE_ZONE_CLIENT;
    eip = context.Eip;
    ++g_profiler.samples;
    ++g_profiler.zone_samples[zone];

    /* Game return addresses on the stack, outermost duplicates removed. */
    for (i = 0; i + 4u <= stack_bytes && return_count < MAX_SAMPLE_RETURNS; i += 4u) {
        uintptr_t value = *(const u32 *)(stack_copy + i);
        u32 rva;
        u32 k;
        if (!is_game_return(value)) continue;
        rva = (u32)(value - base);
        for (k = 0; k < return_count && returns[k] != rva; ++k) {
        }
        if (k == return_count) returns[return_count++] = rva;
    }

    if (eip >= base && eip < base + g_profiler.game_size) {
        ++g_profiler.game_samples[zone];
        map_add(&g_profiler.exclusive[zone], (u32)(eip - base));
        map_add(&g_profiler.inclusive, (u32)(eip - base));
    } else {
        ModuleRange *module = module_for(eip);
        if (module != NULL) {
            ++module->samples[zone];
        } else {
            ++g_profiler.unknown_samples[zone];
        }
        /* The game call site that led out of the game image. */
        if (return_count != 0u) map_add(&g_profiler.external, returns[0]);
    }
    for (i = 0; i < return_count; ++i) map_add(&g_profiler.inclusive, returns[i]);
}

/* ---- per-thread CPU -------------------------------------------------------- */

static void report_threads(u64 window_ms) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    DWORD process = GetCurrentProcessId();
    u32 i;

    if (snapshot == INVALID_HANDLE_VALUE) return;
    for (i = 0; i < g_profiler.thread_count; ++i) g_profiler.threads[i].seen = FALSE;
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            HANDLE thread;
            FILETIME created, exited, kernel, user;
            u64 cpu;
            u64 delta;
            uintptr_t start = 0;
            ThreadTimes *times = NULL;
            ModuleRange *module;

            if (entry.th32OwnerProcessID != process) continue;
            thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == NULL) continue;
            if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                CloseHandle(thread);
                continue;
            }
            if (g_profiler.query_thread != NULL) {
                /* ThreadQuerySetWin32StartAddress = 9 */
                g_profiler.query_thread(thread, 9, &start, sizeof(start), NULL);
            }
            CloseHandle(thread);
            cpu = (((u64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                  (((u64)user.dwHighDateTime << 32) | user.dwLowDateTime);
            for (i = 0; i < g_profiler.thread_count; ++i) {
                if (g_profiler.threads[i].id == entry.th32ThreadID) times = &g_profiler.threads[i];
            }
            if (times == NULL && g_profiler.thread_count < MAX_THREADS) {
                times = &g_profiler.threads[g_profiler.thread_count++];
                times->id = entry.th32ThreadID;
                times->cpu_100ns = 0;
            }
            delta = times != NULL ? cpu - times->cpu_100ns : cpu;
            if (times != NULL) {
                times->cpu_100ns = cpu;
                times->seen = TRUE;
            }
            if (delta < 10000u) continue; /* under 1 ms: idle */
            module = start != 0u ? module_for(start) : NULL;
            report("thread %5lu%s start %s+0x%X cpu %.0f ms (%.1f%%)",
                   (unsigned long)entry.th32ThreadID,
                   entry.th32ThreadID == g_profiler.main_thread_id ? " MAIN" : "     ",
                   module != NULL ? module->name : "?",
                   module != NULL ? (unsigned)(start - module->base) : (unsigned)start,
                   (double)delta / 10000.0,
                   window_ms != 0u ? (double)delta / 100.0 / (double)window_ms : 0.0);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

/* ---- reports --------------------------------------------------------------- */

static void write_report(u64 now_ms) {
    static const char *const zone_names[PROFILE_ZONE_COUNT] = { "client", "logic", "effects" };
    u64 window_ms = now_ms - g_profiler.window_start_ms;
    u32 frames = (u32)g_profiler_frames - g_profiler.frames_at_window_start;
    u32 zone;
    u32 i;

    report("=== window %u t=%.0fs length=%.1fs samples=%u failed=%u frames=%u (%.1f FPS) rate=%ld ===",
           g_profiler.window, (double)(now_ms - g_profiler.session_start_ms) / 1000.0,
           (double)window_ms / 1000.0, g_profiler.samples, g_profiler.failed_samples, frames,
           window_ms != 0u ? (double)frames * 1000.0 / (double)window_ms : 0.0,
           (long)g_profiler_rate);
    for (zone = 0; zone < PROFILE_ZONE_COUNT; ++zone) {
        report("zone %s samples=%u game=%u unknown=%u", zone_names[zone],
               g_profiler.zone_samples[zone], g_profiler.game_samples[zone],
               g_profiler.unknown_samples[zone]);
    }
    for (i = 0; i < g_profiler.module_count; ++i) {
        const ModuleRange *module = &g_profiler.modules[i];
        if (module->samples[0] + module->samples[1] + module->samples[2] == 0u) continue;
        report("module %s client=%u logic=%u effects=%u", module->name, module->samples[0],
               module->samples[1], module->samples[2]);
    }
    report_threads(window_ms);
    for (zone = 0; zone < PROFILE_ZONE_COUNT; ++zone) {
        char tag[24];
        snprintf(tag, sizeof(tag), "excl_%s", zone_names[zone]);
        report_top(tag, &g_profiler.exclusive[zone], TOP_EXCLUSIVE);
    }
    report_top("incl", &g_profiler.inclusive, TOP_INCLUSIVE);
    report_top("ext", &g_profiler.external, TOP_EXTERNAL);
    report("=== end window %u ===", g_profiler.window);

    /* Start the next window. */
    ++g_profiler.window;
    g_profiler.window_start_ms = now_ms;
    g_profiler.frames_at_window_start = (u32)g_profiler_frames;
    g_profiler.samples = 0;
    g_profiler.failed_samples = 0;
    for (zone = 0; zone < PROFILE_ZONE_COUNT; ++zone) {
        map_clear(&g_profiler.exclusive[zone]);
        g_profiler.zone_samples[zone] = 0;
        g_profiler.game_samples[zone] = 0;
        g_profiler.unknown_samples[zone] = 0;
    }
    map_clear(&g_profiler.inclusive);
    map_clear(&g_profiler.external);
    for (i = 0; i < g_profiler.module_count; ++i) {
        memset(g_profiler.modules[i].samples, 0, sizeof(g_profiler.modules[i].samples));
    }
}

static u64 now_ms(void) {
    return GetTickCount64();
}

static void wait_one_millisecond(void) {
    if (g_profiler.timer != NULL) {
        LARGE_INTEGER due;
        due.QuadPart = -10000; /* 1 ms, relative */
        if (SetWaitableTimer(g_profiler.timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(g_profiler.timer, 50);
            return;
        }
    }
    Sleep(1);
}

static DWORD WINAPI sampler_thread(LPVOID parameter) {
    (void)parameter;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        u64 now;
        wait_one_millisecond();
        take_sample();
        now = now_ms();
        if (now - g_profiler.window_start_ms >= g_profiler.report_ms) write_report(now);
    }
    return 0;
}

/* ---- start ----------------------------------------------------------------- */

BOOL profiler_start(const ProfilerSetup *setup) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    HANDLE thread;
    HMODULE ntdll;
    u32 zone;

    if (InterlockedCompareExchange(&g_profiler_started, 1, 0) != 0) return TRUE;
    memset(&g_profiler, 0, sizeof(g_profiler));
    g_profiler.main_thread_id = GetCurrentThreadId();
    g_profiler.main_thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
        g_profiler.main_thread_id);
    g_profiler.stack_base = (uintptr_t)tib->StackBase;
    g_profiler.game = setup->game_module;
    g_profiler.game_size = setup->game_image_size;
    g_profiler.report_ms = (setup->report_seconds != 0u ? setup->report_seconds : 60u) * 1000u;
    g_profiler.pe_timestamp = setup->pe_timestamp;
    strncpy(g_profiler.target_name, setup->target_name != NULL ? setup->target_name : "?",
            sizeof(g_profiler.target_name) - 1u);
    ntdll = GetModuleHandleW(L"ntdll.dll");
    g_profiler.query_thread = ntdll != NULL
        ? (NtQueryInformationThreadFn)(void (*)(void))GetProcAddress(ntdll, "NtQueryInformationThread")
        : NULL;
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+); else Sleep(1). */
    g_profiler.timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);

    for (zone = 0; zone < PROFILE_ZONE_COUNT; ++zone) {
        if (!map_init(&g_profiler.exclusive[zone], EXCLUSIVE_SLOTS)) return FALSE;
    }
    if (g_profiler.main_thread == NULL || !map_init(&g_profiler.inclusive, INCLUSIVE_SLOTS) ||
        !map_init(&g_profiler.external, EXTERNAL_SLOTS)) {
        return FALSE;
    }
    g_profiler.sort_scratch = (Counter *)VirtualAlloc(
        NULL, INCLUSIVE_SLOTS * sizeof(Counter), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_profiler.sort_scratch == NULL) return FALSE;

    g_profiler.output = CreateFileW(setup->output_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_profiler.output == INVALID_HANDLE_VALUE) return FALSE;
    report("fps_profile v1: game=%s pe_timestamp=0x%08X image=0x%X base=0x%p "
           "sampling ~1000/s timer=%s report=%us",
           g_profiler.target_name, g_profiler.pe_timestamp, g_profiler.game_size,
           (void *)g_profiler.game, g_profiler.timer != NULL ? "high-res" : "Sleep(1)",
           g_profiler.report_ms / 1000u);
    report("RVAs are relative to the game image. excl = instruction pointer per zone; "
           "incl = instruction or return address anywhere on the stack; ext = game call "
           "site that led into another module.");

    g_profiler.session_start_ms = now_ms();
    g_profiler.window_start_ms = g_profiler.session_start_ms;
    g_profiler.frames_at_window_start = (u32)g_profiler_frames;
    thread = CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    if (thread == NULL) return FALSE;
    CloseHandle(thread);
    return TRUE;
}

void profiler_note_frame(u32 rate) {
    InterlockedIncrement(&g_profiler_frames);
    g_profiler_rate = (LONG)rate;
}
