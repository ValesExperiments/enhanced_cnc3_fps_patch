#include "range_stats.h"

#include <stdio.h>
#include <stdlib.h>

enum {
    CALLER_SLOTS = 64,
    SIZE_BUCKETS = 7,     /* candidates per query: 0, 1-8, 9-32, 33-128, 129-512, 513-2048, more */
    TOP_CALLERS = 8
};

typedef struct CallerStats {
    u32 caller;           /* return address into the system that asked (RVA) */
    u32 queries;
    u64 candidates;
    u64 accepted;
    u64 ticks;            /* QPC ticks inside the query */
} CallerStats;

static RangeStatsSetup g_range;
static LARGE_INTEGER g_range_frequency;

/* Read and written by the stubs (main thread only). */
volatile u32 g_range_candidates;
volatile u32 g_range_accepted;
u32 g_range_caller;
void *g_range_distance_original[RANGE_DISTANCE_CALLBACKS];
void *g_range_add_original;

static CallerStats g_callers[CALLER_SLOTS];
static u32 g_bucket_queries[SIZE_BUCKETS];
static u64 g_bucket_ticks[SIZE_BUCKETS];
static u32 g_max_candidates;
static u32 g_window_start_ms;

void range_stats_setup(const RangeStatsSetup *setup) {
    u32 i;

    g_range = *setup;
    for (i = 0; i < RANGE_DISTANCE_CALLBACKS; ++i) g_range_distance_original[i] = setup->distance[i];
    g_range_add_original = setup->add_result;
    QueryPerformanceFrequency(&g_range_frequency);
    g_window_start_ms = timeGetTime();
}

static u32 size_bucket(u32 candidates) {
    if (candidates == 0u) return 0u;
    if (candidates <= 8u) return 1u;
    if (candidates <= 32u) return 2u;
    if (candidates <= 128u) return 3u;
    if (candidates <= 512u) return 4u;
    if (candidates <= 2048u) return 5u;
    return 6u;
}

static CallerStats *caller_stats(u32 caller) {
    u32 slot = (caller * 2654435761u) % CALLER_SLOTS;
    u32 probes;

    for (probes = 0; probes < CALLER_SLOTS; ++probes) {
        CallerStats *stats = &g_callers[slot];
        if (stats->caller == caller) return stats;
        if (stats->caller == 0u) {
            stats->caller = caller;
            return stats;
        }
        slot = (slot + 1u) % CALLER_SLOTS;
    }
    return NULL;
}

/* Reached by a JMP from range_stats_query_stub, as the original's thiscall. */
__attribute__((used)) static u32 THISCALL range_stats_query(void *partition, u32 a1, u32 a2,
                                                            u32 a3, u32 a4, u32 a5, u32 a6,
                                                            u32 a7) {
    u32 caller = g_range_caller - (u32)(uintptr_t)g_range.game_module;
    u32 candidates = g_range_candidates;
    u32 accepted = g_range_accepted;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    u32 result;
    CallerStats *stats;
    u32 bucket;

    QueryPerformanceCounter(&start);
    result = g_range.query(partition, a1, a2, a3, a4, a5, a6, a7);
    QueryPerformanceCounter(&end);
    candidates = g_range_candidates - candidates;
    accepted = g_range_accepted - accepted;
    bucket = size_bucket(candidates);
    ++g_bucket_queries[bucket];
    g_bucket_ticks[bucket] += (u64)(end.QuadPart - start.QuadPart);
    if (candidates > g_max_candidates) g_max_candidates = candidates;
    stats = caller_stats(caller);
    if (stats != NULL) {
        ++stats->queries;
        stats->candidates += candidates;
        stats->accepted += accepted;
        stats->ticks += (u64)(end.QuadPart - start.QuadPart);
    }
    return result;
}

/*
 * Called instead of the query (CALL from one of its three thin wrappers,
 * which keep an EBP frame): [EBP+4] is the address in the system that asked.
 */
__attribute__((naked)) void range_stats_query_stub(void) {
    __asm__ volatile(
        "movl 4(%%ebp), %%eax\n\t"
        "movl %%eax, %c0\n\t"
        "jmp %c1"
        :
        : "i"(&g_range_caller), "i"(range_stats_query));
}

