#include "particle_index.h"

/*
 * std::list node: next (+0), prev (+4), RefPtr<ParticleSystem> (+8). The list
 * object's first field is the head (sentinel) node pointer.
 *
 * Invariant kept by the game's own insert policy: systems with the same key
 * are contiguous, and the index entry for a key is the first node of its
 * group (= the first node in list order with that key). Each entry also
 * counts its nodes, so a key that isn't indexed has no node in the list and
 * the new system goes at the end.
 */
enum {
    INDEX_SLOTS = 1u << 13,        /* power of two */
    INDEX_MAX_USED = INDEX_SLOTS / 2u,
    WALK_LIMIT = 1u << 22          /* guards against a corrupted list */
};

typedef enum EntryState { ENTRY_EMPTY = 0, ENTRY_USED = 1, ENTRY_DELETED = 2 } EntryState;

typedef struct IndexEntry {
    u32 key;
    u32 count;
    u8 *first;      /* NULL: count > 0 but the first node is not known */
    u32 state;
} IndexEntry;

static IndexEntry g_entries[INDEX_SLOTS];
static u32 g_used;          /* USED + DELETED slots */
static u32 g_live;
static u8 *g_manager;       /* the manager the index describes */
static BOOL g_valid;
static ParticleIndexSetup g_setup;
static ParticleIndexStats g_stats;

static u8 *node_next(const u8 *node) { return *(u8 *const *)node; }
static u8 *node_prev(const u8 *node) { return *(u8 *const *)(node + 4); }
static u8 *node_system(const u8 *node) { return *(u8 *const *)(node + 8); }

static BOOL node_key(const u8 *node, u32 *key) {
    const u8 *system = node_system(node);
    if (system == NULL) return FALSE;
    *key = *(const u32 *)(system + g_setup.key_offset);
    return TRUE;
}

static u8 *list_head(const u8 *manager) {
    return *(u8 *const *)(manager + g_setup.list_offset);
}

/* The original scan: first node with `key`, or the head (insert at the end). */
static u8 *walk(u8 *head, u32 key) {
    u8 *node = node_next(head);
    u32 steps = 0;

    while (node != head && steps++ < WALK_LIMIT) {
        u32 node_k;
        if (node_key(node, &node_k) && node_k == key) return node;
        node = node_next(node);
    }
    return head;
}

static u32 slot_of(u32 key) {
    return (key * 2654435761u) >> 19; /* 13 bits */
}

static IndexEntry *lookup(u32 key) {
    u32 slot = slot_of(key);
    u32 probes;

    for (probes = 0; probes < INDEX_SLOTS; ++probes) {
        IndexEntry *entry = &g_entries[slot];
        if (entry->state == ENTRY_EMPTY) return NULL;
        if (entry->state == ENTRY_USED && entry->key == key) return entry;
        slot = (slot + 1u) & (INDEX_SLOTS - 1u);
    }
    return NULL;
}

static IndexEntry *add(u32 key) {
    u32 slot = slot_of(key);

    if (g_used >= INDEX_MAX_USED) return NULL;
    while (g_entries[slot].state == ENTRY_USED) slot = (slot + 1u) & (INDEX_SLOTS - 1u);
    if (g_entries[slot].state == ENTRY_EMPTY) ++g_used;
    g_entries[slot].state = ENTRY_USED;
    g_entries[slot].key = key;
    g_entries[slot].count = 0;
    g_entries[slot].first = NULL;
    ++g_live;
    return &g_entries[slot];
}

static void remove_entry(IndexEntry *entry) {
    entry->state = ENTRY_DELETED;
    entry->first = NULL;
    entry->count = 0;
    --g_live;
}

static void clear(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_used = 0;
    g_live = 0;
    g_valid = FALSE;
}

/* One full walk: first node and count for every key. */
static void rebuild(u8 *manager) {
    u8 *head = list_head(manager);
    u8 *node;
    u32 steps = 0;

    clear();
    g_manager = manager;
    ++g_stats.rebuilds;
    if (head == NULL) return;
    for (node = node_next(head); node != head && steps++ < WALK_LIMIT; node = node_next(node)) {
        u32 key;
        IndexEntry *entry;
        if (!node_key(node, &key)) continue;
        entry = lookup(key);
        if (entry == NULL) {
            entry = add(key);
            if (entry == NULL) return; /* too many keys: stay invalid, walk */
            entry->first = node;
        }
        ++entry->count;
    }
    g_valid = node == head;
}

static BOOL linked(const u8 *node) {
    return node_next(node_prev(node)) == node && node_prev(node_next(node)) == node;
}

void particle_index_setup(const ParticleIndexSetup *setup) {
    g_setup = *setup;
    clear();
    g_manager = NULL;
}

void particle_index_reset(void) {
    clear();
    g_manager = NULL;
}

u8 *particle_index_find(u8 *manager, u8 *system) {
    u8 *head = list_head(manager);
    u32 key = *(const u32 *)(system + g_setup.key_offset);
    IndexEntry *entry;
    u8 *result;

    ++g_stats.finds;
    if (!g_setup.enabled) return walk(head, key);
    if (manager != g_manager || !g_valid) rebuild(manager);
    if (!g_valid) return walk(head, key);

    entry = lookup(key);
    if (entry == NULL) {
        ++g_stats.misses;
        result = head;
    } else if (entry->first != NULL && linked(entry->first)) {
        u32 first_key;
        if (node_key(entry->first, &first_key) && first_key == key) {
            ++g_stats.hits;
            result = entry->first;
        } else {
            rebuild(manager);
            result = walk(head, key);
        }
    } else {
        /* The first node is unknown (or no longer linked): find it once. */
        result = walk(head, key);
        if (result == head) {
            rebuild(manager);
        } else {
            entry->first = result;
        }
    }
    if (g_setup.verify) {
        u8 *expected = walk(head, key);
        if (expected != result) {
            ++g_stats.verify_mismatches;
            result = expected;
            g_valid = FALSE;
        }
    }
    return result;
}

