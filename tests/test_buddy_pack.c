#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "buddy_orchestrator.h"
#include "buddy_pack_format.h"
#include "buddy_pack_xfer.h"
#include "buddy_protocol.h"
#include "buddy_state.h"

/* ---- in-memory pack builder ------------------------------------------ */

#define W 4U
#define H 2U
#define FRAME_BYTES (W * H / 2U)

typedef struct {
    uint16_t first_frame, frame_count, frame_ms;
} clip_spec_t;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

/* states[i] = {first_clip, clip_count}; returns total size. Version 1 packs carry
 * 7 states after the name, version 2 packs carry 14 and version 3 packs 17. */
static size_t build_pack_version(uint8_t *out, uint16_t version, const uint16_t (*states)[2],
                                 unsigned state_count, const clip_spec_t *clips,
                                 uint16_t clip_count, uint16_t frame_count)
{
    size_t fixed = 48U + state_count * 4U;
    size_t header = fixed + clip_count * BUDDY_PACK_CLIP_BYTES + 4U;
    uint32_t payload = frame_count * FRAME_BYTES;
    unsigned i;

    memset(out, 0, header + payload);
    memcpy(out, BUDDY_PACK_MAGIC, 4);
    put16(out + 4, version);
    put16(out + 6, (uint16_t)header);
    put16(out + 8, W);
    put16(out + 10, H);
    put16(out + 12, clip_count);
    put16(out + 14, frame_count);
    put32(out + 16, payload);
    memcpy(out + 24, "ANGEL", 5);
    for (i = 0; i < state_count; ++i) {
        put16(out + 48 + i * 4U, states[i][0]);
        put16(out + 50 + i * 4U, states[i][1]);
    }
    for (i = 0; i < clip_count; ++i) {
        uint8_t *c = out + fixed + i * BUDDY_PACK_CLIP_BYTES;
        put16(c, clips[i].first_frame);
        put16(c + 2, clips[i].frame_count);
        put16(c + 4, clips[i].frame_ms);
    }
    for (i = 0; i < payload; ++i) {
        out[header + i] = (uint8_t)(0x11U * ((i / FRAME_BYTES) % 15U + 1U));
    }
    put32(out + 20, buddy_pack_crc32(0, out + header, payload));
    put32(out + header - 4U, buddy_pack_crc32(0, out, header - 4U));
    return header + payload;
}

static size_t build_pack(uint8_t *out, const uint16_t states[7][2], const clip_spec_t *clips,
                         uint16_t clip_count, uint16_t frame_count)
{
    return build_pack_version(out, 1, states, 7, clips, clip_count, frame_count);
}

