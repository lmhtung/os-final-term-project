#include "experiments.h"
#include "allocator_tests.h"
#include "adaptive_heap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ADAPTIVE";
enum { STACK_BYTES = 6144, MAX_REQUESTS = 40, STRESS_STEPS = 1000 };
typedef struct { const char *name; unsigned a_requests, b_requests, inject_every; } scenario_t;
/* B_OVER_RESERVE: while A holds as much as it may, B needs 12 x 256 bytes, more than the
 * 2048-byte ADAPTIVE reserve, so the reserve/borrow trade-off becomes visible. */
static const scenario_t scenarios[] = {
    {"NORMAL", 4, 4, 0}, {"BURST", 20, 4, 0},
    {"PRESSURE", 40, 4, 0}, {"INJECTED", 20, 4, 3},
    {"B_OVER_RESERVE", 40, 12, 0}
};
/* A configuration is an allocator policy plus optional per-group limits. SHARED_QUOTA is
 * one shared 8 KiB region where each group may hold at most 6144 bytes, the same ceiling
 * as ADAPTIVE, so each group is still left 8192 - 6144 bytes. It answers "how is ADAPTIVE
 * different from a shared heap with per-task quotas?". limit 0 keeps the policy default. */
typedef struct { const char *name; ah_policy_t policy; size_t limit; } config_t;
static const config_t configs[] = {
    {"SHARED", AH_SHARED, 0}, {"STATIC", AH_STATIC, 0}, {"ADAPTIVE", AH_ADAPTIVE, 0},
    {"SHARED_QUOTA", AH_SHARED, 6144}
};
typedef struct {
    unsigned ok, no_memory, quota, injected, unexpected;
    uint64_t alloc_us_sum, alloc_us_max, free_us_sum, free_us_max;
    bool data_ok;
} measurements_t;
static measurements_t measured[AH_GROUPS];
static const scenario_t *current;
static bool stress_mode;
static StaticTask_t task_storage[2];
static StackType_t stacks[2][STACK_BYTES / sizeof(StackType_t)];
static StaticSemaphore_t signal_storage[5];
static SemaphoreHandle_t start_a, start_b, b_done, round_done, stress_b_done;

static bool data_matches(void *ptr, size_t bytes, unsigned char pattern)
{ unsigned char *p = ptr; for (size_t i=0; i<bytes; ++i) if (p[i] != pattern) return false; return true; }

static void *allocate_measured(ah_group_t group, size_t bytes)
{
    measurements_t *m = &measured[group]; void *ptr = NULL;
    int64_t before = esp_timer_get_time();
    ah_result_t r = ah_alloc(group, bytes, &ptr);
    uint64_t elapsed = (uint64_t)(esp_timer_get_time() - before);
    m->alloc_us_sum += elapsed; if (elapsed > m->alloc_us_max) m->alloc_us_max = elapsed;
    switch (r) {
        case AH_OK: ++m->ok; break;
        case AH_NO_MEMORY: ++m->no_memory; break;
        case AH_QUOTA: ++m->quota; break;
        case AH_INJECTED: ++m->injected; break;
        default: ++m->unexpected; break;
    }
    if (r == AH_OK && !ptr) ++m->unexpected;
    return ptr;
}

static void release_measured(ah_group_t group, void *ptr, size_t bytes, unsigned char pattern)
{
    measurements_t *m = &measured[group];
    m->data_ok &= data_matches(ptr, bytes, pattern);
    int64_t before = esp_timer_get_time();
    if (ah_free(group, ptr) != AH_OK) ++m->unexpected;
    uint64_t elapsed = (uint64_t)(esp_timer_get_time() - before);
    m->free_us_sum += elapsed; if (elapsed > m->free_us_max) m->free_us_max = elapsed;
}

