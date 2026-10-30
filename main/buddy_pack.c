#include "buddy_pack.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

/* Custom data subtype for the pack region; see partitions.csv. */
#define BUDDY_PACK_PARTITION_SUBTYPE 0x40
#define BUDDY_PACK_PARTITION_LABEL "buddy"

static const char *TAG = "buddy_pack";

static const esp_partition_t *s_partition;
static buddy_pack_storage_t s_storage;
static buddy_pack_xfer_t s_xfer;
static buddy_pack_info_t s_info;
static esp_partition_mmap_handle_t s_map_handle;
static const uint8_t *s_map;
static bool s_mapped;

static void buddy_pack_unmap(void)
{
    if (s_mapped) {
        esp_partition_munmap(s_map_handle);
        s_mapped = false;
        s_map = NULL;
    }
}

static bool buddy_pack_map(const buddy_pack_info_t *info)
{
    const void *pointer = NULL;
    esp_err_t err;

    buddy_pack_unmap();
    err = esp_partition_mmap(s_partition, 0, buddy_pack_total_size(info),
                             ESP_PARTITION_MMAP_DATA, &pointer, &s_map_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mmap failed: %s", esp_err_to_name(err));
        return false;
    }
    s_info = *info;
    s_map = pointer;
    s_mapped = true;
    return true;
}

static bool storage_erase(void *context, uint32_t offset, uint32_t length)
{
    (void)context;
    return esp_partition_erase_range(s_partition, offset, length) == ESP_OK;
}

static bool storage_write(void *context, uint32_t offset, const uint8_t *data, uint32_t length)
{
    (void)context;
    return esp_partition_write(s_partition, offset, data, length) == ESP_OK;
}

static bool storage_read(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    (void)context;
    return esp_partition_read(s_partition, offset, data, length) == ESP_OK;
}

static void storage_invalidate(void *context)
{
    (void)context;
    buddy_pack_unmap();
}

static void storage_commit(void *context, const buddy_pack_info_t *info)
{
    (void)context;
    if (buddy_pack_map(info)) {
        ESP_LOGI(TAG, "installed \"%s\": %u frames, %ux%u", info->name, info->frame_count,
                 info->width, info->height);
    }
}

bool buddy_pack_init(void)
{
    buddy_pack_info_t info;

    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           (esp_partition_subtype_t)BUDDY_PACK_PARTITION_SUBTYPE,
                                           BUDDY_PACK_PARTITION_LABEL);
    if (s_partition == NULL) {
        ESP_LOGW(TAG, "no \"%s\" partition; uploads disabled", BUDDY_PACK_PARTITION_LABEL);
        return false;
    }
    s_storage = (buddy_pack_storage_t){
        .context = NULL,
        .capacity = (uint32_t)s_partition->size,
        .erase = storage_erase,
        .write = storage_write,
        .read = storage_read,
        .invalidate = storage_invalidate,
        .commit = storage_commit,
    };
    buddy_pack_xfer_init(&s_xfer, &s_storage);
    if (buddy_pack_xfer_verify(&s_xfer, &info) && buddy_pack_map(&info)) {
        ESP_LOGI(TAG, "loaded \"%s\"", info.name);
        return true;
    }
    return false;
}

bool buddy_pack_available(void)
{
    return s_mapped;
}

const char *buddy_pack_name(void)
{
    return s_mapped && s_info.name[0] != '\0' ? s_info.name : "CUSTOM";
}

bool buddy_pack_transfer(const buddy_transfer_t *transfer, uint32_t *n, const char **error)
{
    if (s_partition == NULL) {
        if (error != NULL) {
            *error = "no storage partition";
        }
        return false;
    }
    return buddy_pack_xfer_handle(&s_xfer, transfer, n, error);
}

void buddy_pack_abort(void)
{
    buddy_pack_xfer_abort(&s_xfer);
}

bool buddy_pack_delete(void)
{
    return s_partition != NULL && buddy_pack_xfer_delete(&s_xfer);
}

bool buddy_pack_frame(buddy_pack_state_t state, uint64_t elapsed_ms, uint16_t *frame)
{
    return s_mapped && buddy_pack_frame_at(&s_info, state, elapsed_ms, frame);
}

void buddy_pack_size(uint16_t *width, uint16_t *height)
{
    if (width != NULL) *width = s_mapped ? s_info.width : 0U;
    if (height != NULL) *height = s_mapped ? s_info.height : 0U;
}

void buddy_pack_draw(buddy_i4_surface_t *surface, const buddy_i4_clip_t *clip, uint16_t frame,
                     int x, int y)
{
    if (!s_mapped || frame >= s_info.frame_count) {
        return;
    }
    buddy_i4_blit(surface, clip, s_map + s_info.header_size + (size_t)frame * s_info.frame_bytes,
                  s_info.width, s_info.height, x, y);
}
