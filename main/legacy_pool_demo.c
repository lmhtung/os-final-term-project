#include <stdint.h>
#include <string.h>
#include "demo_pool.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "POOL_DEMO";
static StaticSemaphore_t s_signal_storage[4];
static SemaphoreHandle_t s_start_a, s_start_b, s_b_done, s_round_done;
static StaticTask_t s_task_storage[2];
/* ESP-IDF task stack sizes are bytes, not StackType_t elements. */
#define WORKER_STACK_BYTES 4096
static StackType_t s_stacks[2][WORKER_STACK_BYTES / sizeof(StackType_t)];

/* Accesses are ordered by the start/done semaphores, never concurrently. */
static demo_mode_t s_mode;
static size_t s_a_count;
static bool s_b_success, s_a_data_ok, s_b_data_ok, s_cleanup_ok;

static const char *mode_name(void)
{
    return s_mode == DEMO_SHARED ? "SHARED" : "SPLIT";
}

static bool pattern_matches(const void *ptr, uint8_t pattern)
{
    const uint8_t *bytes = ptr;
    for (size_t i = 0; i < DEMO_BLOCK_BYTES; ++i) {
        if (bytes[i] != pattern) {
            return false;
        }
    }
    return true;
}

static void task_a(void *arg)
{
    (void)arg;
    void *held[DEMO_BLOCK_COUNT];
    for (;;) {
        xSemaphoreTake(s_start_a, portMAX_DELAY);
        s_a_count = 0;
        s_a_data_ok = true;
        s_cleanup_ok = true;
        /* At most 33 attempts; never spin on out-of-memory. */
        for (;;) {
            void *ptr = demo_pool_alloc(DEMO_GROUP_A);
            if (ptr == NULL) {
                break;
            }
            if (s_a_count == DEMO_BLOCK_COUNT) {
                /* Defensive guard: never overrun held[]. */
                s_cleanup_ok = false;
                demo_pool_free(DEMO_GROUP_A, ptr);
                break;
            }
            memset(ptr, 0xA5, DEMO_BLOCK_BYTES);
            held[s_a_count++] = ptr;
        }
        ESP_LOGI(TAG, "[%s] A: allocated=%u blocks, next allocation=NO_MEMORY",
                 mode_name(), (unsigned)s_a_count);
        /* A still owns all its blocks while B tries to allocate. */
        xSemaphoreGive(s_start_b);
        xSemaphoreTake(s_b_done, portMAX_DELAY);
        for (size_t i = 0; i < s_a_count; ++i) {
            s_a_data_ok &= pattern_matches(held[i], 0xA5);
            s_cleanup_ok &= demo_pool_free(DEMO_GROUP_A, held[i]) == DEMO_FREE_OK;
        }
        xSemaphoreGive(s_round_done);
    }
}

static void task_b(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_start_b, portMAX_DELAY);
        void *ptr = demo_pool_alloc(DEMO_GROUP_B);
        s_b_success = ptr != NULL;
        s_b_data_ok = true;
        if (ptr != NULL) {
            memset(ptr, 0x5A, DEMO_BLOCK_BYTES);
            s_b_data_ok = pattern_matches(ptr, 0x5A);
            s_b_data_ok &= demo_pool_free(DEMO_GROUP_B, ptr) == DEMO_FREE_OK;
        }
        ESP_LOGI(TAG, "[%s] B: allocation=%s (A still holds its blocks)",
                 mode_name(), s_b_success ? "SUCCESS" : "NO_MEMORY");
        xSemaphoreGive(s_b_done);
    }
}