static void concurrent_stress(ah_group_t group)
{
    for (unsigned i=0; i<STRESS_STEPS; ++i) {
        size_t bytes = 1 + (i * 37u + (unsigned)group * 19u) % 700;
        void *p = allocate_measured(group, bytes);
        if (p) {
            memset(p, group == AH_A ? 0xA5 : 0x5A, bytes);
            if (i % 16 == 0) vTaskDelay(1);
            release_measured(group, p, bytes, group == AH_A ? 0xA5 : 0x5A);
        }
        if (i % 16 == 0) vTaskDelay(1);
    }
}

static void worker_a(void *unused)
{
    (void)unused;
    void *held[MAX_REQUESTS];
    for (;;) {
        xSemaphoreTake(start_a, portMAX_DELAY);
        if (stress_mode) {
            concurrent_stress(AH_A); xSemaphoreGive(round_done); continue;
        }
        size_t count = 0;
        for (unsigned i=0; i<current->a_requests; ++i) {
            void *p = allocate_measured(AH_A, 256);
            if (p) { memset(p, 0xA5, 256); held[count++] = p; }
        }
        xSemaphoreGive(start_b);
        xSemaphoreTake(b_done, portMAX_DELAY);
        for (size_t i=0; i<count; ++i) release_measured(AH_A, held[i], 256, 0xA5);
        xSemaphoreGive(round_done);
    }
}

static void worker_b(void *unused)
{
    (void)unused;
    void *held[MAX_REQUESTS];
    for (;;) {
        xSemaphoreTake(start_b, portMAX_DELAY);
        if (stress_mode) {
            concurrent_stress(AH_B); xSemaphoreGive(stress_b_done); continue;
        }
        size_t count = 0;
        for (unsigned i=0; i<current->b_requests; ++i) {
            void *p = allocate_measured(AH_B, 256);
            if (p) { memset(p, 0x5A, 256); held[count++] = p; }
        }
        for (size_t i=0; i<count; ++i) release_measured(AH_B, held[i], 256, 0x5A);
        xSemaphoreGive(b_done);
    }
}

static bool workers_init(void)
{
    SemaphoreHandle_t signals[5];
    for (unsigned i=0; i<5; ++i) {
        signals[i] = xSemaphoreCreateBinaryStatic(&signal_storage[i]);
        if (!signals[i]) return false;
    }
    start_a = signals[0]; start_b = signals[1]; b_done = signals[2];
    round_done = signals[3]; stress_b_done = signals[4];
    /* This project targets the dual-core ESP32-S3. B has higher CPU priority. */
    TaskHandle_t a = xTaskCreateStaticPinnedToCore(worker_a, "memory_A", STACK_BYTES, NULL, 3, stacks[0], &task_storage[0], 0);
    TaskHandle_t b = xTaskCreateStaticPinnedToCore(worker_b, "critical_B", STACK_BYTES, NULL, 4, stacks[1], &task_storage[1], 1);
    return a && b;
}

static bool finish_is_clean(const ah_stats_t *initial, ah_stats_t *final)
{
    return ah_validate() == AH_OK && ah_get_stats(final) == AH_OK &&
        final->live_blocks == 0 && final->free_payload_bytes == initial->free_payload_bytes &&
        final->group[0].borrowed_bytes == 0 && final->group[1].borrowed_bytes == 0 &&
        measured[0].data_ok && measured[1].data_ok &&
        !measured[0].unexpected && !measured[1].unexpected;
}

