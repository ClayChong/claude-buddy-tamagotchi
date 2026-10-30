#pragma once

/*
 * Character pack binary format (BPK1).
 *
 * The host converts a Claude Desktop character pack (manifest.json + GIFs) or
 * PNG sprite sheets into this format before upload, so the device needs no GIF
 * decoder. Frames are stored as 4-bit indices into the UI palette; index 0 is
 * transparent.
 *
 * Layout (little-endian):
 *   0   char[4]  magic "BPK1"
 *   4   u16      version (1, 2 or 3)
 *   6   u16      header_size, bytes before the first frame
 *   8   u16      frame width (even)
 *   10  u16      frame height
 *   12  u16      clip count
 *   14  u16      frame count
 *   16  u32      payload size (frame_count * frame_bytes)
 *   20  u32      payload CRC-32
 *   24  char[24] name, NUL padded
 *   48  N x {u16 first_clip, u16 clip_count}, in buddy_pack_state_t order;
 *       N is 7 in version 1, 14 in version 2 and 17 in version 3
 *   F   clip_count x {u16 first_frame, u16 frame_count, u16 frame_ms, u16 reserved};
 *       F is 76 in version 1, 104 in version 2 and 116 in version 3
 *   ..  u32      header CRC-32 over bytes [0, header_size - 4)
 *   header_size: frames, row-major, two pixels per byte, high nibble = left pixel
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUDDY_PACK_MAGIC "BPK1"
#define BUDDY_PACK_VERSION 3U
#define BUDDY_PACK_NAME_MAX 24U
#define BUDDY_PACK_V1_STATE_COUNT 7U
#define BUDDY_PACK_FIXED_HEADER_V1 76U
#define BUDDY_PACK_V2_STATE_COUNT 14U
#define BUDDY_PACK_FIXED_HEADER_V2 104U
#define BUDDY_PACK_FIXED_HEADER 116U
#define BUDDY_PACK_CLIP_BYTES 8U
#define BUDDY_PACK_MAX_CLIPS 64U
#define BUDDY_PACK_MAX_FRAMES 512U
#define BUDDY_PACK_MAX_WIDTH 120U
#define BUDDY_PACK_MAX_HEIGHT 120U
#define BUDDY_PACK_MIN_FRAME_MS 40U
#define BUDDY_PACK_MAX_HEADER \
    (BUDDY_PACK_FIXED_HEADER + BUDDY_PACK_MAX_CLIPS * BUDDY_PACK_CLIP_BYTES + 4U)

/* The first seven follow the upstream manifest "states" keys; version 2 adds the
 * care reactions and mood idles, version 3 the poop clean-up. POOP frames hold
 * the poop in their bottom-left quarter; the device draws only that quarter. */
typedef enum {
    BUDDY_PACK_SLEEP,
    BUDDY_PACK_IDLE,
    BUDDY_PACK_BUSY,
    BUDDY_PACK_ATTENTION,
    BUDDY_PACK_CELEBRATE,
    BUDDY_PACK_DIZZY,
    BUDDY_PACK_HEART,
    BUDDY_PACK_EAT,
    BUDDY_PACK_PLAY,
    BUDDY_PACK_PAT,
    BUDDY_PACK_REFUSE,
    BUDDY_PACK_HUNGRY,
    BUDDY_PACK_SAD,
    BUDDY_PACK_TIRED,
    BUDDY_PACK_CLEAN,
    BUDDY_PACK_STINKY,
    BUDDY_PACK_POOP,
    BUDDY_PACK_STATE_COUNT,
} buddy_pack_state_t;

typedef struct {
    uint16_t first_frame;
    uint16_t frame_count;
    uint16_t frame_ms;
} buddy_pack_clip_t;

typedef struct {
    uint16_t first_clip;
    uint16_t clip_count;
} buddy_pack_state_entry_t;

typedef struct {
    char name[BUDDY_PACK_NAME_MAX + 1U];
    uint16_t version;
    uint16_t header_size;
    uint16_t width;
    uint16_t height;
    uint16_t clip_count;
    uint16_t frame_count;
    uint32_t frame_bytes;
    uint32_t payload_size;
    uint32_t payload_crc;
    buddy_pack_state_entry_t states[BUDDY_PACK_STATE_COUNT];
    buddy_pack_clip_t clips[BUDDY_PACK_MAX_CLIPS];
} buddy_pack_info_t;

uint32_t buddy_pack_crc32(uint32_t crc, const uint8_t *data, size_t length);

/* Validates the header (including its CRC and every clip reference). The
 * payload CRC is checked separately because the payload may live in flash. */
bool buddy_pack_parse_header(const uint8_t *data, size_t length, buddy_pack_info_t *info);

/* Total bytes the pack occupies (header + payload). */
uint32_t buddy_pack_total_size(const buddy_pack_info_t *info);

/* The state whose clips play for a requested state. Empty states fall back:
 * eat, pat and clean to heart, play to celebrate, refuse to dizzy, the rest to
 * idle. Empty mood states (hungry, sad, tired, stinky) and poop have no
 * fallback and return false. */
bool buddy_pack_resolve_state(const buddy_pack_info_t *info, buddy_pack_state_t state,
                              buddy_pack_state_t *resolved);

/* Frame to show for a state at a point in time, after fallback. Several clips
 * in one state play one after another (the upstream idle carousel). Returns
 * false if there is nothing to show. */
bool buddy_pack_frame_at(const buddy_pack_info_t *info, buddy_pack_state_t state,
                         uint64_t elapsed_ms, uint16_t *frame_index);