/* sleep: clip 0; idle: clips 1-2 (carousel); others missing. */
static const uint16_t k_states[7][2] = {{0, 1}, {1, 2}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
static const clip_spec_t k_clips[3] = {{0, 1, 400}, {1, 2, 100}, {3, 3, 200}};

/* ---- fake flash -------------------------------------------------------- */

#define CAPACITY (4U * BUDDY_PACK_SECTOR_SIZE)

typedef struct {
    uint8_t flash[CAPACITY];
    unsigned erases;
    unsigned invalidations;
    unsigned commits;
    buddy_pack_info_t committed;
} fake_flash_t;

static bool fake_erase(void *context, uint32_t offset, uint32_t length)
{
    fake_flash_t *f = context;
    assert(offset % BUDDY_PACK_SECTOR_SIZE == 0U && length % BUDDY_PACK_SECTOR_SIZE == 0U);
    assert(offset + length <= CAPACITY);
    memset(f->flash + offset, 0xff, length);
    ++f->erases;
    return true;
}

static bool fake_write(void *context, uint32_t offset, const uint8_t *data, uint32_t length)
{
    fake_flash_t *f = context;
    uint32_t i;
    assert(offset + length <= CAPACITY);
    for (i = 0; i < length; ++i) {
        /* NOR flash can only clear bits; writing unerased flash is a bug. */
        assert(f->flash[offset + i] == 0xff);
        f->flash[offset + i] = data[i];
    }
    return true;
}

static bool fake_read(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    fake_flash_t *f = context;
    assert(offset + length <= CAPACITY);
    memcpy(data, f->flash + offset, length);
    return true;
}

static void fake_invalidate(void *context) { ((fake_flash_t *)context)->invalidations++; }

static void fake_commit(void *context, const buddy_pack_info_t *info)
{
    fake_flash_t *f = context;
    f->commits++;
    f->committed = *info;
}

static buddy_pack_storage_t fake_storage(fake_flash_t *f)
{
    buddy_pack_storage_t s = {
        .context = f, .capacity = CAPACITY, .erase = fake_erase, .write = fake_write,
        .read = fake_read, .invalidate = fake_invalidate, .commit = fake_commit,
    };
    memset(f->flash, 0xff, sizeof(f->flash));
    return s;
}

static buddy_transfer_t xfer_cmd(buddy_transfer_command_t command)
{
    buddy_transfer_t t;
    memset(&t, 0, sizeof(t));
    t.command = command;
    return t;
}

static bool upload(buddy_pack_xfer_t *x, const uint8_t *pack, size_t size, size_t chunk,
                   const char *path, const char **error)
{
    buddy_transfer_t t = xfer_cmd(BUDDY_TRANSFER_CHAR_BEGIN);
    uint32_t n;
    size_t off;

    t.total = (uint32_t)size;
    if (!buddy_pack_xfer_handle(x, &t, &n, error)) return false;
    t = xfer_cmd(BUDDY_TRANSFER_FILE);
    snprintf(t.path, sizeof(t.path), "%s", path);
    t.size = (uint32_t)size;
    if (!buddy_pack_xfer_handle(x, &t, &n, error)) return false;
    for (off = 0; off < size; off += chunk) {
        size_t len = size - off < chunk ? size - off : chunk;
        t = xfer_cmd(BUDDY_TRANSFER_CHUNK);
        memcpy(t.data, pack + off, len);
        t.data_length = len;
        if (!buddy_pack_xfer_handle(x, &t, &n, error)) return false;
        assert(n == off + len);
    }
    t = xfer_cmd(BUDDY_TRANSFER_FILE_END);
    if (!buddy_pack_xfer_handle(x, &t, &n, error)) return false;
    assert(n == size);
    t = xfer_cmd(BUDDY_TRANSFER_CHAR_END);
    return buddy_pack_xfer_handle(x, &t, &n, error);
}

/* ---- tests ------------------------------------------------------------- */

static void test_crc_and_base64(void)
{
    uint8_t out[16];
    size_t n;

    assert(buddy_pack_crc32(0, (const uint8_t *)"123456789", 9) == 0xcbf43926U);
    assert(buddy_protocol_base64_decode("TWFu", 4, out, sizeof(out), &n) && n == 3 &&
           memcmp(out, "Man", 3) == 0);
    assert(buddy_protocol_base64_decode("TWE=", 4, out, sizeof(out), &n) && n == 2);
    assert(buddy_protocol_base64_decode("TQ==", 4, out, sizeof(out), &n) && n == 1 &&
           out[0] == 'M');
    assert(buddy_protocol_base64_decode("", 0, out, sizeof(out), &n) && n == 0);
    assert(!buddy_protocol_base64_decode("TWF", 3, out, sizeof(out), &n));
    assert(!buddy_protocol_base64_decode("TW!u", 4, out, sizeof(out), &n));
    assert(!buddy_protocol_base64_decode("T=Fu", 4, out, sizeof(out), &n));
    assert(!buddy_protocol_base64_decode("TQ==TWFu", 8, out, sizeof(out), &n));
    assert(!buddy_protocol_base64_decode("TWFuTWFu", 8, out, 5, &n));
}

static void test_header_validation(void)
{
    static uint8_t pack[1024];
    buddy_pack_info_t info;
    size_t size = build_pack(pack, k_states, k_clips, 3, 6);
    uint16_t bad_states[7][2];
    clip_spec_t bad_clips[3];

    assert(buddy_pack_parse_header(pack, size, &info));
    assert(strcmp(info.name, "ANGEL") == 0 && info.width == W && info.height == H);
    assert(info.frame_count == 6 && info.frame_bytes == FRAME_BYTES);
    assert(buddy_pack_total_size(&info) == size);

    pack[30] ^= 1U; /* name byte: header CRC no longer matches */
    assert(!buddy_pack_parse_header(pack, size, &info));
    assert(!buddy_pack_parse_header(pack, 10, &info));

    memcpy(bad_states, k_states, sizeof(bad_states));
    bad_states[1][1] = 0; /* no idle */
    size = build_pack(pack, bad_states, k_clips, 3, 6);
    assert(!buddy_pack_parse_header(pack, size, &info));

    memcpy(bad_clips, k_clips, sizeof(bad_clips));
    bad_clips[2].frame_count = 9; /* runs past the last frame */
    size = build_pack(pack, k_states, bad_clips, 3, 6);
    assert(!buddy_pack_parse_header(pack, size, &info));

    memcpy(bad_clips, k_clips, sizeof(bad_clips));
    bad_clips[0].frame_ms = 5; /* too fast */
    size = build_pack(pack, k_states, bad_clips, 3, 6);
    assert(!buddy_pack_parse_header(pack, size, &info));
}

static void test_frame_timing(void)
{
    static uint8_t pack[1024];
    buddy_pack_info_t info;
    uint16_t f;

    assert(buddy_pack_parse_header(pack, build_pack(pack, k_states, k_clips, 3, 6), &info));
    /* sleep: one 1-frame clip */
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_SLEEP, 12345, &f) && f == 0);
    /* idle carousel: clip 1 (frames 1-2, 100 ms) then clip 2 (frames 3-5, 200 ms); 800 ms cycle */
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_IDLE, 0, &f) && f == 1);
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_IDLE, 150, &f) && f == 2);
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_IDLE, 200, &f) && f == 3);
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_IDLE, 799, &f) && f == 5);
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_IDLE, 800, &f) && f == 1);
    /* missing states fall back to idle */
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_HEART, 250, &f) && f == 3);
    assert(!buddy_pack_frame_at(&info, BUDDY_PACK_STATE_COUNT, 0, &f));
}

