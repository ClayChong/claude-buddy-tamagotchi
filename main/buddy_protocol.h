#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "buddy_pack_xfer.h"
#include "buddy_types.h"

#define BUDDY_EVENT_MALFORMED (-1)
#define BUDDY_EVENT_UNSUPPORTED_COMMAND (-2)
#define BUDDY_EVENT_UNKNOWN_COMMAND (-3)
#define BUDDY_JSON_MAX_DEPTH 16U
/* Worst-case JSON escaping for every protocol field emitted by Task 6. */
#define BUDDY_PROTOCOL_TX_MAX 768U

typedef struct {
    char name[BUDDY_NAME_MAX];
    uint64_t uptime_ms;
    uint64_t free_heap;
    uint64_t approval_count;
    uint64_t denial_count;
    uint64_t queue_overflow_count;
    uint64_t highest_celebrated_level;
    bool encrypted;
    bool battery_available;
    uint8_t battery_percent;
    uint16_t battery_mv;
} buddy_status_report_t;

int buddy_protocol_parse(const char *json, size_t length, buddy_event_t *event);
int buddy_protocol_permission_json(char *json, size_t size, const char *id,
                                   buddy_permission_decision_t decision);
int buddy_protocol_command_ack_json(char *json, size_t size, const char *command,
                                    bool ok, const char *error);
int buddy_protocol_device_status_json(char *json, size_t size,
                                      const buddy_status_report_t *status);

#define BUDDY_TRANSFER_NONE 0
#define BUDDY_TRANSFER_PARSED 1
#define BUDDY_TRANSFER_MALFORMED (-1)

/* Parses char_begin/file/chunk/file_end/char_end. Returns BUDDY_TRANSFER_NONE
 * for any other line, BUDDY_TRANSFER_MALFORMED (with transfer->command set)
 * for a transfer command with bad fields. */
int buddy_protocol_parse_transfer(const char *json, size_t length, buddy_transfer_t *transfer);
int buddy_protocol_transfer_ack_json(char *json, size_t size, const char *command, bool ok,
                                     uint32_t n, const char *error);
/* Decodes standard base64; returns false on bad input or overflow. */
bool buddy_protocol_base64_decode(const char *input, size_t length, uint8_t *output,
                                  size_t capacity, size_t *decoded);
