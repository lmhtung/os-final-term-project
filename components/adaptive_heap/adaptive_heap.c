#include "adaptive_heap.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* AH_VALIDATE_EACH_CALL
 *   1 (default): ah_alloc/ah_free validate the regions they touch on every call.
 *   0: skip that walk inside ah_alloc/ah_free so call latency measures the
 *      allocator itself. ah_validate, ah_get_stats, ah_reset_group still validate,
 *      so callers must call ah_validate() at round boundaries.
 * The Kconfig option is phrased as "skip" so a missing option keeps the safe
 * original behaviour. Native builds may pass -DAH_VALIDATE_EACH_CALL=0/1. */
#ifndef AH_VALIDATE_EACH_CALL
#  ifdef ESP_PLATFORM
#    include "sdkconfig.h"
#  endif
#  ifdef CONFIG_AH_SKIP_PER_CALL_VALIDATION
#    define AH_VALIDATE_EACH_CALL 0
#  else
#    define AH_VALIDATE_EACH_CALL 1
#  endif
#endif
#if AH_VALIDATE_EACH_CALL != 0 && AH_VALIDATE_EACH_CALL != 1
#  error "AH_VALIDATE_EACH_CALL must be 0 or 1"
#endif
#if AH_GUARD_BYTES == 0 || AH_GUARD_BYTES % AH_ALIGNMENT
#  error "AH_GUARD_BYTES must be a non-zero multiple of AH_ALIGNMENT"
#endif

typedef struct block {
    size_t span, requested;
    struct block *next;
    uint32_t owner; /* 0 free, 1 A, 2 B */
} block_t;
#define HEADER ((sizeof(block_t) + AH_ALIGNMENT - 1) & ~(AH_ALIGNMENT - 1))
#define MIN_BLOCK (HEADER + AH_ALIGNMENT)
#define GUARD_BYTE 0xA5u
/* Fault isolation is per region. Each region is followed by AH_GUARD_BYTES of a fixed
 * pattern, so an overrun off the end of a region breaks that region's guard instead of
 * the next region's first header, and the fault is charged to the region that caused it.
 * charged[] lives outside the arena so a corrupted region cannot falsify it. A faulted
 * region is quarantined: nothing walks its lists again until every group that holds
 * bytes in it has been reset. */
typedef struct {
    uint8_t *base; size_t bytes; /* usable bytes; the guard follows them */
    block_t *free, *used;
    size_t charged[AH_GROUPS];
    bool faulted;
} heap_t;
static _Alignas(AH_ALIGNMENT) uint8_t arena[AH_ARENA_BYTES];
static heap_t heaps[3];
static size_t heap_count;
static ah_policy_t policy;
static ah_group_stats_t gs[AH_GROUPS];
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static bool ready;
static uint32_t inject_n, inject_mask, inject_count[AH_GROUPS];

static bool group_ok(ah_group_t group) { return group == AH_A || group == AH_B; }
static size_t reserve(void) { return policy == AH_ADAPTIVE ? 2048 : 0; }
static size_t maximum(void) { return policy == AH_SHARED ? 8192 : policy == AH_STATIC ? 4096 : 6144; }
static size_t initial_grant(void) { return policy == AH_ADAPTIVE ? 2048 : maximum(); }
static bool is_borrow_region(size_t i) { return policy == AH_ADAPTIVE && i == 2; }

static bool header_ok(const heap_t *h, const block_t *b)
{
    uintptr_t p = (uintptr_t)b, start = (uintptr_t)h->base;
    return p >= start && p - start <= h->bytes - HEADER && p % AH_ALIGNMENT == 0;
}

static bool list_ok(const heap_t *h, block_t *b, bool used)
{
    uintptr_t previous_end = 0;
    size_t steps = 0;
    while (b != NULL) {
        if (++steps > h->bytes / MIN_BLOCK || !header_ok(h, b)) return false;
        const uintptr_t p = (uintptr_t)b;
        if (p < previous_end || (!used && p == previous_end)) return false;
        if (b->span < MIN_BLOCK || b->span % AH_ALIGNMENT ||
            b->span > h->bytes - (p - (uintptr_t)h->base)) return false;
        if (used) {
            if (b->owner < 1 || b->owner > AH_GROUPS || b->requested == 0 ||
                b->requested > b->span - HEADER) return false;
        } else if (b->owner != 0 || b->requested != 0) return false;
        previous_end = p + b->span;
        b = b->next;
    }
    return true;
}

