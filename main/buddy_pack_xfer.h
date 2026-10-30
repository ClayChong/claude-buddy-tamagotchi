#pragma once

/*
 * Character upload over the upstream folder-push commands:
 *   char_begin -> file -> chunk... -> file_end -> char_end
 *
 * Only one file is accepted, "pack.bin", in the BPK1 format produced by the
 * host converter. Storage is a raw flash region: erasing its first sector
 * at char_begin invalidates the old pack, and char_end verifies both CRCs
 * before the new pack is used, so an interrupted upload never shows half a
 * character.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buddy_pack_format.h"

#define BUDDY_PACK_SECTOR_SIZE 4096U
#define BUDDY_PACK_FILE_NAME "pack.bin"
#define BUDDY_TRANSFER_NAME_MAX 32U
#define BUDDY_TRANSFER_PATH_MAX 64U
/* A 4096-byte JSON line holds at most ~3000 decoded bytes. */
#define BUDDY_TRANSFER_CHUNK_MAX 3072U

typedef enum {
    BUDDY_TRANSFER_CHAR_BEGIN,
    BUDDY_TRANSFER_FILE,
    BUDDY_TRANSFER_CHUNK,
    BUDDY_TRANSFER_FILE_END,
    BUDDY_TRANSFER_CHAR_END,
} buddy_transfer_command_t;

typedef struct {
    buddy_transfer_command_t command;
    char name[BUDDY_TRANSFER_NAME_MAX];
    char path[BUDDY_TRANSFER_PATH_MAX];
    uint32_t total;
    uint32_t size;
    uint8_t data[BUDDY_TRANSFER_CHUNK_MAX];
    size_t data_length;
} buddy_transfer_t;

typedef struct {
    void *context;
    uint32_t capacity;
    /* offset and length are multiples of BUDDY_PACK_SECTOR_SIZE. */
    bool (*erase)(void *context, uint32_t offset, uint32_t length);
    bool (*write)(void *context, uint32_t offset, const uint8_t *data, uint32_t length);
    bool (*read)(void *context, uint32_t offset, uint8_t *data, uint32_t length);
    /* Called before the region is modified, so readers drop the old pack. */
    void (*invalidate)(void *context);
    /* Called once a verified pack is in place. */
    void (*commit)(void *context, const buddy_pack_info_t *info);
} buddy_pack_storage_t;

typedef struct {
    const buddy_pack_storage_t *storage;
    bool active;
    bool file_open;
    bool file_done;
    uint32_t total;
    uint32_t file_size;
    uint32_t written;
    uint32_t erased_until;
    uint8_t scratch[BUDDY_PACK_MAX_HEADER];
} buddy_pack_xfer_t;

void buddy_pack_xfer_init(buddy_pack_xfer_t *xfer, const buddy_pack_storage_t *storage);

/* Handles one command. Returns the ack result; *n is the byte count to report
 * and *error a short reason on failure. */
bool buddy_pack_xfer_handle(buddy_pack_xfer_t *xfer, const buddy_transfer_t *transfer,
                            uint32_t *n, const char **error);

/* Abandons an upload in progress (for example on disconnect). */
void buddy_pack_xfer_abort(buddy_pack_xfer_t *xfer);

/* Erases the stored pack. */
bool buddy_pack_xfer_delete(buddy_pack_xfer_t *xfer);

/* Reads and fully verifies the stored pack. */
bool buddy_pack_xfer_verify(buddy_pack_xfer_t *xfer, buddy_pack_info_t *info);

const char *buddy_transfer_command_name(buddy_transfer_command_t command);
