#include "allocator_tests.h"
#include "adaptive_heap.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "AH_TEST";
#define REQUIRE(expr) do { if (!(expr)) { ESP_LOGE(TAG, "FAIL line=%d expression=%s", __LINE__, #expr); return false; } } while (0)
static bool pattern(void *p, size_t n, unsigned char value)
{ unsigned char *b = p; for (size_t i=0; i<n; ++i) if (b[i] != value) return false; return true; }
static uint32_t random_next(uint32_t *state)
{ *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5; return *state; }

bool allocator_tests_run(void)
{
    void *p = NULL, *q = NULL;
    ah_stats_t start, now;
    REQUIRE(ah_reset(AH_SHARED) == AH_OK);
    REQUIRE(ah_get_stats(&start) == AH_OK);
    REQUIRE(ah_alloc(AH_A, 0, &p) == AH_INVALID && p == NULL);
    REQUIRE(ah_alloc(AH_A, SIZE_MAX, &p) == AH_INVALID && p == NULL);
    REQUIRE(ah_alloc(AH_A, 16, NULL) == AH_INVALID);
    REQUIRE(ah_alloc((ah_group_t)9, 16, &p) == AH_INVALID);
    REQUIRE(ah_alloc(AH_A, 31, &p) == AH_OK);
    REQUIRE((uintptr_t)p % AH_ALIGNMENT == 0);
    memset(p, 0x5C, 31);
    REQUIRE(ah_free(AH_B, p) == AH_WRONG_OWNER);
    REQUIRE(ah_free(AH_A, (uint8_t *)p + 1) == AH_BAD_POINTER);
    REQUIRE(ah_free(AH_A, &start) == AH_BAD_POINTER);
    REQUIRE(ah_reset(AH_ADAPTIVE) == AH_BUSY);
    REQUIRE(pattern(p, 31, 0x5C));
    REQUIRE(ah_free(AH_A, p) == AH_OK);
    REQUIRE(ah_free(AH_A, p) == AH_BAD_POINTER);
    REQUIRE(ah_free(AH_A, NULL) == AH_OK);
    REQUIRE(ah_get_stats(&now) == AH_OK && now.free_payload_bytes == start.free_payload_bytes);
    REQUIRE(ah_alloc(AH_A, start.free_payload_bytes, &p) == AH_OK);
    REQUIRE(ah_alloc(AH_B, 1, &q) != AH_OK && q == NULL);
    REQUIRE(ah_free(AH_A, p) == AH_OK && ah_validate() == AH_OK);
    ESP_LOGI(TAG, "TEST,api_pointer_alignment_exact_fit,PASS");

    /* Fill the arena so no tail block can hide external fragmentation. */
    void *blocks[64]; size_t count = 0;
    while (count < 64 && ah_alloc(AH_A, 256, &blocks[count]) == AH_OK) ++count;
    REQUIRE(count >= 4 && count < 64);
    REQUIRE(ah_free(AH_A, blocks[0]) == AH_OK); blocks[0] = NULL;
    REQUIRE(ah_free(AH_A, blocks[2]) == AH_OK); blocks[2] = NULL;
    REQUIRE(ah_get_stats(&now) == AH_OK);
    REQUIRE(now.free_payload_bytes >= 400 && now.largest_free_payload_bytes < 400);
    REQUIRE(ah_alloc(AH_A, 400, &q) == AH_NO_MEMORY && q == NULL);
    REQUIRE(ah_free(AH_A, blocks[1]) == AH_OK); blocks[1] = NULL;
    REQUIRE(ah_alloc(AH_A, 400, &q) == AH_OK);
    REQUIRE(ah_free(AH_A, q) == AH_OK);
    for (size_t i=0; i<count; ++i) if (blocks[i]) REQUIRE(ah_free(AH_A, blocks[i]) == AH_OK);
    REQUIRE(ah_get_stats(&now) == AH_OK && now.free_payload_bytes == start.free_payload_bytes);
    ESP_LOGI(TAG, "TEST,fragmentation_and_coalescing,PASS");

    REQUIRE(ah_reset(AH_ADAPTIVE) == AH_OK);
    REQUIRE(ah_alloc(AH_A, 3000, &p) == AH_OK); /* cannot fit reserve: borrow */
    REQUIRE(ah_get_stats(&now) == AH_OK && now.group[AH_A].borrowed_bytes > 0);
    ESP_LOGI(TAG, "ADAPT_EVENT,borrow,live=%u,borrowed=%u,grant=%u,limit=%u",
        (unsigned)now.group[AH_A].live_bytes, (unsigned)now.group[AH_A].borrowed_bytes,
        (unsigned)now.group[AH_A].granted_bytes, (unsigned)now.group[AH_A].limit_bytes);
    REQUIRE(ah_set_limit(AH_A, 2048) == AH_BUSY);
    REQUIRE(ah_free(AH_A, p) == AH_OK);
    REQUIRE(ah_get_stats(&now) == AH_OK && now.group[AH_A].borrowed_bytes == 0);
    REQUIRE(now.group[AH_A].granted_bytes == 2048);
    ESP_LOGI(TAG, "ADAPT_EVENT,return,borrowed=0,grant=2048");
    REQUIRE(ah_set_limit(AH_A, 2048) == AH_OK);
    REQUIRE(ah_alloc(AH_A, 3000, &p) == AH_QUOTA && p == NULL);
    REQUIRE(ah_alloc(AH_B, 512, &q) == AH_OK && ah_free(AH_B, q) == AH_OK);
    REQUIRE(ah_set_limit(AH_A, 6144) == AH_OK);
    REQUIRE(ah_alloc(AH_A, 3000, &p) == AH_OK);
    REQUIRE(ah_free(AH_A, p) == AH_OK);
    ESP_LOGI(TAG, "ADAPT_EVENT,quota_change,2048_to_6144,request_3000=OK");
    ah_inject_every(1, 1u << AH_A);
    REQUIRE(ah_alloc(AH_A, 64, &p) == AH_INJECTED && p == NULL);
    REQUIRE(ah_alloc(AH_B, 64, &q) == AH_OK && ah_free(AH_B, q) == AH_OK);
    ah_inject_every(0, 0);
    REQUIRE(ah_alloc(AH_A, 64, &p) == AH_OK && ah_free(AH_A, p) == AH_OK);
    REQUIRE(ah_validate() == AH_OK);
    ESP_LOGI(TAG, "TEST,borrow_return_quota_injection,PASS");

    /* A overruns its own region by one byte. B must keep working in its region (and keep
     * borrowing under ADAPTIVE); only A is refused, and ah_reset_group(A) recovers it. */
    for (int mode = AH_STATIC; mode <= AH_ADAPTIVE; ++mode) {
        void *kept = NULL;
        REQUIRE(ah_reset((ah_policy_t)mode) == AH_OK);
        REQUIRE(ah_get_stats(&start) == AH_OK);
        const size_t whole = (mode == AH_STATIC ? 4096 : 2048) - start.guard_bytes - start.header_bytes;
        REQUIRE(ah_alloc(AH_A, whole, &p) == AH_OK);
        REQUIRE(ah_get_stats(&now) == AH_OK && now.group[AH_A].borrowed_bytes == 0);
        REQUIRE(ah_alloc(AH_B, 64, &kept) == AH_OK);
        memset(kept, 0x3C, 64);
        ((unsigned char *)p)[whole] ^= 0xFF; /* deliberate overrun into A's guard */
        REQUIRE(ah_alloc(AH_B, 64, &q) == AH_OK && ah_free(AH_B, q) == AH_OK);
        REQUIRE(ah_validate() == AH_CORRUPT);
        REQUIRE(ah_get_stats(&now) == AH_CORRUPT && now.faulted_regions == 1u << AH_A);
        REQUIRE(ah_alloc(AH_A, 64, &q) == AH_CORRUPT && q == NULL);
        REQUIRE(ah_free(AH_A, p) == AH_CORRUPT);
        REQUIRE(ah_alloc(AH_B, 64, &q) == AH_OK && ah_free(AH_B, q) == AH_OK);
        if (mode == AH_ADAPTIVE) REQUIRE(ah_alloc(AH_B, 3000, &q) == AH_OK && ah_free(AH_B, q) == AH_OK);
        REQUIRE(ah_reset((ah_policy_t)mode) == AH_BUSY);
        REQUIRE(ah_reset_group(AH_A) == AH_OK && ah_validate() == AH_OK);
        REQUIRE(ah_get_stats(&now) == AH_OK && now.faulted_regions == 0 && now.group[AH_A].live_bytes == 0);
        REQUIRE(now.group[AH_A].corrupt_failures == 1 && now.group[AH_B].corrupt_failures == 0);
        REQUIRE(pattern(kept, 64, 0x3C) && ah_free(AH_B, kept) == AH_OK);
        REQUIRE(ah_alloc(AH_A, whole, &p) == AH_OK && ah_free(AH_A, p) == AH_OK);
        REQUIRE(ah_get_stats(&now) == AH_OK && now.free_payload_bytes == start.free_payload_bytes);
    }
    ESP_LOGI(TAG, "TEST,region_isolation_and_group_reset,PASS");

    /* A fixed seed and bounded pointer table make variable-size workloads repeatable. */
    for (int mode = AH_SHARED; mode <= AH_ADAPTIVE; ++mode) {
        REQUIRE(ah_reset((ah_policy_t)mode) == AH_OK);
        REQUIRE(ah_get_stats(&start) == AH_OK);
        void *slots[16] = {0}; size_t sizes[16] = {0};
        uint32_t seed = 0x1234ABCD;
        for (size_t step=0; step<4000; ++step) {
            size_t index = random_next(&seed) % 16;
            ah_group_t owner = (ah_group_t)(index % 2);
            if (slots[index]) {
                REQUIRE(pattern(slots[index], sizes[index], (unsigned char)(index + 1)));
                REQUIRE(ah_free(owner, slots[index]) == AH_OK); slots[index] = NULL;
            } else {
                sizes[index] = 1 + random_next(&seed) % 600;
                ah_result_t r = ah_alloc(owner, sizes[index], &slots[index]);
                REQUIRE(r == AH_OK || r == AH_NO_MEMORY || r == AH_QUOTA);
                if (r == AH_OK) memset(slots[index], (int)(index + 1), sizes[index]);
                else REQUIRE(slots[index] == NULL);
            }
            REQUIRE(ah_validate() == AH_OK);
        }
        for (size_t i=0; i<16; ++i) if (slots[i]) {
            REQUIRE(pattern(slots[i], sizes[i], (unsigned char)(i + 1)));
            REQUIRE(ah_free((ah_group_t)(i % 2), slots[i]) == AH_OK);
        }
        REQUIRE(ah_get_stats(&now) == AH_OK && now.live_blocks == 0 && now.free_payload_bytes == start.free_payload_bytes);
        ESP_LOGI(TAG, "TEST,random_%s_4000_seed_1234ABCD,PASS", ah_policy_name((ah_policy_t)mode));
    }
    return true;
}