static void test_upload_flow(void)
{
    static fake_flash_t flash;
    static uint8_t pack[1024];
    buddy_pack_storage_t storage = fake_storage(&flash);
    buddy_pack_xfer_t x;
    buddy_pack_info_t info;
    const char *error = NULL;
    size_t size = build_pack(pack, k_states, k_clips, 3, 6);

    buddy_pack_xfer_init(&x, &storage);
    assert(upload(&x, pack, size, 7, BUDDY_PACK_FILE_NAME, &error));
    assert(flash.commits == 1 && flash.committed.frame_count == 6);
    assert(memcmp(flash.flash, pack, size) == 0);
    assert(buddy_pack_xfer_verify(&x, &info) && strcmp(info.name, "ANGEL") == 0);

    /* An accepted char_begin invalidates the installed pack immediately. */
    {
        buddy_transfer_t t = xfer_cmd(BUDDY_TRANSFER_CHAR_BEGIN);
        uint32_t n;
        t.total = (uint32_t)size;
        assert(buddy_pack_xfer_handle(&x, &t, &n, &error));
        assert(!buddy_pack_xfer_verify(&x, &info));
        buddy_pack_xfer_abort(&x);
    }

    /* Uploading again works and commits again. */
    assert(upload(&x, pack, size, 64, BUDDY_PACK_FILE_NAME, &error));
    assert(flash.commits == 2);
    assert(buddy_pack_xfer_delete(&x) && !buddy_pack_xfer_verify(&x, &info));
}

