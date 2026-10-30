#include "buddy_pack_xfer.h"

#include <string.h>

void buddy_pack_xfer_init(buddy_pack_xfer_t *xfer, const buddy_pack_storage_t *storage)
{
    if (xfer == NULL) {
        return;
    }
    memset(xfer, 0, sizeof(*xfer));
    xfer->storage = storage;
}

const char *buddy_transfer_command_name(buddy_transfer_command_t command)
{
    switch (command) {
    case BUDDY_TRANSFER_CHAR_BEGIN: return "char_begin";
    case BUDDY_TRANSFER_FILE: return "file";
    case BUDDY_TRANSFER_CHUNK: return "chunk";
    case BUDDY_TRANSFER_FILE_END: return "file_end";
    case BUDDY_TRANSFER_CHAR_END: return "char_end";
    }
    return "unknown";
}

static void buddy_pack_xfer_reset(buddy_pack_xfer_t *xfer)
{
    xfer->active = false;
    xfer->file_open = false;
    xfer->file_done = false;
    xfer->total = 0;
    xfer->file_size = 0;
    xfer->written = 0;
    xfer->erased_until = 0;
}

static bool buddy_pack_xfer_wipe_header(buddy_pack_xfer_t *xfer)
{
    const buddy_pack_storage_t *storage = xfer->storage;

    if (storage->invalidate != NULL) {
        storage->invalidate(storage->context);
    }
    return storage->erase(storage->context, 0, BUDDY_PACK_SECTOR_SIZE);
}

bool buddy_pack_xfer_verify(buddy_pack_xfer_t *xfer, buddy_pack_info_t *info)
{
    const buddy_pack_storage_t *storage;
    uint32_t header_bytes;
    uint32_t offset;
    uint32_t crc = 0;

    if (xfer == NULL || xfer->storage == NULL || info == NULL) {
        return false;
    }
    storage = xfer->storage;
    header_bytes = storage->capacity < BUDDY_PACK_MAX_HEADER ? storage->capacity
                                                             : BUDDY_PACK_MAX_HEADER;
    if (!storage->read(storage->context, 0, xfer->scratch, header_bytes) ||
        !buddy_pack_parse_header(xfer->scratch, header_bytes, info) ||
        buddy_pack_total_size(info) > storage->capacity) {
        return false;
    }
    for (offset = 0; offset < info->payload_size;) {
        uint32_t length = info->payload_size - offset;

        if (length > sizeof(xfer->scratch)) {
            length = sizeof(xfer->scratch);
        }
        if (!storage->read(storage->context, info->header_size + offset, xfer->scratch, length)) {
            return false;
        }
        crc = buddy_pack_crc32(crc, xfer->scratch, length);
        offset += length;
    }
    return crc == info->payload_crc;
}

static bool buddy_pack_xfer_fail(const char **error, const char *reason)
{
    if (error != NULL) {
        *error = reason;
    }
    return false;
}

static bool buddy_pack_xfer_begin(buddy_pack_xfer_t *xfer, const buddy_transfer_t *transfer,
                                  const char **error)
{
    if (transfer->total == 0U || transfer->total > xfer->storage->capacity) {
        return buddy_pack_xfer_fail(error, "pack too large");
    }
    buddy_pack_xfer_reset(xfer);
    if (!buddy_pack_xfer_wipe_header(xfer)) {
        return buddy_pack_xfer_fail(error, "erase failed");
    }
    xfer->active = true;
    xfer->total = transfer->total;
    xfer->erased_until = BUDDY_PACK_SECTOR_SIZE;
    return true;
}

static bool buddy_pack_xfer_file(buddy_pack_xfer_t *xfer, const buddy_transfer_t *transfer,
                                 const char **error)
{
    if (!xfer->active || xfer->file_open || xfer->file_done) {
        return buddy_pack_xfer_fail(error, "unexpected file");
    }
    if (strcmp(transfer->path, BUDDY_PACK_FILE_NAME) != 0) {
        /* GIF packs from Claude Desktop land here; they need host conversion. */
        return buddy_pack_xfer_fail(error, "only pack.bin; convert with buddyd push");
    }
    if (transfer->size == 0U || transfer->size > xfer->total) {
        return buddy_pack_xfer_fail(error, "bad size");
    }
    xfer->file_open = true;
    xfer->file_size = transfer->size;
    xfer->written = 0;
    return true;
}

