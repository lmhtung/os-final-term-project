#include "demo_pool.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Each block starts on a 16-byte boundary. Metadata is outside the arena. */
static _Alignas(16) uint8_t s_arena[DEMO_ARENA_BYTES];
static uint8_t s_owner[DEMO_BLOCK_COUNT]; /* 0=free, 1=A, 2=B */
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static demo_mode_t s_mode;

static bool valid_group(demo_group_t group)
{
    return group == DEMO_GROUP_A || group == DEMO_GROUP_B;
}

bool demo_pool_init(void)
{
    if (s_lock != NULL) {
        return false;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    return s_lock != NULL;
}

bool demo_pool_reset(demo_mode_t mode)
{
    if (s_lock == NULL || (mode != DEMO_SHARED && mode != DEMO_SPLIT)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < DEMO_BLOCK_COUNT; ++i) {
        if (s_owner[i] != 0) {
            xSemaphoreGive(s_lock);
            return false;
        }
    }
    s_mode = mode;
    xSemaphoreGive(s_lock);
    return true;
}

void *demo_pool_alloc(demo_group_t group)
{
    if (s_lock == NULL || !valid_group(group)) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t begin = 0;
    size_t end = DEMO_BLOCK_COUNT;
    if (s_mode == DEMO_SPLIT) {
        begin = group == DEMO_GROUP_A ? 0 : DEMO_BLOCK_COUNT / 2;
        end = begin + DEMO_BLOCK_COUNT / 2;
    }
    void *result = NULL;
    for (size_t i = begin; i < end; ++i) {
        if (s_owner[i] == 0) {
            s_owner[i] = (uint8_t)group + 1;
            result = &s_arena[i * DEMO_BLOCK_BYTES];
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return result;
}

demo_free_result_t demo_pool_free(demo_group_t group, void *ptr)
{
    if (s_lock == NULL || !valid_group(group)) {
        return DEMO_FREE_INVALID_ARGUMENT;
    }
    if (ptr == NULL) {
        return DEMO_FREE_OK;
    }
    /* Inspect numeric address only; never dereference the untrusted pointer. */
    const uintptr_t address = (uintptr_t)ptr;
    const uintptr_t base = (uintptr_t)s_arena;
    if (address < base || address - base >= sizeof(s_arena) ||
        (address - base) % DEMO_BLOCK_BYTES != 0) {
        return DEMO_FREE_INVALID_POINTER;
    }
    const size_t index = (address - base) / DEMO_BLOCK_BYTES;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    demo_free_result_t result;
    if (s_owner[index] == 0) {
        result = DEMO_FREE_ALREADY_FREE;
    } else if (s_owner[index] != (uint8_t)group + 1) {
        result = DEMO_FREE_WRONG_OWNER;
    } else {
        s_owner[index] = 0;
        result = DEMO_FREE_OK;
    }
    xSemaphoreGive(s_lock);
    return result;
}

size_t demo_pool_free_blocks(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = 0;
    for (size_t i = 0; i < DEMO_BLOCK_COUNT; ++i) {
        count += s_owner[i] == 0;
    }
    xSemaphoreGive(s_lock);
    return count;
}