static void test_upload_errors(void)
{
    static fake_flash_t flash;
    static uint8_t pack[1024];
    buddy_pack_storage_t storage = fake_storage(&flash);
    buddy_pack_xfer_t x;
    buddy_pack_info_t info;
    buddy_transfer_t t;
    const char *error = NULL;
    uint32_t n;
    size_t size = build_pack(pack, k_states, k_clips, 3, 6);

    buddy_pack_xfer_init(&x, &storage);

    /* GIF packs from Claude Desktop are refused with a hint. */
    assert(!upload(&x, pack, size, 64, "idle_0.gif", &error));
    assert(strstr(error, "pack.bin") != NULL);

    /* Too large for the partition. */
    t = xfer_cmd(BUDDY_TRANSFER_CHAR_BEGIN);
    t.total = CAPACITY + 1U;
    assert(!buddy_pack_xfer_handle(&x, &t, &n, &error) && strcmp(error, "pack too large") == 0);

    /* Commands out of order. */
    t = xfer_cmd(BUDDY_TRANSFER_CHUNK);
    t.data_length = 1;
    assert(!buddy_pack_xfer_handle(&x, &t, &n, &error));
    t = xfer_cmd(BUDDY_TRANSFER_CHAR_END);
    assert(!buddy_pack_xfer_handle(&x, &t, &n, &error));

    /* Chunk overflowing the announced file size. */
    t = xfer_cmd(BUDDY_TRANSFER_CHAR_BEGIN);
    t.total = 10;
    assert(buddy_pack_xfer_handle(&x, &t, &n, &error));
    t = xfer_cmd(BUDDY_TRANSFER_FILE);
    strcpy(t.path, BUDDY_PACK_FILE_NAME);
    t.size = 10;
    assert(buddy_pack_xfer_handle(&x, &t, &n, &error));
    t = xfer_cmd(BUDDY_TRANSFER_CHUNK);
    t.data_length = 11;
    assert(!buddy_pack_xfer_handle(&x, &t, &n, &error));
    t = xfer_cmd(BUDDY_TRANSFER_FILE_END);
    assert(!buddy_pack_xfer_handle(&x, &t, &n, &error) && strcmp(error, "incomplete file") == 0);
    buddy_pack_xfer_abort(&x);

    /* Corrupt payload: char_end rejects it and leaves nothing loadable. */
    pack[size - 1] ^= 0xffU;
    assert(!upload(&x, pack, size, 64, BUDDY_PACK_FILE_NAME, &error));
    assert(strcmp(error, "invalid pack") == 0);
    assert(!buddy_pack_xfer_verify(&x, &info));
    assert(flash.commits == 0);
}

static void test_protocol_transfer(void)
{
    static buddy_transfer_t t;
    char json[256];
    const char *line;

    line = "{\"cmd\":\"char_begin\",\"name\":\"angel\",\"total\":4096}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_PARSED);
    assert(t.command == BUDDY_TRANSFER_CHAR_BEGIN && t.total == 4096 && strcmp(t.name, "angel") == 0);

    line = "{\"cmd\":\"file\",\"path\":\"pack.bin\",\"size\":12}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_PARSED);
    assert(t.command == BUDDY_TRANSFER_FILE && t.size == 12 && strcmp(t.path, "pack.bin") == 0);

    line = "{\"cmd\":\"chunk\",\"d\":\"TWFuTWE=\"}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_PARSED);
    assert(t.command == BUDDY_TRANSFER_CHUNK && t.data_length == 5 && memcmp(t.data, "ManMa", 5) == 0);

    line = "{\"cmd\":\"chunk\",\"d\":\"not base64!\"}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_MALFORMED);
    assert(t.command == BUDDY_TRANSFER_CHUNK);
    line = "{\"cmd\":\"char_begin\"}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_MALFORMED);
    line = "{\"cmd\":\"file\",\"size\":3}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_MALFORMED);

    line = "{\"cmd\":\"status\"}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_NONE);
    line = "{\"total\":1,\"running\":0,\"waiting\":0,\"msg\":\"x\",\"entries\":[],"
           "\"tokens\":0,\"tokens_today\":0}";
    assert(buddy_protocol_parse_transfer(line, strlen(line), &t) == BUDDY_TRANSFER_NONE);
    assert(buddy_protocol_parse_transfer("{bad", 4, &t) == BUDDY_TRANSFER_NONE);

    assert(buddy_protocol_transfer_ack_json(json, sizeof(json), "chunk", true, 1536, NULL) > 0);
    assert(strcmp(json, "{\"ack\":\"chunk\",\"ok\":true,\"n\":1536}\n") == 0);
    assert(buddy_protocol_transfer_ack_json(json, sizeof(json), "file", false, 0, "bad size") > 0);
    assert(strcmp(json, "{\"ack\":\"file\",\"ok\":false,\"n\":0,\"error\":\"bad size\"}\n") == 0);
}

/* ---- orchestrator routing --------------------------------------------- */

typedef struct {
    buddy_pack_xfer_t xfer;
    char last_sent[256];
    unsigned deletes;
} fake_device_t;

static bool fake_secure(void *context, uint32_t generation)
{
    (void)context;
    return generation == 7U;
}

static esp_err_t fake_send(void *context, const char *data, size_t length, uint32_t generation)
{
    fake_device_t *d = context;
    assert(generation == 7U && length < sizeof(d->last_sent));
    memcpy(d->last_sent, data, length);
    d->last_sent[length] = '\0';
    return ESP_OK;
}

