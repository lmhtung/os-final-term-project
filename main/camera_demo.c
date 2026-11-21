#include "adaptive_heap.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WIDTH = 160, HEIGHT = 96, TILE = 16, TILE_BYTES = TILE * TILE,
    TILE_COLS = WIDTH / TILE, TILE_COUNT = TILE_COLS * (HEIGHT / TILE),
    BATCH_CAPACITY = AH_ARENA_BYTES / TILE_BYTES,
    MEDIAN_RADIUS = 3, HALO_WIDTH = TILE + 2,
    PERIOD_MS = 250, ACK_TIMEOUT_MS = 4000, STACK_BYTES = 4096,
    START = 1u << 0, SENSOR_READY = 1u << 5,
    READY = (1u << 1) | (1u << 2) | SENSOR_READY, DONE = (1u << 3) | (1u << 4),
};

typedef struct {
    unsigned batch;
    int64_t received_us;
} ack_t;

typedef struct {
    const char *name;
    ah_policy_t policy;
    BaseType_t core;
    EventBits_t ready_bit, done_bit;
    ah_instance_t *heap;
    size_t initial_free;
    const char *error;
    TaskHandle_t worker;
    StaticTask_t tcb;
    StackType_t stack[STACK_BYTES / sizeof(StackType_t)];
    QueueHandle_t acks;
    StaticQueue_t ack_queue;
    uint8_t ack_buffer[sizeof(ack_t)];
} lane_t;

typedef struct {
    int64_t sampled_us;
    int32_t temperature_mc;
    bool sensor_ok;
} sensor_packet_t;

typedef struct {
    const char *status;
    int32_t temperature_mc;
    bool measured;
    bool healthy;
} sample_t;

typedef struct {
    char line[80];
    size_t used;
    bool discard;
} line_reader_t;

static lane_t lanes[] = {
    { .name = "SHARED", .policy = AH_SHARED, .core = 0, .ready_bit = 1u << 1, .done_bit = 1u << 3 },
    { .name = "ADAPTIVE", .policy = AH_ADAPTIVE, .core = 1, .ready_bit = 1u << 2, .done_bit = 1u << 4 },
};
enum { LANE_COUNT = sizeof(lanes) / sizeof(lanes[0]) };
static const char *TAG = "CAMERA_DEMO";
static const uart_port_t uart = CONFIG_ESP_CONSOLE_UART_NUM;
static uint8_t input[WIDTH * HEIGHT];
static uint32_t stream_id;
static int64_t stream_started_us;
static bool telemetry_ok;
static temperature_sensor_handle_t sensor;
static TaskHandle_t sensor_task;
static StaticTask_t sensor_tcb;
static StackType_t sensor_stack[STACK_BYTES / sizeof(StackType_t)];
static StaticSemaphore_t sensor_done_storage;
static SemaphoreHandle_t sensor_done;
static StaticEventGroup_t race_storage;
static EventGroupHandle_t race;

static void emit(uint32_t id, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void emit(uint32_t id, const char *format, ...)
{
    char line[640];
    const int prefix = snprintf(line, sizeof(line), "CAMERA,%" PRIu32 ",", id);
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, args);
    va_end(args);
    if (length < 0 || (size_t)(prefix + length) >= sizeof(line) - 1) abort();
    line[prefix + length] = '\n';
    /* The UART driver serializes writes from both cores. Each write is one full record. */
    uart_write_bytes(uart, line, (size_t)(prefix + length + 1));
}

static int clamp(int index, int length)
{
    if (index < 0) return 0;
    if (index >= length) return length - 1;
    return index;
}

static uint8_t median_pixel(int x, int y)
{
    enum { DIAMETER = 2 * MEDIAN_RADIUS + 1, SIZE = DIAMETER * DIAMETER };
    uint8_t sorted[SIZE];
    unsigned used = 0;
    for (int dy = -MEDIAN_RADIUS; dy <= MEDIAN_RADIUS; ++dy) {
        const uint8_t *row = &input[clamp(y + dy, HEIGHT) * WIDTH];
        for (int dx = -MEDIAN_RADIUS; dx <= MEDIAN_RADIUS; ++dx) {
            const uint8_t value = row[clamp(x + dx, WIDTH)];
            unsigned slot = used++;
            while (slot > 0 && sorted[slot - 1] > value) {
                sorted[slot] = sorted[slot - 1];
                --slot;
            }
            sorted[slot] = value;
        }
    }
    return sorted[SIZE / 2];
}

