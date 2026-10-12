#ifndef MULTICORE_PARTICLE_INDEX_H
#define MULTICORE_PARTICLE_INDEX_H

#include "common.h"

/*
 * M1: instant insertion point for the particle-system manager's list.
 *
 * Adding a particle system walks the manager's std::list for the first system
 * with the same key and inserts before it, so systems with the same key stay
 * together. In big battles (thousands of live systems) that walk was ~8% of
 * the main thread. This index maps key -> first node of its group and gives
 * the same position without the walk. It is kept exact by watching the list's
 * only insert and only erase, validates every hit against the list, and falls
 * back to a full walk (rebuilding itself) whenever anything looks off.
 */

typedef u8 *(THISCALL *ParticleListInsertFn)(void *list, u8 **out, u8 *where, void *value);
typedef u32(THISCALL *ParticleListEraseFn)(void *manager, u8 **iterator);

typedef struct ParticleIndexSetup {
    u32 list_offset;               /* manager -> std::list (head pointer first) */
    u32 key_offset;                /* system -> key */
    ParticleListInsertFn insert;   /* the list's insert(out, where, value) */
    ParticleListEraseFn erase;     /* the manager's erase(iterator*) */
    BOOL enabled;                  /* FALSE: the original walk, unchanged */
    BOOL verify;                   /* also walk and count disagreements */
} ParticleIndexSetup;

void particle_index_setup(const ParticleIndexSetup *setup);
/* Forget everything (each game session). */
void particle_index_reset(void);

/* Replacement for the walk: cdecl(manager, system) -> position node. */
u8 *particle_index_find(u8 *manager, u8 *system);
/* Naked replacement for the scan (see particle_index.c for registers). */
void particle_index_scan_stub(void);
/* Wrappers for the list's insert CALL and the three erase CALLs. */
u8 *THISCALL particle_index_insert(void *list, u8 **out, u8 *where, void *value);
u32 THISCALL particle_index_erase(void *manager, u8 **iterator);

typedef struct ParticleIndexStats {
    u32 finds;
    u32 hits;
    u32 misses;           /* key not in the list: insert at the end */
    u32 rebuilds;
    u32 verify_mismatches;
    u32 entries;
} ParticleIndexStats;

/* Returns and clears the counters. */
ParticleIndexStats particle_index_take_stats(void);

#endif
