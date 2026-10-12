#ifndef MULTICORE_RANGE_STATS_H
#define MULTICORE_RANGE_STATS_H

#include "common.h"

/*
 * M4 diagnostic (installed with profiler=1): measures the partition
 * manager's range queries without changing them. Counts queries per caller,
 * their time, the candidates their distance callbacks test and the results
 * they accept, and a histogram of candidates per query.
 */

enum { RANGE_DISTANCE_CALLBACKS = 5 };

typedef u32(THISCALL *RangeQueryFn)(void *partition, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5,
                                    u32 a6, u32 a7);

typedef struct RangeStatsSetup {
    u8 *game_module;
    RangeQueryFn query;
    void *distance[RANGE_DISTANCE_CALLBACKS];
    void *add_result;
} RangeStatsSetup;

void range_stats_setup(const RangeStatsSetup *setup);

/* Replacements: the query CALLs, the callback table entries, the add CALL. */
void range_stats_query_stub(void);
extern void *const g_range_distance_stubs[RANGE_DISTANCE_CALLBACKS];
void range_stats_add_stub(void);

/* Writes the window's summary lines with `log` and starts a new window. */
void range_stats_report(void (*log)(const char *format, ...), u32 seconds);

#endif