u8 *THISCALL particle_index_insert(void *list, u8 **out, u8 *where, void *value) {
    u8 *manager = (u8 *)list - g_setup.list_offset;
    u8 *result = g_setup.insert(list, out, where, value);
    u8 *node = *out;
    u32 key;
    IndexEntry *entry;

    if (!g_setup.enabled || !g_valid || manager != g_manager) {
        g_valid = FALSE;
        return result;
    }
    if (!node_key(node, &key)) return result;
    entry = lookup(key);
    if (entry == NULL) {
        entry = add(key);
        if (entry == NULL) {
            g_valid = FALSE;
            return result;
        }
    }
    /* Inserted before the group's first node, or as the key's only node. */
    if (entry->count == 0u || entry->first == where) {
        entry->first = node;
    } else {
        entry->first = NULL; /* somewhere else: find the first node later */
    }
    ++entry->count;
    return result;
}

u32 THISCALL particle_index_erase(void *manager, u8 **iterator) {
    u8 *node = *iterator;
    u32 key;

    if (g_setup.enabled && g_valid && (u8 *)manager == g_manager && node != NULL &&
        node != list_head((u8 *)manager) && node_key(node, &key)) {
        IndexEntry *entry = lookup(key);
        if (entry == NULL || entry->count == 0u) {
            g_valid = FALSE;
        } else if (--entry->count == 0u) {
            remove_entry(entry);
            /* Many deleted slots slow probing: start over on the next find. */
            if (g_used >= INDEX_MAX_USED && g_live < INDEX_MAX_USED / 2u) g_valid = FALSE;
        } else if (entry->first == node) {
            u8 *next = node_next(node);
            u32 next_key;
            /* Groups are contiguous: the next node is the new first, if it matches. */
            entry->first = next != list_head((u8 *)manager) && node_key(next, &next_key) &&
                                   next_key == key
                               ? next
                               : NULL;
        }
    } else if ((u8 *)manager == g_manager) {
        g_valid = FALSE;
    }
    return g_setup.erase(manager, iterator);
}

/*
 * Called in place of the scan (CALL, then a JMP past it). In: EAX = system,
 * EDI = manager. Out, as the scan left them: EAX = position node, ECX = the
 * list (manager + 0x24, checked by the resolver). EBX, ESI, EDI, EBP and EDX
 * are kept (the scan clobbered ESI and EDX, but nothing reads them).
 */
__attribute__((naked)) void particle_index_scan_stub(void) {
    __asm__ volatile(
        "pushl %%edx\n\t"
        "pushl %%eax\n\t"
        "pushl %%edi\n\t"
        "calll %c0\n\t"
        "addl $8, %%esp\n\t"
        "popl %%edx\n\t"
        "leal 0x24(%%edi), %%ecx\n\t"
        "retl"
        :
        : "i"(particle_index_find));
}

/* Number of separate runs of `key` in the list (1 when contiguous). */
static u32 count_runs(u8 *head, u32 key) {
    u8 *node = node_next(head);
    u32 runs = 0;
    BOOL inside = FALSE;
    u32 steps = 0;

    while (node != head && steps++ < WALK_LIMIT) {
        u32 node_k;
        BOOL match = node_key(node, &node_k) && node_k == key;
        if (match && !inside) ++runs;
        inside = match;
        node = node_next(node);
    }
    return runs;
}

u8 *particle_index_group_first(u8 *list, u32 key) {
    u8 *manager = list - g_setup.list_offset;
    u8 *head = *(u8 **)list;
    u8 *result = NULL;

    ++g_stats.sibling_searches;
    if (g_setup.enabled && g_valid && manager == g_manager) {
        IndexEntry *entry = lookup(key);
        u32 first_key;
        /* The system being set up is in the list, so its key is too. */
        if (entry != NULL && entry->first != NULL && linked(entry->first) &&
                   node_key(entry->first, &first_key) && first_key == key) {
            result = entry->first;
        }
    }
    if (result == NULL) result = walk(head, key);
    if (g_setup.verify) {
        if (result != walk(head, key)) {
            ++g_stats.verify_mismatches;
            result = walk(head, key);
        }
        if (count_runs(head, key) > 1u) ++g_stats.split_groups;
    }
    return result;
}

/*
 * Replaces "MOV EAX,[EBX]; PUSH EDI; MOV EDI,[EAX]; CMP EDI,EAX; JZ end" with
 * "PUSH EDI; CALL stub; JZ end; NOP". In: EBX = the list, [EBP-4] = key.
 * Out: EAX = head, EDI = the key's first node (the loop then stops at the
 * first node with another key; groups are contiguous), flags = CMP EDI,EAX.
 */
__attribute__((naked)) void particle_index_sibling_stub(void) {
    __asm__ volatile(
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "pushl -4(%%ebp)\n\t"
        "pushl %%ebx\n\t"
        "calll %c0\n\t"
        "addl $8, %%esp\n\t"
        "movl %%eax, %%edi\n\t"
        "movl (%%ebx), %%eax\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "cmpl %%eax, %%edi\n\t"
        "retl"
        :
        : "i"(particle_index_group_first));
}

ParticleIndexStats particle_index_take_stats(void) {
    ParticleIndexStats stats = g_stats;
    stats.entries = g_live;
    memset(&g_stats, 0, sizeof(g_stats));
    return stats;
}