static bool fake_transfer(void *context, const buddy_transfer_t *t, uint32_t *n, const char **error)
{
    return buddy_pack_xfer_handle(&((fake_device_t *)context)->xfer, t, n, error);
}

static esp_err_t fake_delete(void *context)
{
    ((fake_device_t *)context)->deletes++;
    return ESP_OK;
}

static bool rx(buddy_state_t *state, const buddy_orchestrator_ops_t *ops, const char *line,
               buddy_action_t *action)
{
    memset(action, 0, sizeof(*action));
    return buddy_orchestrator_process_rx(state, ops, line, strlen(line), 7U, 1000U, action);
}

static void base64(const uint8_t *in, size_t length, char *out)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    size_t o = 0;

    for (i = 0; i < length; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < length) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < length) v |= in[i + 2];
        out[o++] = alphabet[(v >> 18) & 63U];
        out[o++] = alphabet[(v >> 12) & 63U];
        out[o++] = i + 1 < length ? alphabet[(v >> 6) & 63U] : '=';
        out[o++] = i + 2 < length ? alphabet[v & 63U] : '=';
    }
    out[o] = '\0';
}

static void test_orchestrator_routing(void)
{
    static fake_flash_t flash;
    static fake_device_t device;
    static uint8_t pack[1024];
    static char line[BUDDY_JSON_LINE_MAX];
    static char encoded[2048];
    buddy_pack_storage_t storage = fake_storage(&flash);
    buddy_settings_snapshot_t settings = {.ble_enabled = true};
    buddy_orchestrator_ops_t ops = {
        .context = &device, .generation_secure = fake_secure, .send = fake_send,
        .transfer = fake_transfer, .delete_pack = fake_delete,
    };
    buddy_state_t state;
    buddy_action_t action;
    size_t size = build_pack(pack, k_states, k_clips, 3, 6);

    buddy_pack_xfer_init(&device.xfer, &storage);
    buddy_state_init(&state, &settings);
    assert(!state.pack_available && state.species == 0);

    snprintf(line, sizeof(line), "{\"cmd\":\"char_begin\",\"name\":\"angel\",\"total\":%zu}", size);
    assert(rx(&state, &ops, line, &action));
    assert(strcmp(device.last_sent, "{\"ack\":\"char_begin\",\"ok\":true,\"n\":0}\n") == 0);
    snprintf(line, sizeof(line), "{\"cmd\":\"file\",\"path\":\"pack.bin\",\"size\":%zu}", size);
    assert(rx(&state, &ops, line, &action));
    base64(pack, size, encoded);
    snprintf(line, sizeof(line), "{\"cmd\":\"chunk\",\"d\":\"%s\"}", encoded);
    assert(rx(&state, &ops, line, &action));
    assert(strstr(device.last_sent, "\"ack\":\"chunk\",\"ok\":true") != NULL);
    assert(rx(&state, &ops, "{\"cmd\":\"file_end\"}", &action));
    assert(rx(&state, &ops, "{\"cmd\":\"char_end\"}", &action));
    assert(strcmp(device.last_sent, "{\"ack\":\"char_end\",\"ok\":true,\"n\":0}\n") == 0);
    assert(action.type == BUDDY_ACTION_UI_REFRESH);
    assert(state.pack_available && state.species == BUDDY_SPECIES_PACK);

    /* A malformed transfer command is acked as a failure, not ignored. */
    assert(!rx(&state, &ops, "{\"cmd\":\"chunk\",\"d\":\"@@@@\"}", &action));
    assert(strcmp(device.last_sent,
                  "{\"ack\":\"chunk\",\"ok\":false,\"n\":0,\"error\":\"invalid request\"}\n") == 0);

    /* Commands from an unsecured connection are dropped without a reply. */
    device.last_sent[0] = '\0';
    memset(&action, 0, sizeof(action));
    assert(!buddy_orchestrator_process_rx(&state, &ops, "{\"cmd\":\"char_end\"}", 18, 3U, 1000U,
                                          &action));
    assert(device.last_sent[0] == '\0');

    /* Settings -> Reset -> Delete character removes the pack. */
    state.page = BUDDY_PAGE_SETTINGS;
    state.reset_open = true;
    state.reset_selection = BUDDY_RESET_DELETE_CHARACTER;
    {
        buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
        buddy_event_t result;
        memset(&action, 0, sizeof(action));
        buddy_state_reduce(&state, &ok, 2000U, &action);
        assert(action.type == BUDDY_ACTION_DELETE_CHARACTER);
        assert(!state.pack_available && state.species == 0);
        assert(buddy_orchestrator_execute_action(&state, &ops, &action, &result));
        assert(device.deletes == 1);
    }
}