static void filter_tile(unsigned index, uint8_t *out)
{
    const int left = (int)(index % TILE_COLS) * TILE;
    const int top = (int)(index / TILE_COLS) * TILE;
    uint8_t smoothed[HALO_WIDTH][HALO_WIDTH];
    /* Median reduces noise; the one-pixel halo lets Sobel cross tile boundaries. */
    for (int y = 0; y < HALO_WIDTH; ++y) {
        for (int x = 0; x < HALO_WIDTH; ++x) {
            smoothed[y][x] = median_pixel(clamp(left + x - 1, WIDTH), clamp(top + y - 1, HEIGHT));
        }
    }
    for (int y = 0; y < TILE; ++y) {
        for (int x = 0; x < TILE; ++x) {
            const uint8_t *upper = &smoothed[y][x];
            const uint8_t *mid = &smoothed[y + 1][x];
            const uint8_t *lower = &smoothed[y + 2][x];
            const int gx = upper[2] + 2 * mid[2] + lower[2] - upper[0] - 2 * mid[0] - lower[0];
            const int gy = lower[0] + 2 * lower[1] + lower[2] - upper[0] - 2 * upper[1] - upper[2];
            const int edge = abs(gx) + abs(gy);
            out[y * TILE + x] = (uint8_t)(edge > 255 ? 255 : edge);
        }
    }
}

static void send_tile(const lane_t *lane, unsigned index, const uint8_t *tile)
{
    static const char hex[] = "0123456789abcdef";
    char gray[2 * TILE_BYTES + 1];
    for (unsigned j = 0; j < TILE_BYTES; ++j) {
        gray[2 * j] = hex[tile[j] >> 4];
        gray[2 * j + 1] = hex[tile[j] & 15];
    }
    gray[2 * TILE_BYTES] = '\0';
    emit(stream_id, "{\"event\":\"tile\",\"mode\":\"%s\",\"index\":%u,\"gray\":\"%s\"}", lane->name, index, gray);
}

static int64_t wait_for_ack(const lane_t *lane, unsigned batch)
{
    const TickType_t timeout = pdMS_TO_TICKS(ACK_TIMEOUT_MS);
    const TickType_t start = xTaskGetTickCount();
    TickType_t remaining = timeout;
    ack_t ack;
    while (xQueueReceive(lane->acks, &ack, remaining) == pdTRUE) {
        if (ack.batch == batch) return ack.received_us;
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) break;
        remaining = timeout - elapsed;
    }
    return 0;
}

static const char *process_frame(lane_t *lane)
{
    const int64_t offset = esp_timer_get_time() - stream_started_us;
    emit(stream_id, "{\"event\":\"lane_start\",\"mode\":\"%s\",\"core\":%d,\"offset_us\":%" PRId64 "}",
         lane->name, (int)xPortGetCoreID(), offset);

    unsigned next = 0, batch = 0;
    int64_t filtered_us = 0, last_ack_us = 0;
    while (next < TILE_COUNT) {
        uint8_t *tiles[BATCH_CAPACITY];
        unsigned count = 0;
        const unsigned first = next;
        const char *error = NULL;
        const int64_t started = esp_timer_get_time();
        while (count < BATCH_CAPACITY && next < TILE_COUNT) {
            void *buffer = NULL;
            const ah_result_t status = ah_instance_alloc(lane->heap, AH_A, TILE_BYTES, &buffer);
            if (status != AH_OK) {
                if (status != AH_NO_MEMORY && status != AH_QUOTA) error = "HEAP_BATCH";
                break;
            }
            tiles[count++] = buffer;
            filter_tile(next++, buffer);
        }
        filtered_us += esp_timer_get_time() - started;

        ah_stats_t held;
        if (!error && (count == 0 || ah_instance_get_stats(lane->heap, &held) != AH_OK)) error = "HEAP_BATCH";
        if (!error) {
            emit(stream_id, "{\"event\":\"batch\",\"mode\":\"%s\",\"batch\":%u,\"first\":%u,\"count\":%u,"
                 "\"borrowed\":%zu,\"charged\":%zu}", lane->name, ++batch, first, count,
                 held.group[AH_A].borrowed_bytes, held.group[AH_A].live_bytes);
            for (unsigned i = 0; i < count; ++i) send_tile(lane, first + i, tiles[i]);
            last_ack_us = wait_for_ack(lane, batch);
            if (!last_ack_us) error = "BATCH_ACK";
        }
        for (unsigned i = 0; i < count; ++i) {
            if (ah_instance_free(lane->heap, AH_A, tiles[i]) != AH_OK) error = "HEAP_FINISH";
        }
        if (error) return error;
        emit(stream_id, "{\"event\":\"released\",\"mode\":\"%s\",\"batch\":%u}", lane->name, batch);
    }
    emit(stream_id, "{\"event\":\"done\",\"mode\":\"%s\",\"batches\":%u,\"filter_us\":%" PRId64
         ",\"frame_us\":%" PRId64 "}", lane->name, batch, filtered_us, last_ack_us - stream_started_us);
    return NULL;
}