static bool region_ok(size_t i)
{
    heap_t *h = &heaps[i];
    size_t charged[AH_GROUPS] = {0};
    for (size_t k = 0; k < AH_GUARD_BYTES; ++k) if (h->base[h->bytes + k] != GUARD_BYTE) return false;
    if (!list_ok(h, h->free, false) || !list_ok(h, h->used, true)) return false;
    block_t *f = h->free, *u = h->used;
    uintptr_t cursor = (uintptr_t)h->base;
    while (f || u) {
        bool take_free = !u || (f && (uintptr_t)f < (uintptr_t)u);
        block_t *b = take_free ? f : u;
        if ((uintptr_t)b != cursor) return false; /* overlap, gap or duplicate */
        cursor += b->span;
        if (take_free) f = f->next;
        else {
            size_t g = b->owner - 1;
            charged[g] += b->span;
            if (policy != AH_SHARED && i < 2 && i != g) return false;
            u = u->next;
        }
    }
    if (cursor != (uintptr_t)h->base + h->bytes) return false;
    for (size_t g = 0; g < AH_GROUPS; ++g) if (charged[g] != h->charged[g]) return false;
    return true;
}

/* Validates region i unless it is already quarantined; quarantines it on failure. */
static bool check_region(size_t i)
{
    if (!heaps[i].faulted && !region_ok(i)) heaps[i].faulted = true;
    return !heaps[i].faulted;
}

/* Per-call path: in measurement builds only the quarantine flag is consulted. */
static bool region_usable(size_t i) { return AH_VALIDATE_EACH_CALL ? check_region(i) : !heaps[i].faulted; }

/* Group counters must agree with the per-region charges. Neither lives in the arena. */
static bool totals_ok(void)
{
    for (size_t g = 0; g < AH_GROUPS; ++g) {
        size_t live = 0, borrowed = 0;
        for (size_t i = 0; i < heap_count; ++i) {
            live += heaps[i].charged[g];
            if (is_borrow_region(i)) borrowed += heaps[i].charged[g];
        }
        if (live != gs[g].live_bytes || borrowed != gs[g].borrowed_bytes || live > gs[g].limit_bytes ||
            gs[g].granted_bytes != initial_grant() + borrowed) return false;
    }
    return true;
}

/* Checks every region (quarantining the bad ones) and the totals. */
static bool validate_locked(void)
{
    bool ok = totals_ok();
    for (size_t i = 0; i < heap_count; ++i) if (!check_region(i)) ok = false;
    return ok;
}

static void region_init(heap_t *h)
{
    block_t *b = (block_t *)h->base;
    *b = (block_t){ .span = h->bytes };
    h->free = b; h->used = NULL; h->faulted = false;
    memset(h->charged, 0, sizeof(h->charged));
    memset(h->base + h->bytes, GUARD_BYTE, AH_GUARD_BYTES);
}

static void uncharge(size_t i, size_t group, size_t bytes)
{
    heaps[i].charged[group] -= bytes;
    gs[group].live_bytes -= bytes;
    if (is_borrow_region(i)) {
        gs[group].borrowed_bytes -= bytes;
        gs[group].granted_bytes = initial_grant() + gs[group].borrowed_bytes;
    }
}

bool ah_init(void)
{
    if (lock != NULL) return false;
    lock = xSemaphoreCreateMutexStatic(&lock_storage);
    return lock != NULL;
}

ah_result_t ah_reset(ah_policy_t selected)
{
    if (!lock || selected < AH_SHARED || selected > AH_ADAPTIVE) return AH_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    /* Live bytes, including bytes stranded in a quarantined region, must first be
     * released with ah_free or ah_reset_group. A region's contents are rebuilt here,
     * so a fault with nothing charged to it does not block the reset. */
    if (ready && (gs[0].live_bytes || gs[1].live_bytes)) { xSemaphoreGive(lock); return AH_BUSY; }
    policy = selected;
    memset(heaps, 0, sizeof(heaps));
    memset(gs, 0, sizeof(gs));
    memset(inject_count, 0, sizeof(inject_count));
    inject_n = inject_mask = 0;
    heap_count = selected == AH_SHARED ? 1 : selected == AH_STATIC ? 2 : 3;
    size_t offset = 0;
    for (size_t i = 0; i < heap_count; ++i) {
        size_t bytes = selected == AH_SHARED ? 8192 : selected == AH_STATIC ? 4096 : (i == 2 ? 4096 : 2048);
        heaps[i].base = arena + offset; heaps[i].bytes = bytes - AH_GUARD_BYTES;
        region_init(&heaps[i]);
        offset += bytes;
    }
    for (size_t g = 0; g < AH_GROUPS; ++g) {
        gs[g].limit_bytes = maximum(); gs[g].granted_bytes = initial_grant();
    }
    ready = true;
    xSemaphoreGive(lock);
    return AH_OK;
}