static bool buddy_pack_xfer_chunk(buddy_pack_xfer_t *xfer, const buddy_transfer_t *transfer,
                                  const char **error)
{
    const buddy_pack_storage_t *storage = xfer->storage;
    uint32_t end;

    if (!xfer->file_open) {
        return buddy_pack_xfer_fail(error, "no open file");
    }
    if (transfer->data_length == 0U ||
        transfer->data_length > xfer->file_size - xfer->written) {
        return buddy_pack_xfer_fail(error, "chunk overflows file");
    }
    end = xfer->written + (uint32_t)transfer->data_length;
    /* Erase lazily, one sector ahead of the write, so no single command
     * blocks the app task for the whole region. */
    while (xfer->erased_until < end) {
        if (!storage->erase(storage->context, xfer->erased_until, BUDDY_PACK_SECTOR_SIZE)) {
            return buddy_pack_xfer_fail(error, "erase failed");
        }
        xfer->erased_until += BUDDY_PACK_SECTOR_SIZE;
    }
    if (!storage->write(storage->context, xfer->written, transfer->data,
                        (uint32_t)transfer->data_length)) {
        return buddy_pack_xfer_fail(error, "write failed");
    }
    xfer->written = end;
    return true;
}

static bool buddy_pack_xfer_end(buddy_pack_xfer_t *xfer, const char **error)
{
    buddy_pack_info_t info;

    if (!xfer->active || !xfer->file_done) {
        return buddy_pack_xfer_fail(error, "no pack received");
    }
    if (!buddy_pack_xfer_verify(xfer, &info) || buddy_pack_total_size(&info) != xfer->file_size) {
        /* Never leave a pack that failed verification readable. */
        (void)buddy_pack_xfer_wipe_header(xfer);
        buddy_pack_xfer_reset(xfer);
        return buddy_pack_xfer_fail(error, "invalid pack");
    }
    buddy_pack_xfer_reset(xfer);
    if (xfer->storage->commit != NULL) {
        xfer->storage->commit(xfer->storage->context, &info);
    }
    return true;
}

bool buddy_pack_xfer_handle(buddy_pack_xfer_t *xfer, const buddy_transfer_t *transfer,
                            uint32_t *n, const char **error)
{
    bool ok = false;

    if (n != NULL) {
        *n = 0;
    }
    if (error != NULL) {
        *error = NULL;
    }
    if (xfer == NULL || xfer->storage == NULL || transfer == NULL) {
        return buddy_pack_xfer_fail(error, "unavailable");
    }
    switch (transfer->command) {
    case BUDDY_TRANSFER_CHAR_BEGIN:
        ok = buddy_pack_xfer_begin(xfer, transfer, error);
        break;
    case BUDDY_TRANSFER_FILE:
        ok = buddy_pack_xfer_file(xfer, transfer, error);
        break;
    case BUDDY_TRANSFER_CHUNK:
        ok = buddy_pack_xfer_chunk(xfer, transfer, error);
        break;
    case BUDDY_TRANSFER_FILE_END:
        if (!xfer->file_open || xfer->written != xfer->file_size) {
            ok = buddy_pack_xfer_fail(error, "incomplete file");
        } else {
            xfer->file_open = false;
            xfer->file_done = true;
            ok = true;
        }
        break;
    case BUDDY_TRANSFER_CHAR_END:
        ok = buddy_pack_xfer_end(xfer, error);
        break;
    }
    if (n != NULL) {
        *n = xfer->written;
    }
    return ok;
}

void buddy_pack_xfer_abort(buddy_pack_xfer_t *xfer)
{
    if (xfer != NULL && xfer->active) {
        /* The header sector was erased at char_begin, so nothing half-written
         * can be mistaken for a pack; just forget the session. */
        buddy_pack_xfer_reset(xfer);
    }
}

bool buddy_pack_xfer_delete(buddy_pack_xfer_t *xfer)
{
    if (xfer == NULL || xfer->storage == NULL) {
        return false;
    }
    buddy_pack_xfer_reset(xfer);
    return buddy_pack_xfer_wipe_header(xfer);
}