static void image_worker(void *arg)
{
    lane_t *lane = arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xEventGroupSetBits(race, lane->ready_bit);
        xEventGroupWaitBits(race, START, pdFALSE, pdTRUE, portMAX_DELAY);
        lane->error = process_frame(lane);
        xEventGroupSetBits(race, lane->done_bit);
    }
}

static sample_t prepare_sample(const lane_t *lane, const sensor_packet_t *reading)
{
    void *buffer = NULL;
    const ah_result_t result = ah_instance_alloc(lane->heap, AH_B, sizeof(*reading), &buffer);
    if (result != AH_OK) {
        return (sample_t){ .status = ah_result_name(result), .healthy = result == AH_NO_MEMORY || result == AH_QUOTA };
    }
    sensor_packet_t *packet = buffer;
    *packet = *reading;
    sample_t report = {
        .status = packet->sensor_ok ? "OK" : "SENSOR_ERROR",
        .temperature_mc = packet->temperature_mc,
        .measured = packet->sensor_ok,
    };
    report.healthy = ah_instance_free(lane->heap, AH_B, packet) == AH_OK;
    return report;
}

static bool sample_lanes(const char *phase, unsigned index, int64_t started_us, EventBits_t finished)
{
    float celsius = 0;
    const esp_err_t status = temperature_sensor_get_celsius(sensor, &celsius);
    const sensor_packet_t reading = {
        .sampled_us = esp_timer_get_time(),
        .sensor_ok = status == ESP_OK,
        .temperature_mc = status == ESP_OK ? (int32_t)(celsius * 1000) : 0,
    };
    bool ok = true;
    sample_t reports[LANE_COUNT];
    /* Both allocation attempts precede UART transmission of either result. */
    for (size_t i = 0; i < LANE_COUNT; ++i) {
        if (!(finished & lanes[i].done_bit)) {
            reports[i] = prepare_sample(&lanes[i], &reading);
            if (!reports[i].healthy) ok = false;
        }
    }
    const int64_t elapsed = started_us ? (reading.sampled_us - started_us) / 1000 : 0;
    for (size_t i = 0; i < LANE_COUNT; ++i) {
        if (finished & lanes[i].done_bit) continue;
        char temp[24] = "null";
        if (reports[i].measured) snprintf(temp, sizeof(temp), "%" PRId32, reports[i].temperature_mc);
        emit(stream_id, "{\"event\":\"sample\",\"mode\":\"%s\",\"phase\":\"%s\",\"index\":%u,"
             "\"time_ms\":%" PRId64 ",\"status\":\"%s\",\"temperature_mc\":%s}", lanes[i].name, phase, index,
             elapsed, reports[i].status, temp);
    }
    return ok;
}

static void collect_temperature(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(PERIOD_MS);
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        xEventGroupSetBits(race, SENSOR_READY);
        xEventGroupWaitBits(race, START, pdFALSE, pdTRUE, portMAX_DELAY);
        TickType_t tick = xTaskGetTickCount();
        unsigned index = 0;
        for (;;) {
            const TickType_t elapsed = xTaskGetTickCount() - tick;
            const TickType_t wait = elapsed < period ? period - elapsed : 0;
            if (ulTaskNotifyTake(pdTRUE, wait)) break;
            tick += period;
            if (!sample_lanes("during", ++index, stream_started_us, xEventGroupGetBits(race))) telemetry_ok = false;
        }
        xSemaphoreGive(sensor_done);
    }
}