static block_t *take_block(heap_t *h, size_t needed, size_t budget)
{
    block_t **link = &h->free;
    while (*link) {
        block_t *b = *link;
        if (b->span >= needed) {
            size_t actual = b->span - needed >= MIN_BLOCK ? needed : b->span;
            if (actual <= budget) {
                if (actual < b->span) {
                    block_t *rest = (block_t *)((uint8_t *)b + actual);
                    *rest = (block_t){ .span = b->span - actual, .next = b->next };
                    *link = rest;
                } else *link = b->next;
                b->span = actual;
                block_t **allocated = &h->used;
                while (*allocated && (uintptr_t)*allocated < (uintptr_t)b) allocated = &(*allocated)->next;
                b->next = *allocated; *allocated = b;
                return b;
            }
        }
        link = &b->next;
    }
    return NULL;
}

ah_result_t ah_alloc(ah_group_t group, size_t bytes, void **out)
{
    if (!out) return AH_INVALID;
    *out = NULL;
    if (!lock || !group_ok(group) || bytes == 0 || bytes > SIZE_MAX - HEADER - (AH_ALIGNMENT - 1)) return AH_INVALID;
    const size_t needed = (bytes + HEADER + AH_ALIGNMENT - 1) & ~(AH_ALIGNMENT - 1);
    xSemaphoreTake(lock, portMAX_DELAY);
    ah_result_t result = AH_OK;
    if (!ready) { result = AH_INVALID; goto done; }
    if (AH_VALIDATE_EACH_CALL && !totals_ok()) { result = AH_CORRUPT; goto done; }
    ah_group_stats_t *g = &gs[group];
    ++g->attempts;
    size_t source = policy == AH_SHARED ? 0 : (size_t)group;
    /* Only the regions this request can use are checked, so a fault elsewhere does not stop it. */
    if (!region_usable(source)) { ++g->corrupt_failures; result = AH_CORRUPT; goto done; }
    if (inject_n && (inject_mask & (1u << group)) && ++inject_count[group] % inject_n == 0) {
        ++g->injected; result = AH_INJECTED; goto done;
    }
    size_t budget = g->limit_bytes - g->live_bytes;
    if (needed > budget) { ++g->quota_failures; result = AH_QUOTA; goto done; }
    block_t *b = take_block(&heaps[source], needed, budget);
    if (!b && policy == AH_ADAPTIVE) {
        source = 2;
        if (!region_usable(source)) { ++g->corrupt_failures; result = AH_CORRUPT; goto done; }
        b = take_block(&heaps[source], needed, budget);
    }
    if (!b) { ++g->no_memory; result = AH_NO_MEMORY; goto done; }
    b->requested = bytes; b->owner = (uint32_t)group + 1;
    heaps[source].charged[group] += b->span;
    g->live_bytes += b->span;
    if (g->live_bytes > g->peak_bytes) g->peak_bytes = g->live_bytes;
    if (is_borrow_region(source)) {
        g->borrowed_bytes += b->span;
        if (g->borrowed_bytes > g->peak_borrowed_bytes) g->peak_borrowed_bytes = g->borrowed_bytes;
        g->granted_bytes = initial_grant() + g->borrowed_bytes;
    }
    ++g->successes;
    *out = (uint8_t *)b + HEADER;
done:
    xSemaphoreGive(lock);
    return result;
}

/* Unlinks *link from region i's used list and returns it to the free list, coalescing. */
static void release_block(size_t i, block_t **link)
{
    heap_t *h = &heaps[i];
    block_t *b = *link;
    *link = b->next;
    uncharge(i, b->owner - 1, b->span);
    b->owner = 0; b->requested = 0;
    block_t *prev = NULL, **free_link = &h->free;
    while (*free_link && (uintptr_t)*free_link < (uintptr_t)b) { prev = *free_link; free_link = &prev->next; }
    b->next = *free_link; *free_link = b;
    if (b->next && (uint8_t *)b + b->span == (uint8_t *)b->next) {
        b->span += b->next->span; b->next = b->next->next;
    }
    if (prev && (uint8_t *)prev + prev->span == (uint8_t *)b) {
        prev->span += b->span; prev->next = b->next;
    }
}