static bool run_round(demo_mode_t mode)
{
    if (!demo_pool_reset(mode)) {
        ESP_LOGE(TAG, "Cannot begin round: pool must be empty");
        return false;
    }
    s_mode = mode;
    ESP_LOGI(TAG, "===== %s: arena=%u bytes; block=%u bytes =====",
             mode_name(), (unsigned)DEMO_ARENA_BYTES, (unsigned)DEMO_BLOCK_BYTES);
    xSemaphoreGive(s_start_a);
    if (xSemaphoreTake(s_round_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Round timed out; stop demo without resetting live storage");
        return false;
    }
    const size_t expected_a = mode == DEMO_SHARED ? 32 : 16;
    const size_t free_blocks = demo_pool_free_blocks();
    const bool passed = s_a_count == expected_a &&
        s_b_success == (mode == DEMO_SPLIT) && s_a_data_ok &&
        s_b_data_ok && s_cleanup_ok && free_blocks == DEMO_BLOCK_COUNT;
    ESP_LOGI(TAG, "[%s] A_blocks=%u B_success=%u free_after_cleanup=%u/32 RESULT=%s",
             mode_name(), (unsigned)s_a_count, s_b_success,
             (unsigned)free_blocks, passed ? "PASS" : "FAIL");
    return passed;
}

static bool check(const char *name, bool condition)
{
    if (condition) {
        ESP_LOGI(TAG, "[CHECK] %s: PASS", name);
    } else {
        ESP_LOGE(TAG, "[CHECK] %s: FAIL", name);
    }
    return condition;
}

static bool run_invalid_free_checks(void)
{
    if (!demo_pool_reset(DEMO_SPLIT)) {
        return false;
    }
    uint8_t *ptr = demo_pool_alloc(DEMO_GROUP_A);
    if (ptr == NULL) {
        return false;
    }
    memset(ptr, 0xC3, DEMO_BLOCK_BYTES);
    uint8_t outside = 0;
    bool ok = true;
    ok &= check("wrong owner rejected", demo_pool_free(DEMO_GROUP_B, ptr) == DEMO_FREE_WRONG_OWNER);
    ok &= check("interior pointer rejected", demo_pool_free(DEMO_GROUP_A, ptr + 1) == DEMO_FREE_INVALID_POINTER);
    ok &= check("outside pointer rejected", demo_pool_free(DEMO_GROUP_A, &outside) == DEMO_FREE_INVALID_POINTER);
    ok &= check("reset with live block rejected", !demo_pool_reset(DEMO_SHARED));
    ok &= check("live data and allocation preserved", pattern_matches(ptr, 0xC3) && demo_pool_free_blocks() == 31);
    ok &= check("valid free accepted", demo_pool_free(DEMO_GROUP_A, ptr) == DEMO_FREE_OK);
    /* Test before the address is allocated again. */
    ok &= check("double free rejected", demo_pool_free(DEMO_GROUP_A, ptr) == DEMO_FREE_ALREADY_FREE);
    ok &= check("NULL free accepted", demo_pool_free(DEMO_GROUP_A, NULL) == DEMO_FREE_OK);
    ok &= check("all blocks reclaimed", demo_pool_free_blocks() == DEMO_BLOCK_COUNT);
    return ok;
}

void legacy_pool_demo_run(void)
{
    ESP_LOGI(TAG, "Fixed-block pool demonstration; not the final variable-size allocator");
    ESP_LOGI(TAG, "Only the 8 KiB application arena is exhausted; RTOS objects are outside it");
    if (!demo_pool_init()) {
        ESP_LOGE(TAG, "Pool initialization failed");
        return;
    }
    SemaphoreHandle_t signals[4];
    for (size_t i = 0; i < 4; ++i) {
        signals[i] = xSemaphoreCreateBinaryStatic(&s_signal_storage[i]);
        if (signals[i] == NULL) {
            ESP_LOGE(TAG, "Semaphore initialization failed");
            return;
        }
    }
    s_start_a = signals[0]; s_start_b = signals[1];
    s_b_done = signals[2]; s_round_done = signals[3];
    TaskHandle_t a = xTaskCreateStatic(task_a, "demo_a", WORKER_STACK_BYTES,
                                     NULL, 3, s_stacks[0], &s_task_storage[0]);
    TaskHandle_t b = xTaskCreateStatic(task_b, "demo_b", WORKER_STACK_BYTES,
                                     NULL, 3, s_stacks[1], &s_task_storage[1]);
    if (a == NULL || b == NULL) {
        ESP_LOGE(TAG, "Worker creation failed");
        return;
    }
    if (!run_round(DEMO_SHARED) || !run_round(DEMO_SPLIT)) {
        ESP_LOGE(TAG, "DEMO FAIL: see round output above");
        return;
    }
    const bool ok = run_invalid_free_checks();
    ESP_LOGI(TAG, "DEMO %s: press RESET/EN to run again", ok ? "PASS" : "FAIL");
    /* Workers remain blocked. No automatic restart, and no live pool blocks. */
}

