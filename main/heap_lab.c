#include "adaptive_heap.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { MAX_HANDLES = AH_MAP_CELLS / 2, MAX_BATCH = 40 };
typedef struct {
    void *ptr;
    uint32_t id;
    size_t bytes;
    ah_group_t group;
} handle_t;

typedef struct {
    const char *name;
    ah_policy_t policy;
    size_t limit;
} lab_mode_t;

static const lab_mode_t modes[] = {
    {"ADAPTIVE", AH_ADAPTIVE, 0}, {"STATIC", AH_STATIC, 0},
    {"SHARED", AH_SHARED, 0}, {"SHARED_QUOTA", AH_SHARED, 6144},
};
static const lab_mode_t *mode = &modes[0];
static handle_t handles[MAX_HANDLES];
static uint32_t next_handle;
static ah_map_t map;

static ah_result_t select_mode(const lab_mode_t *selected)
{
    ah_result_t result = ah_reset(selected->policy);
    if (result != AH_OK) return result;
    if (selected->limit) {
        result = ah_set_limit(AH_A, selected->limit);
        if (result == AH_OK) result = ah_set_limit(AH_B, selected->limit);
        if (result != AH_OK) return result;
    }
    mode = selected;
    return AH_OK;
}

static ah_result_t allocate(ah_group_t group, size_t bytes, uint32_t *id)
{
    for (size_t i = 0; i < MAX_HANDLES; ++i) {
        if (handles[i].ptr) continue;
        void *ptr = NULL;
        ah_result_t result = ah_alloc(group, bytes, &ptr);
        if (result != AH_OK) return result;
        ++next_handle;
        if (next_handle == 0) ++next_handle;
        handles[i] = (handle_t){ .ptr = ptr, .id = next_handle, .bytes = bytes, .group = group };
        *id = next_handle;
        return AH_OK;
    }
    return AH_NO_MEMORY;
}

static ah_result_t release(uint32_t id)
{
    for (size_t i = 0; i < MAX_HANDLES; ++i) {
        handle_t *h = &handles[i];
        if (!h->ptr || h->id != id) continue;
        ah_result_t result = ah_free(h->group, h->ptr);
        if (result == AH_OK) *h = (handle_t){0};
        return result;
    }
    return AH_BAD_POINTER;
}

static ah_result_t clear(void)
{
    for (size_t g = 0; g < AH_GROUPS; ++g) {
        ah_result_t result = ah_reset_group((ah_group_t)g);
        if (result != AH_OK) return result;
        for (size_t i = 0; i < MAX_HANDLES; ++i) {
            if (handles[i].group == (ah_group_t)g) handles[i] = (handle_t){0};
        }
    }
    return select_mode(mode);
}

static void reply(uint32_t seq, ah_result_t result, unsigned completed,
                  unsigned requested, uint32_t handle)
{
    const ah_result_t health = ah_get_map(&map);
    printf("LAB,%" PRIu32 ",{\"code\":\"%s\",\"completed\":%u,\"requested\":%u,\"handle\":%" PRIu32
           ",\"health\":\"%s\",\"mode\":\"%s\",\"align\":%u,\"arena\":%u,"
           "\"free\":%zu,\"largest\":%zu,\"live\":%zu,\"header\":%zu,\"guard\":%zu,\"regions\":[",
           seq, ah_result_name(result), completed, requested, handle,
           ah_result_name(health), mode->name, AH_ALIGNMENT, AH_ARENA_BYTES,
           map.stats.free_payload_bytes, map.stats.largest_free_payload_bytes, map.stats.live_blocks,
           map.stats.header_bytes, map.stats.guard_bytes);
    for (size_t i = 0; i < map.region_count; ++i) printf("%s%zu", i ? "," : "", map.region_bytes[i]);
    printf("],\"cells\":\"%s\",\"groups\":[", map.cells);
    for (size_t i = 0; i < AH_GROUPS; ++i) {
        const ah_group_stats_t *g = &map.stats.group[i];
        printf("%s{\"live\":%zu,\"limit\":%zu,\"borrowed\":%zu,\"ok\":%" PRIu32
               ",\"no_memory\":%" PRIu32 ",\"quota\":%" PRIu32 ",\"injected\":%" PRIu32 "}",
               i ? "," : "", g->live_bytes, g->limit_bytes, g->borrowed_bytes,
               g->successes, g->no_memory, g->quota_failures, g->injected);
    }
    printf("],\"blocks\":[");
    bool comma = false;
    for (size_t i = 0; i < MAX_HANDLES; ++i) {
        const handle_t *h = &handles[i];
        if (!h->ptr) continue;
        printf("%s[%" PRIu32 ",\"%c\",%zu]", comma ? "," : "", h->id, h->group == AH_A ? 'A' : 'B', h->bytes);
        comma = true;
    }
    puts("]}");
    fflush(stdout);
}