ah_result_t ah_free(ah_group_t group, void *ptr)
{
    if (!lock || !group_ok(group)) return AH_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    ah_result_t result = AH_BAD_POINTER;
    if (!ready) { result = AH_INVALID; goto done; }
    if (AH_VALIDATE_EACH_CALL && !totals_ok()) { result = AH_CORRUPT; goto done; }
    if (!ptr) { result = AH_OK; goto done; }
    for (size_t i = 0; i < heap_count; ++i) {
        heap_t *h = &heaps[i];
        if ((uint8_t *)ptr < h->base || (uint8_t *)ptr >= h->base + h->bytes) continue;
        if (!region_usable(i)) { result = AH_CORRUPT; goto done; }
        block_t **link = &h->used;
        while (*link && (void *)((uint8_t *)*link + HEADER) != ptr) link = &(*link)->next;
        if (!*link) break;
        if ((*link)->owner != (uint32_t)group + 1) { result = AH_WRONG_OWNER; goto done; }
        release_block(i, link);
        result = AH_OK; break;
    }
done:
    xSemaphoreGive(lock);
    return result;
}

ah_result_t ah_reset_group(ah_group_t group)
{
    if (!lock || !group_ok(group)) return AH_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    ah_result_t result = AH_OK;
    if (!ready) { result = AH_INVALID; goto done; }
    if (!totals_ok()) { result = AH_CORRUPT; goto done; }
    for (size_t i = 0; i < heap_count; ++i) {
        heap_t *h = &heaps[i];
        if (check_region(i)) {
            /* Healthy region: free this group's blocks one by one; others keep theirs. */
            block_t **link = &h->used;
            while (*link) {
                if ((*link)->owner == (uint32_t)group + 1) release_block(i, link);
                else link = &(*link)->next;
            }
        } else {
            /* Quarantined region: its lists cannot be trusted, so drop the charge without
             * walking them, and rebuild the region once no group holds bytes in it. */
            uncharge(i, group, h->charged[group]);
            size_t remaining = 0;
            for (size_t g = 0; g < AH_GROUPS; ++g) remaining += h->charged[g];
            if (remaining == 0) region_init(h);
        }
    }
    ++gs[group].group_resets;
done:
    xSemaphoreGive(lock);
    return result;
}

ah_result_t ah_get_stats(ah_stats_t *out)
{
    if (!lock || !out) return AH_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!ready) { xSemaphoreGive(lock); return AH_INVALID; }
    const bool ok = validate_locked();
    memset(out, 0, sizeof(*out));
    out->policy = policy; out->header_bytes = HEADER; out->guard_bytes = AH_GUARD_BYTES;
    memcpy(out->group, gs, sizeof(gs));
    for (size_t i = 0; i < heap_count; ++i) {
        if (heaps[i].faulted) { out->faulted_regions |= 1u << i; continue; }
        for (block_t *b = heaps[i].free; b; b = b->next) {
            size_t payload = b->span - HEADER;
            out->free_payload_bytes += payload;
            if (payload > out->largest_free_payload_bytes) out->largest_free_payload_bytes = payload;
        }
        for (block_t *b = heaps[i].used; b; b = b->next) ++out->live_blocks;
    }
    xSemaphoreGive(lock);
    return ok ? AH_OK : AH_CORRUPT;
}

ah_result_t ah_validate(void)
{
    if (!lock) return AH_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    ah_result_t r = !ready ? AH_INVALID : validate_locked() ? AH_OK : AH_CORRUPT;
    xSemaphoreGive(lock); return r;
}

ah_result_t ah_set_limit(ah_group_t group, size_t bytes)
{
    if (!lock || !group_ok(group)) return AH_INVALID;
    xSemaphoreTake(lock, portMAX_DELAY);
    ah_result_t r = AH_OK;
    if (!ready || bytes < reserve() || bytes > maximum()) r = AH_INVALID;
    else if (bytes < gs[group].live_bytes) r = AH_BUSY;
    else gs[group].limit_bytes = bytes;
    xSemaphoreGive(lock); return r;
}

void ah_inject_every(uint32_t n, uint32_t mask)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    inject_n = n; inject_mask = mask; memset(inject_count, 0, sizeof(inject_count));
    xSemaphoreGive(lock);
}

bool ah_validate_each_call(void) { return AH_VALIDATE_EACH_CALL != 0; }

const char *ah_policy_name(ah_policy_t p)
{ return p == AH_SHARED ? "SHARED" : p == AH_STATIC ? "STATIC" : p == AH_ADAPTIVE ? "ADAPTIVE" : "INVALID"; }
const char *ah_result_name(ah_result_t r)
{
    static const char *names[] = {"OK", "INVALID", "NO_MEMORY", "QUOTA", "INJECTED", "BAD_POINTER", "WRONG_OWNER", "BUSY", "CORRUPT"};
    return (unsigned)r < sizeof(names)/sizeof(names[0]) ? names[r] : "UNKNOWN";
}