static bool read_frame(void)
{
    size_t received = 0;
    const int64_t deadline = esp_timer_get_time() + 6000000;
    while (received < sizeof(input) && esp_timer_get_time() < deadline) {
        const int count = uart_read_bytes(uart, input + received, sizeof(input) - received, pdMS_TO_TICKS(100));
        if (count < 0) return false;
        received += (size_t)count;
    }
    return received == sizeof(input);
}

/* Retain an unfinished command across polls while the two workers are running. */
static int read_line(line_reader_t *reader, bool polling)
{
    const TickType_t wait = polling ? pdMS_TO_TICKS(50) : portMAX_DELAY;
    const int64_t deadline = polling ? esp_timer_get_time() + 50000 : 0;
    do {
        uint8_t byte;
        const int count = uart_read_bytes(uart, &byte, 1, wait);
        if (count < 0) return -1;
        if (count == 0) return 0;
        if (byte == '\n') {
            const bool complete = !reader->discard && reader->used > 0;
            reader->line[reader->used] = '\0';
            reader->used = 0;
            reader->discard = false;
            if (complete) return 1;
        } else if (byte != '\r' && !reader->discard) {
            if (byte < 32 || byte > 126 || reader->used == sizeof(reader->line) - 1) reader->discard = true;
            else reader->line[reader->used++] = (char)byte;
        }
    } while (!polling || esp_timer_get_time() < deadline);
    return 0;
}

static void route_ack(const char *line)
{
    uint32_t id;
    unsigned batch;
    char name[16], extra;
    if (sscanf(line, "CAMERA %9" SCNu32 " ACK %15s %u %c", &id, name, &batch, &extra) != 3 ||
        id != stream_id || batch == 0 || batch > TILE_COUNT) return;
    const ack_t ack = { .batch = batch, .received_us = esp_timer_get_time() };
    for (size_t i = 0; i < LANE_COUNT; ++i) {
        if (strcmp(name, lanes[i].name) == 0) {
            xQueueOverwrite(lanes[i].acks, &ack);
            return;
        }
    }
}

static void run_comparison(uint32_t id, uint32_t expected_crc, line_reader_t *reader)
{
    emit(id, "{\"event\":\"ready\"}");
    if (!read_frame()) {
        emit(id, "{\"event\":\"error\",\"reason\":\"IMAGE_TIMEOUT\"}");
        return;
    }
    const uint32_t crc = esp_rom_crc32_le(0, input, sizeof(input));
    if (crc != expected_crc) {
        emit(id, "{\"event\":\"error\",\"reason\":\"IMAGE_CRC\"}");
        return;
    }
    for (size_t i = 0; i < LANE_COUNT; ++i) {
        lane_t *lane = &lanes[i];
        ah_stats_t initial;
        if (ah_instance_reset(lane->heap, lane->policy) != AH_OK ||
            ah_instance_get_stats(lane->heap, &initial) != AH_OK) {
            emit(id, "{\"event\":\"error\",\"mode\":\"%s\",\"reason\":\"HEAP_START\"}", lane->name);
            return;
        }
        lane->initial_free = initial.free_payload_bytes;
        xQueueReset(lane->acks);
    }
    xEventGroupClearBits(race, START | READY | DONE);
    stream_id = id;
    emit(id, "{\"event\":\"start\",\"crc32\":\"%08" PRIx32 "\",\"arena\":%u,\"period_ms\":%d,"
         "\"width\":%d,\"height\":%d}", crc, AH_ARENA_BYTES, PERIOD_MS, WIDTH, HEIGHT);
    telemetry_ok = sample_lanes("before", 0, 0, 0);
    if (!telemetry_ok) {
        emit(id, "{\"event\":\"error\",\"reason\":\"TELEMETRY_START\"}");
        return;
    }

    for (size_t i = 0; i < LANE_COUNT; ++i) xTaskNotifyGive(lanes[i].worker);
    xTaskNotifyGive(sensor_task);
    xEventGroupWaitBits(race, READY, pdFALSE, pdTRUE, portMAX_DELAY);
    stream_started_us = esp_timer_get_time();
    xEventGroupSetBits(race, START);

    while ((xEventGroupGetBits(race) & DONE) != DONE) {
        const int received = read_line(reader, true);
        if (received < 0) {
            xEventGroupWaitBits(race, DONE, pdFALSE, pdTRUE, portMAX_DELAY);
            break;
        }
        if (received > 0) route_ack(reader->line);
    }
    xTaskNotifyGive(sensor_task);
    xSemaphoreTake(sensor_done, portMAX_DELAY);

    for (size_t i = 0; i < LANE_COUNT; ++i) {
        const lane_t *lane = &lanes[i];
        ah_stats_t final;
        const char *error = lane->error;
        if (ah_instance_get_stats(lane->heap, &final) != AH_OK || final.live_blocks != 0 ||
            final.free_payload_bytes != lane->initial_free) error = "HEAP_FINISH";
        if (error) {
            emit(id, "{\"event\":\"error\",\"mode\":\"%s\",\"reason\":\"%s\"}", lane->name, error);
            return;
        }
    }
    if (!telemetry_ok || !sample_lanes("after", 0, stream_started_us, 0)) {
        emit(id, "{\"event\":\"error\",\"reason\":\"TELEMETRY_FINISH\"}");
        return;
    }
    emit(id, "{\"event\":\"complete\"}");
}

