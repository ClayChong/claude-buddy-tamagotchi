#include "buddy_pack_format.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(data[0] | (data[1] << 8));
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

uint32_t buddy_pack_crc32(uint32_t crc, const uint8_t *data, size_t length)
{
    size_t index;
    unsigned bit;

    crc = ~crc;
    for (index = 0; index < length; ++index) {
        crc ^= data[index];
        for (bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

bool buddy_pack_parse_header(const uint8_t *data, size_t length, buddy_pack_info_t *info)
{
    size_t expected_header;
    unsigned index;

    if (data == NULL || info == NULL || length < BUDDY_PACK_FIXED_HEADER + 4U ||
        memcmp(data, BUDDY_PACK_MAGIC, 4) != 0 || read_u16(data + 4) != BUDDY_PACK_VERSION) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->header_size = read_u16(data + 6);
    info->width = read_u16(data + 8);
    info->height = read_u16(data + 10);
    info->clip_count = read_u16(data + 12);
    info->frame_count = read_u16(data + 14);
    info->payload_size = read_u32(data + 16);
    info->payload_crc = read_u32(data + 20);

    if (info->width == 0U || info->height == 0U || (info->width & 1U) != 0U ||
        info->width > BUDDY_PACK_MAX_WIDTH || info->height > BUDDY_PACK_MAX_HEIGHT ||
        info->clip_count == 0U || info->clip_count > BUDDY_PACK_MAX_CLIPS ||
        info->frame_count == 0U || info->frame_count > BUDDY_PACK_MAX_FRAMES) {
        return false;
    }
    expected_header = BUDDY_PACK_FIXED_HEADER + (size_t)info->clip_count * BUDDY_PACK_CLIP_BYTES + 4U;
    if (info->header_size != expected_header || length < expected_header ||
        read_u32(data + expected_header - 4U) !=
            buddy_pack_crc32(0, data, expected_header - 4U)) {
        return false;
    }
    info->frame_bytes = (uint32_t)info->width * info->height / 2U;
    if (info->payload_size != info->frame_bytes * info->frame_count) {
        return false;
    }

    memcpy(info->name, data + 24, BUDDY_PACK_NAME_MAX);
    info->name[BUDDY_PACK_NAME_MAX] = '\0';
    for (index = 0; index < BUDDY_PACK_NAME_MAX && info->name[index] != '\0'; ++index) {
        /* Shown on screen with an ASCII font; reject anything else. */
        if (info->name[index] < 0x20 || info->name[index] > 0x7e) {
            return false;
        }
    }

    for (index = 0; index < BUDDY_PACK_STATE_COUNT; ++index) {
        const uint8_t *entry = data + 48U + index * 4U;
        buddy_pack_state_entry_t *state = &info->states[index];

        state->first_clip = read_u16(entry);
        state->clip_count = read_u16(entry + 2);
        if (state->clip_count != 0U &&
            (uint32_t)state->first_clip + state->clip_count > info->clip_count) {
            return false;
        }
    }
    if (info->states[BUDDY_PACK_IDLE].clip_count == 0U) {
        return false;
    }

    for (index = 0; index < info->clip_count; ++index) {
        const uint8_t *entry = data + BUDDY_PACK_FIXED_HEADER + index * BUDDY_PACK_CLIP_BYTES;
        buddy_pack_clip_t *clip = &info->clips[index];

        clip->first_frame = read_u16(entry);
        clip->frame_count = read_u16(entry + 2);
        clip->frame_ms = read_u16(entry + 4);
        if (clip->frame_count == 0U || clip->frame_ms < BUDDY_PACK_MIN_FRAME_MS ||
            (uint32_t)clip->first_frame + clip->frame_count > info->frame_count) {
            return false;
        }
    }
    return true;
}

uint32_t buddy_pack_total_size(const buddy_pack_info_t *info)
{
    return info == NULL ? 0U : (uint32_t)info->header_size + info->payload_size;
}

bool buddy_pack_frame_at(const buddy_pack_info_t *info, buddy_pack_state_t state,
                         uint64_t elapsed_ms, uint16_t *frame_index)
{
    const buddy_pack_state_entry_t *entry;
    uint64_t cycle_ms = 0;
    uint64_t position;
    unsigned index;

    if (info == NULL || frame_index == NULL || state >= BUDDY_PACK_STATE_COUNT) {
        return false;
    }
    entry = &info->states[state];
    if (entry->clip_count == 0U) {
        entry = &info->states[BUDDY_PACK_IDLE];
    }
    if (entry->clip_count == 0U) {
        return false;
    }
    for (index = 0; index < entry->clip_count; ++index) {
        const buddy_pack_clip_t *clip = &info->clips[entry->first_clip + index];
        cycle_ms += (uint64_t)clip->frame_count * clip->frame_ms;
    }
    position = elapsed_ms % cycle_ms;
    for (index = 0; index < entry->clip_count; ++index) {
        const buddy_pack_clip_t *clip = &info->clips[entry->first_clip + index];
        uint64_t clip_ms = (uint64_t)clip->frame_count * clip->frame_ms;

        if (position < clip_ms) {
            *frame_index = (uint16_t)(clip->first_frame + position / clip->frame_ms);
            return true;
        }
        position -= clip_ms;
    }
    return false;
}