static void test_species_cycle(void)
{
    buddy_settings_snapshot_t settings = {.ble_enabled = true};
    buddy_state_t state;
    unsigned i;

    buddy_state_init(&state, &settings);
    buddy_state_set_pack(&state, true, false);
    state.species = BUDDY_SPECIES_BUILTIN_COUNT - 1U;
    state.page = BUDDY_PAGE_SETTINGS;
    state.settings_selection = BUDDY_SETTINGS_ASCII_PET;
    for (i = 0; i < 2; ++i) {
        buddy_event_t ok = {.type = BUDDY_EVENT_KEY_CLICK, .key = BUDDY_KEY_OK};
        buddy_action_t action;
        memset(&action, 0, sizeof(action));
        buddy_state_reduce(&state, &ok, 1000U, &action);
        assert(state.species == (i == 0 ? BUDDY_SPECIES_PACK : 0U));
    }
    buddy_state_set_pack(&state, true, true);
    assert(state.species == BUDDY_SPECIES_PACK);
    buddy_state_set_pack(&state, false, false);
    assert(state.species == 0);
}

/* idle clip 0, heart clip 1, celebrate clip 2, dizzy clip 3, eat clip 4. */
static const clip_spec_t k_care_clips[5] = {
    {0, 1, 300}, {1, 1, 200}, {2, 1, 100}, {3, 1, 200}, {4, 1, 200}};

static void test_v1_pack_leaves_new_states_empty(void)
{
    static uint8_t pack[1024];
    buddy_pack_info_t info;

    assert(buddy_pack_parse_header(pack, build_pack(pack, k_states, k_clips, 3, 6), &info));
    assert(info.version == 1);
    assert(info.states[BUDDY_PACK_IDLE].clip_count == 2);
    assert(info.states[BUDDY_PACK_EAT].clip_count == 0);
    assert(info.states[BUDDY_PACK_TIRED].clip_count == 0);
}

static void test_v2_pack_reads_fourteen_states(void)
{
    static uint8_t pack[1024];
    uint16_t states[BUDDY_PACK_STATE_COUNT][2] = {{0}};
    buddy_pack_info_t info;
    size_t size;

    states[BUDDY_PACK_IDLE][0] = 0;
    states[BUDDY_PACK_IDLE][1] = 1;
    states[BUDDY_PACK_HEART][0] = 1;
    states[BUDDY_PACK_HEART][1] = 1;
    states[BUDDY_PACK_EAT][0] = 4;
    states[BUDDY_PACK_EAT][1] = 1;
    size = build_pack_version(pack, 2, states, BUDDY_PACK_V2_STATE_COUNT, k_care_clips, 5, 5);
    assert(buddy_pack_parse_header(pack, size, &info));
    assert(info.version == 2);
    assert(info.header_size == 104U + 8U * 5U + 4U);
    assert(info.states[BUDDY_PACK_EAT].first_clip == 4);
    assert(info.states[BUDDY_PACK_EAT].clip_count == 1);
    assert(info.clips[4].frame_ms == 200);
    assert(info.states[BUDDY_PACK_CLEAN].clip_count == 0);
    assert(info.states[BUDDY_PACK_POOP].clip_count == 0);
}

