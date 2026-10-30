#pragma once

/*
 * Uploaded character pack: flash storage in the "buddy" data partition and
 * frame lookup for the UI. All functions run on the app task, which also
 * renders the UI, so uploads and drawing never overlap.
 */

#include <stdbool.h>
#include <stdint.h>

#include "buddy_i4.h"
#include "buddy_pack_format.h"
#include "buddy_pack_xfer.h"

/* Finds the partition and maps a stored pack if it verifies. */
bool buddy_pack_init(void);
bool buddy_pack_available(void);
const char *buddy_pack_name(void);

bool buddy_pack_transfer(const buddy_transfer_t *transfer, uint32_t *n, const char **error);
void buddy_pack_abort(void);
bool buddy_pack_delete(void);

/* Frame to show for a state at elapsed_ms; false if no pack is mapped. */
bool buddy_pack_frame(buddy_pack_state_t state, uint64_t elapsed_ms, uint16_t *frame);
void buddy_pack_size(uint16_t *width, uint16_t *height);
void buddy_pack_draw(buddy_i4_surface_t *surface, const buddy_i4_clip_t *clip, uint16_t frame,
                     int x, int y);
