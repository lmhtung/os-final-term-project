#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DEMO_BLOCK_BYTES 256u
#define DEMO_BLOCK_COUNT 32u
#define DEMO_ARENA_BYTES (DEMO_BLOCK_BYTES * DEMO_BLOCK_COUNT)

typedef enum { DEMO_GROUP_A, DEMO_GROUP_B } demo_group_t;
typedef enum { DEMO_SHARED, DEMO_SPLIT } demo_mode_t;
typedef enum {
    DEMO_FREE_OK,
    DEMO_FREE_INVALID_ARGUMENT,
    DEMO_FREE_INVALID_POINTER,
    DEMO_FREE_WRONG_OWNER,
    DEMO_FREE_ALREADY_FREE
} demo_free_result_t;

/* Call once, before creating workers. All storage is static/internal RAM. */
bool demo_pool_init(void);
/* Caller must quiesce workers first. Refuses reset if any block is live. */
bool demo_pool_reset(demo_mode_t mode);
/* Fixed-size allocation: every successful request returns 256 payload bytes. */
void *demo_pool_alloc(demo_group_t group);
demo_free_result_t demo_pool_free(demo_group_t group, void *ptr);
size_t demo_pool_free_blocks(void);