/* Distance callbacks: count, then continue in the original (stack untouched). */
#define RANGE_DISTANCE_STUB(n)                                                   \
    __attribute__((naked)) static void range_distance_stub_##n(void) {           \
        __asm__ volatile("incl %c0\n\t"                                          \
                         "jmp *%c1"                                              \
                         :                                                       \
                         : "i"(&g_range_candidates),                             \
                           "i"(&g_range_distance_original[n]));                  \
    }
RANGE_DISTANCE_STUB(0)
RANGE_DISTANCE_STUB(1)
RANGE_DISTANCE_STUB(2)
RANGE_DISTANCE_STUB(3)
RANGE_DISTANCE_STUB(4)

void *const g_range_distance_stubs[RANGE_DISTANCE_CALLBACKS] = {
    (void *)range_distance_stub_0, (void *)range_distance_stub_1, (void *)range_distance_stub_2,
    (void *)range_distance_stub_3, (void *)range_distance_stub_4
};

__attribute__((naked)) void range_stats_add_stub(void) {
    __asm__ volatile("incl %c0\n\t"
                     "jmp *%c1"
                     :
                     : "i"(&g_range_accepted), "i"(&g_range_add_original));
}

static int compare_callers(const void *a, const void *b) {
    const CallerStats *left = (const CallerStats *)a;
    const CallerStats *right = (const CallerStats *)b;
    if (left->ticks != right->ticks) return left->ticks < right->ticks ? 1 : -1;
    return 0;
}

void range_stats_report(void (*log)(const char *format, ...), u32 seconds) {
    static const char *const bucket_names[SIZE_BUCKETS] = {
        "0", "1-8", "9-32", "33-128", "129-512", "513-2048", "2049+"
    };
    CallerStats sorted[CALLER_SLOTS];
    u32 now = timeGetTime();
    double window_ms = (double)(now - g_window_start_ms);
    double tick_ms = 1000.0 / (double)g_range_frequency.QuadPart;
    u32 queries = 0;
    u64 candidates = 0;
    u64 accepted = 0;
    u64 ticks = 0;
    u32 count = 0;
    u32 i;
    char line[512];
    int used;

    for (i = 0; i < CALLER_SLOTS; ++i) {
        if (g_callers[i].caller == 0u) continue;
        sorted[count++] = g_callers[i];
        queries += g_callers[i].queries;
        candidates += g_callers[i].candidates;
        accepted += g_callers[i].accepted;
        ticks += g_callers[i].ticks;
    }
    if (window_ms < 1.0) window_ms = 1.0;
    log("Range queries [%us]: %u queries (%.0f/s), %.1f ms/s inside, candidates %.1f/query "
        "(max %u), accepted %.1f/query",
        seconds, queries, queries * 1000.0 / window_ms, (double)ticks * tick_ms * 1000.0 / window_ms,
        queries ? (double)candidates / queries : 0.0, g_max_candidates,
        queries ? (double)accepted / queries : 0.0);
    used = snprintf(line, sizeof(line), "Range query sizes (candidates: queries, ms/s):");
    for (i = 0; i < SIZE_BUCKETS && used > 0 && (size_t)used < sizeof(line); ++i) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %s: %u, %.1f;", bucket_names[i],
                         g_bucket_queries[i],
                         (double)g_bucket_ticks[i] * tick_ms * 1000.0 / window_ms);
    }
    log("%s", line);
    qsort(sorted, count, sizeof(CallerStats), compare_callers);
    for (i = 0; i < count && i < TOP_CALLERS; ++i) {
        const CallerStats *s = &sorted[i];
        log("  range caller 0x%06X: %u queries, %.1f ms/s, candidates %.1f/query, "
            "accepted %.1f/query",
            s->caller, s->queries, (double)s->ticks * tick_ms * 1000.0 / window_ms,
            s->queries ? (double)s->candidates / s->queries : 0.0,
            s->queries ? (double)s->accepted / s->queries : 0.0);
    }

    memset(g_callers, 0, sizeof(g_callers));
    memset(g_bucket_queries, 0, sizeof(g_bucket_queries));
    memset(g_bucket_ticks, 0, sizeof(g_bucket_ticks));
    g_max_candidates = 0;
    g_window_start_ms = now;
}