static bool number(const char *input, uint32_t max, uint32_t *value)
{
    if (!input || !*input || strspn(input, "0123456789") != strlen(input)) return false;
    uint32_t parsed = 0;
    for (const char *p = input; *p; ++p) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (parsed > UINT32_MAX / 10 || (parsed == UINT32_MAX / 10 && digit > UINT32_MAX % 10)) return false;
        parsed = parsed * 10 + digit;
    }
    if (parsed > max) return false;
    *value = parsed;
    return true;
}

static void command(char *line)
{
    char *context;
    const char *prefix = strtok_r(line, " ", &context);
    if (!prefix || strcmp(prefix, "LAB") != 0) return;
    uint32_t seq;
    if (!number(strtok_r(NULL, " ", &context), UINT32_MAX, &seq)) return;
    const char *verb = strtok_r(NULL, " ", &context);
    const char *arg[4];
    for (size_t i = 0; i < 4; ++i) arg[i] = strtok_r(NULL, " ", &context);

    ah_result_t result = AH_INVALID;
    uint32_t handle = 0, bytes, count, requests_a, requests_b, injection;
    unsigned completed = 0, requested = 0;
    if (!verb || arg[3]) {
        reply(seq, result, 0, 0, 0);
        return;
    }
    if (strcmp(verb, "GET") == 0 && !arg[0]) {
        result = AH_OK;
    } else if (strcmp(verb, "CLEAR") == 0 && !arg[0]) {
        result = clear();
    } else if (strcmp(verb, "MODE") == 0 && arg[0] && !arg[1]) {
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            if (strcmp(arg[0], modes[i].name) == 0) {
                result = select_mode(&modes[i]);
                break;
            }
        }
    } else if (strcmp(verb, "FREE") == 0 && !arg[1] && number(arg[0], UINT32_MAX, &handle)) {
        result = release(handle);
    } else if (strcmp(verb, "ALLOC") == 0 && arg[0] && (strcmp(arg[0], "A") == 0 || strcmp(arg[0], "B") == 0)
               && number(arg[1], AH_ARENA_BYTES, &bytes) && bytes && number(arg[2], MAX_BATCH, &count) && count) {
        const ah_group_t group = arg[0][0] == 'A' ? AH_A : AH_B;
        requested = count;
        result = AH_OK;
        for (unsigned i = 0; i < count; ++i) {
            ah_result_t status = allocate(group, bytes, &handle);
            if (status == AH_OK) ++completed;
            else if (result == AH_OK) result = status;
        }
    } else if (strcmp(verb, "PRESET") == 0 && number(arg[0], MAX_BATCH, &requests_a)
               && number(arg[1], MAX_BATCH, &requests_b) && number(arg[2], MAX_BATCH, &injection)) {
        result = select_mode(mode);
        if (result == AH_OK) {
            requested = requests_a + requests_b;
            ah_inject_every(injection, (1u << AH_A) | (1u << AH_B));
            const unsigned requests[] = {requests_a, requests_b};
            for (size_t g = 0; g < AH_GROUPS; ++g) {
                for (unsigned i = 0; i < requests[g]; ++i) {
                    if (allocate((ah_group_t)g, 256, &handle) == AH_OK) ++completed;
                }
            }
            ah_inject_every(0, 0);
        }
    }
    reply(seq, result, completed, requested, handle);
}

void heap_lab_run(void)
{
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    if (!ah_init() || select_mode(mode) != AH_OK ||
        uart_driver_install(port, 1024, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE("HEAP_LAB", "Initialization failed");
        return;
    }
    puts("HEAP_LAB_READY,1");
    fflush(stdout);
    char line[96];
    size_t used = 0;
    bool discard = false;
    uint8_t byte;
    for (;;) {
        int n = uart_read_bytes(port, &byte, 1, portMAX_DELAY);
        if (n < 0) {
            ESP_LOGE("HEAP_LAB", "UART read failed");
            return;
        }
        if (n == 0) continue;
        if (byte == '\n' || byte == '\r') {
            if (!discard && used > 0) {
                line[used] = '\0';
                command(line);
            }
            used = 0;
            discard = false;
        } else if (byte < 32 || byte > 126 || used >= sizeof(line) - 1) {
            discard = true;
        } else if (!discard) {
            line[used++] = (char)byte;
        }
    }
}