static void test_v3_pack_reads_seventeen_states(void)
{
    static uint8_t pack[1024];
    uint16_t states[BUDDY_PACK_STATE_COUNT][2] = {{0}};
    buddy_pack_info_t info;
    size_t size;

    states[BUDDY_PACK_IDLE][1] = 1;
    states[BUDDY_PACK_CLEAN][0] = 1;
    states[BUDDY_PACK_CLEAN][1] = 1;
    states[BUDDY_PACK_STINKY][0] = 2;
    states[BUDDY_PACK_STINKY][1] = 1;
    states[BUDDY_PACK_POOP][0] = 3;
    states[BUDDY_PACK_POOP][1] = 2;
    size = build_pack_version(pack, 3, states, BUDDY_PACK_STATE_COUNT, k_care_clips, 5, 5);
    assert(buddy_pack_parse_header(pack, size, &info));
    assert(info.version == 3);
    assert(info.header_size == 116U + 8U * 5U + 4U);
    assert(info.states[BUDDY_PACK_CLEAN].first_clip == 1);
    assert(info.states[BUDDY_PACK_STINKY].first_clip == 2);
    assert(info.states[BUDDY_PACK_POOP].first_clip == 3);
    assert(info.states[BUDDY_PACK_POOP].clip_count == 2);
}

static void test_version_4_rejected(void)
{
    static uint8_t pack[1024];
    uint16_t states[BUDDY_PACK_STATE_COUNT][2] = {{0}};
    buddy_pack_info_t info;
    size_t size;

    states[BUDDY_PACK_IDLE][1] = 1;
    size = build_pack_version(pack, 4, states, BUDDY_PACK_STATE_COUNT, k_care_clips, 5, 5);
    assert(!buddy_pack_parse_header(pack, size, &info));
}

static void test_resolve_fallbacks(void)
{
    static uint8_t pack[1024];
    uint16_t states[BUDDY_PACK_STATE_COUNT][2] = {{0}};
    buddy_pack_info_t info;
    buddy_pack_state_t r;
    uint16_t f;
    size_t size;

    states[BUDDY_PACK_IDLE][0] = 0;
    states[BUDDY_PACK_IDLE][1] = 1;
    states[BUDDY_PACK_HEART][0] = 1;
    states[BUDDY_PACK_HEART][1] = 1;
    states[BUDDY_PACK_CELEBRATE][0] = 2;
    states[BUDDY_PACK_CELEBRATE][1] = 1;
    states[BUDDY_PACK_DIZZY][0] = 3;
    states[BUDDY_PACK_DIZZY][1] = 1;
    size = build_pack_version(pack, 3, states, BUDDY_PACK_STATE_COUNT, k_care_clips, 5, 5);
    assert(buddy_pack_parse_header(pack, size, &info));

    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_EAT, &r) && r == BUDDY_PACK_HEART);
    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_PAT, &r) && r == BUDDY_PACK_HEART);
    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_PLAY, &r) && r == BUDDY_PACK_CELEBRATE);
    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_REFUSE, &r) && r == BUDDY_PACK_DIZZY);
    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_SLEEP, &r) && r == BUDDY_PACK_IDLE);
    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_IDLE, &r) && r == BUDDY_PACK_IDLE);
    assert(!buddy_pack_resolve_state(&info, BUDDY_PACK_HUNGRY, &r));
    assert(!buddy_pack_resolve_state(&info, BUDDY_PACK_SAD, &r));
    assert(!buddy_pack_resolve_state(&info, BUDDY_PACK_TIRED, &r));
    assert(buddy_pack_frame_at(&info, BUDDY_PACK_EAT, 0, &f) && f == 1);
    assert(!buddy_pack_frame_at(&info, BUDDY_PACK_HUNGRY, 0, &f));
    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_CLEAN, &r) && r == BUDDY_PACK_HEART);
    assert(!buddy_pack_resolve_state(&info, BUDDY_PACK_STINKY, &r));
    assert(!buddy_pack_resolve_state(&info, BUDDY_PACK_POOP, &r));

    states[BUDDY_PACK_DIZZY][1] = 0;
    size = build_pack_version(pack, 3, states, BUDDY_PACK_STATE_COUNT, k_care_clips, 5, 5);
    assert(buddy_pack_parse_header(pack, size, &info));
    assert(buddy_pack_resolve_state(&info, BUDDY_PACK_REFUSE, &r) && r == BUDDY_PACK_IDLE);
}

int main(void)
{
    test_crc_and_base64();
    test_header_validation();
    test_frame_timing();
    test_v1_pack_leaves_new_states_empty();
    test_v2_pack_reads_fourteen_states();
    test_v3_pack_reads_seventeen_states();
    test_version_4_rejected();
    test_resolve_fallbacks();
    test_upload_flow();
    test_upload_errors();
    test_protocol_transfer();
    test_orchestrator_routing();
    test_species_cycle();
    return 0;
}
