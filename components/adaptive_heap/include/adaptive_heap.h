#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AH_ARENA_BYTES 8192u
#define AH_ALIGNMENT 16u
#define AH_GROUPS 2u
/* Every region ends with this many guard bytes, so a small overrun past a region is
 * charged to that region instead of corrupting the next one. Counted inside the region. */
#define AH_GUARD_BYTES 16u
typedef enum { AH_A, AH_B } ah_group_t;
typedef enum { AH_SHARED, AH_STATIC, AH_ADAPTIVE } ah_policy_t;
typedef enum {
    AH_OK, AH_INVALID, AH_NO_MEMORY, AH_QUOTA, AH_INJECTED,
    AH_BAD_POINTER, AH_WRONG_OWNER, AH_BUSY, AH_CORRUPT
} ah_result_t;
typedef struct {
    size_t live_bytes, peak_bytes; /* charged span: header + aligned payload */
    size_t borrowed_bytes, peak_borrowed_bytes;
    size_t limit_bytes, granted_bytes;
    uint32_t attempts, successes, no_memory, quota_failures, injected;
    uint32_t corrupt_failures; /* requests refused because a region they needed is quarantined */
    uint32_t group_resets;
} ah_group_stats_t;
typedef struct {
    ah_policy_t policy;
    size_t free_payload_bytes, largest_free_payload_bytes, live_blocks;
    size_t header_bytes, guard_bytes;
    uint32_t faulted_regions; /* bit i: region i quarantined (SHARED: 0; STATIC: 0=A 1=B; ADAPTIVE: 0=A 1=B 2=common) */
    ah_group_stats_t group[AH_GROUPS];
} ah_stats_t;

/* Init once before workers. APIs are task-context only; pointer outputs must be valid. */
bool ah_init(void);
/* Workers must be quiescent. Refuses reset while any allocation is live, including bytes
 * stranded in a quarantined region (release them with ah_reset_group first). */
ah_result_t ah_reset(ah_policy_t policy);
ah_result_t ah_alloc(ah_group_t group, size_t bytes, void **out);
ah_result_t ah_free(ah_group_t group, void *ptr);
/* Fills *out even when it returns AH_CORRUPT; quarantined regions are left out of the
 * free/live totals and reported in faulted_regions. */
ah_result_t ah_get_stats(ah_stats_t *out);
/* Checks every region. A region that fails is quarantined: later calls that need it return
 * AH_CORRUPT, while calls that only use other regions keep working. */
ah_result_t ah_validate(void);
/* Releases every block of `group`; all of that group's pointers become invalid, so its
 * task must be stopped or restarted around the call. Healthy regions keep other groups'
 * blocks. In a quarantined region the group's charge is dropped, and the region is
 * rebuilt once no group holds bytes in it. */
ah_result_t ah_reset_group(ah_group_t group);
/* True when ah_alloc/ah_free validate invariants on every call (default).
 * False in latency-measurement builds (CONFIG_AH_SKIP_PER_CALL_VALIDATION). */
bool ah_validate_each_call(void);
/* Limit counts allocator overhead too. Cannot reduce below current usage/reserve. */
ah_result_t ah_set_limit(ah_group_t group, size_t charged_bytes);
/* Every Nth valid request of selected groups fails before mutation. 0 disables. */
void ah_inject_every(uint32_t n, uint32_t group_mask);
const char *ah_policy_name(ah_policy_t policy);
const char *ah_result_name(ah_result_t result);