static void dispatch(line_reader_t *reader)
{
    char *context;
    const char *prefix = strtok_r(reader->line, " ", &context);
    if (!prefix || strcmp(prefix, "CAMERA") != 0) return;
    const char *sequence = strtok_r(NULL, " ", &context);
    if (!sequence || strlen(sequence) > 9 || strspn(sequence, "0123456789") != strlen(sequence)) return;
    const uint32_t id = (uint32_t)strtoul(sequence, NULL, 10);
    const char *cmd = strtok_r(NULL, " ", &context);
    const char *crc = strtok_r(NULL, " ", &context);
    if (cmd && strcmp(cmd, "PING") == 0 && !crc) {
        emit(id, "{\"event\":\"hello\",\"version\":3}");
    } else if (cmd && strcmp(cmd, "RUN") == 0 && crc && strlen(crc) == 8 &&
               strspn(crc, "0123456789abcdef") == 8 && !strtok_r(NULL, " ", &context)) {
        run_comparison(id, (uint32_t)strtoul(crc, NULL, 16), reader);
    } else {
        emit(id, "{\"event\":\"error\",\"reason\":\"BAD_COMMAND\"}");
    }
}

void camera_demo_run(void)
{
    const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&config, &sensor) != ESP_OK || temperature_sensor_enable(sensor) != ESP_OK ||
        uart_driver_install(uart, 4096, 2048, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize sensor or UART");
        return;
    }
    race = xEventGroupCreateStatic(&race_storage);
    sensor_done = xSemaphoreCreateBinaryStatic(&sensor_done_storage);
    if (!race || !sensor_done) {
        ESP_LOGE(TAG, "Failed to initialize synchronization");
        return;
    }
    for (size_t i = 0; i < LANE_COUNT; ++i) {
        lane_t *lane = &lanes[i];
        lane->heap = ah_instance_get(i);
        lane->acks = xQueueCreateStatic(1, sizeof(ack_t), lane->ack_buffer, &lane->ack_queue);
        if (!ah_instance_init(lane->heap) || !lane->acks) {
            ESP_LOGE(TAG, "Failed to initialize heap or ACK queue for %s", lane->name);
            return;
        }
        lane->worker = xTaskCreateStaticPinnedToCore(image_worker, lane->name, STACK_BYTES, lane, 3,
                                                   lane->stack, &lane->tcb, lane->core);
        if (!lane->worker) {
            ESP_LOGE(TAG, "Failed to initialize image worker for %s", lane->name);
            return;
        }
    }
    sensor_task = xTaskCreateStatic(collect_temperature, "temperature", STACK_BYTES, NULL, 4, sensor_stack, &sensor_tcb);
    if (!sensor_task) {
        ESP_LOGE(TAG, "Failed to initialize sensor worker");
        return;
    }
    puts("CAMERA_DEMO_READY,3");
    fflush(stdout);
    line_reader_t reader = {0};
    int received;
    while ((received = read_line(&reader, false)) >= 0) {
        if (received > 0) dispatch(&reader);
    }
    ESP_LOGE(TAG, "UART read failed");
}