static bool run_case(const config_t *config, const scenario_t *scenario, unsigned run)
{
    ah_stats_t initial, final;
    const ah_policy_t selected = config->policy;
    if (ah_reset(selected) != AH_OK) return false;
    if (config->limit && (ah_set_limit(AH_A, config->limit) != AH_OK ||
                          ah_set_limit(AH_B, config->limit) != AH_OK)) return false;
    if (ah_get_stats(&initial) != AH_OK) return false;
    memset(measured, 0, sizeof(measured)); measured[0].data_ok = measured[1].data_ok = true;
    current = scenario; stress_mode = false;
    ah_inject_every(scenario->inject_every, 3);
    xSemaphoreGive(start_a);
    if (xSemaphoreTake(round_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout: stop without resetting storage in use"); return false;
    }
    bool passed = finish_is_clean(&initial, &final);
    /* Verify allocation counts against region capacity and the injection interval.
     * Each region ends with guard_bytes that cannot be allocated. */
    const unsigned unit = 256 + (unsigned)initial.header_bytes, guard = (unsigned)initial.guard_bytes;
    unsigned capacity_a = selected == AH_SHARED ? (8192 - guard) / unit :
        selected == AH_STATIC ? (4096 - guard) / unit :
        (2048 - guard) / unit + (4096 - guard) / unit;
    if (config->limit && config->limit / unit < capacity_a) capacity_a = (unsigned)(config->limit / unit);
    unsigned valid_a = scenario->a_requests - (scenario->inject_every ? scenario->a_requests / scenario->inject_every : 0);
    passed &= measured[AH_A].ok == (valid_a < capacity_a ? valid_a : capacity_a);
    if (!scenario->inject_every) {
        /* Buffers B is guaranteed even when A holds all it may. A allocates first and keeps
         * everything, so what A leaves in a shared region is one contiguous tail. */
        const bool plain_shared = selected == AH_SHARED && !config->limit;
        const unsigned guaranteed_b = config->limit ? (config->limit < 8192 - guard ? (unsigned)(8192 - guard - config->limit) / unit : 0) :
            selected == AH_STATIC ? (4096 - guard) / unit : selected == AH_ADAPTIVE ? (2048 - guard) / unit : 0;
        unsigned expected_b = plain_shared && scenario->a_requests == 40 ? 0 : scenario->b_requests;
        /* Beyond its guarantee B's count is the measurement, so only the guarantee is checked. */
        if (plain_shared || scenario->b_requests <= guaranteed_b) passed &= measured[AH_B].ok == expected_b;
        else passed &= measured[AH_B].ok >= guaranteed_b && measured[AH_B].ok <= scenario->b_requests;
        passed &= measured[AH_B].ok + measured[AH_B].no_memory == scenario->b_requests && !measured[AH_B].quota;
        if (scenario->a_requests == 4) passed &= measured[AH_A].ok == 4;
        if (scenario->a_requests == 20) passed &= selected == AH_STATIC ? measured[AH_A].ok < 20 : measured[AH_A].ok == 20;
    } else {
        passed &= measured[AH_A].injected == scenario->a_requests / scenario->inject_every;
        passed &= measured[AH_B].injected == scenario->b_requests / scenario->inject_every;
        passed &= measured[AH_B].ok + measured[AH_B].injected == scenario->b_requests;
    }
    if (ah_get_stats(&final) != AH_OK) return false;
    for (unsigned g=0; g<AH_GROUPS; ++g) {
        const measurements_t *m = &measured[g];
        const ah_group_stats_t *s = &final.group[g];
        passed &= s->successes == m->ok && s->injected == m->injected &&
            s->no_memory == m->no_memory && s->quota_failures == m->quota;
    }
    printf("RESULT,%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s\n",
        config->name, scenario->name, run,
        measured[0].ok, measured[1].ok, measured[0].no_memory, measured[1].no_memory,
        measured[0].quota, measured[1].quota, measured[0].injected, measured[1].injected,
        (unsigned)final.group[0].peak_bytes, (unsigned)final.group[1].peak_bytes,
        (unsigned)final.group[0].peak_borrowed_bytes, (unsigned)final.free_payload_bytes,
        (unsigned)final.header_bytes, passed ? "PASS" : "FAIL");
    for (unsigned g=0; g<AH_GROUPS; ++g) {
        measurements_t *m = &measured[g];
        unsigned calls = m->ok + m->no_memory + m->quota + m->injected + m->unexpected;
        printf("TIMING,%s,%s,%u,%c,%u,%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 "\n",
            config->name, scenario->name, run, g == 0 ? 'A' : 'B', calls,
            m->alloc_us_sum, m->alloc_us_max, m->ok, m->free_us_sum, m->free_us_max);
    }
    return passed;
}

void adaptive_experiments_run(void)
{
#ifdef CONFIG_FREERTOS_UNICORE
    ESP_LOGE(TAG, "SUITE_FAIL,this experiment requires dual-core FreeRTOS");
    return;
#endif
    const char *validate_mode = ah_validate_each_call() ? "each_call" : "boundary";
    ESP_LOGI(TAG, "SUITE_BEGIN,adaptive-v2,IDF=%s,arena=%u,repeats=%d,validate=%s", esp_get_idf_version(), AH_ARENA_BYTES, CONFIG_APP_EXPERIMENT_REPEATS, validate_mode);
    ESP_LOGI(TAG, "Reservations ADAPTIVE: A=2048 B=2048 common=4096; all limits include headers/alignment");
    ESP_LOGI(TAG, "SHARED_QUOTA: one shared 8192 region, limit 6144 per group (same ceiling as ADAPTIVE)");
    if (ah_validate_each_call())
        ESP_LOGI(TAG, "Invariant checks are enabled inside allocator operations; timings include these checks and lock wait");
    else
        ESP_LOGI(TAG, "Invariant checks run at round boundaries only; timings include lock wait but not the invariant walk");
    if (!ah_init() || !allocator_tests_run() || !workers_init()) {
        ESP_LOGE(TAG, "SUITE_FAIL,initialization_or_selftest"); return;
    }
    /* Concurrent allocation and release on both ESP32-S3 cores. */
    for (int p=AH_SHARED; p<=AH_ADAPTIVE; ++p) {
        ah_stats_t before, after;
        if (ah_reset((ah_policy_t)p) != AH_OK || ah_get_stats(&before) != AH_OK) {
            ESP_LOGE(TAG, "SUITE_FAIL,stress_setup"); return;
        }
        memset(measured, 0, sizeof(measured)); measured[0].data_ok = measured[1].data_ok = true;
        stress_mode = true;
        xSemaphoreGive(start_a); xSemaphoreGive(start_b);
        bool done_a = xSemaphoreTake(round_done, pdMS_TO_TICKS(30000)) == pdTRUE;
        bool done_b = xSemaphoreTake(stress_b_done, pdMS_TO_TICKS(30000)) == pdTRUE;
        bool ok = done_a && done_b && finish_is_clean(&before, &after) &&
            measured[0].ok == STRESS_STEPS && measured[1].ok == STRESS_STEPS;
        ESP_LOGI(TAG, "TEST,smp_%s_2000_allocations,%s", ah_policy_name((ah_policy_t)p), ok ? "PASS" : "FAIL");
        if (!ok) { ESP_LOGE(TAG, "SUITE_FAIL,stress"); return; }
    }
    ESP_LOGI(TAG, "CSV_HEADER,policy,scenario,run,a_ok,b_ok,a_no_memory,b_no_memory,a_quota,b_quota,a_injected,b_injected,a_peak_charged,b_peak_charged,a_peak_borrowed,free_payload_after,header_bytes,result");
    ESP_LOGI(TAG, "TIMING_HEADER,policy,scenario,run,group,alloc_calls,alloc_us_sum,alloc_us_max,free_calls,free_us_sum,free_us_max");
    unsigned cases = 0;
    for (unsigned run=1; run<=CONFIG_APP_EXPERIMENT_REPEATS; ++run) {
        for (unsigned s=0; s<sizeof(scenarios)/sizeof(scenarios[0]); ++s) {
            for (unsigned c=0; c<sizeof(configs)/sizeof(configs[0]); ++c) {
                if (!run_case(&configs[c], &scenarios[s], run)) {
                    ESP_LOGE(TAG, "SUITE_FAIL,case=%u", cases + 1); return;
                }
                ++cases;
                vTaskDelay(1);
            }
        }
    }
    ESP_LOGI(TAG, "SUITE_PASS,cases=%u; press EN once to repeat", cases);
}
