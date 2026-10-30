#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "buddy_i4.h"

int main(void)
{
    uint8_t guarded[18];
    uint8_t *pixels = guarded + 1;
    buddy_i4_surface_t surface;
    buddy_i4_clip_t clip = {.x = 2, .y = 1, .w = 4, .h = 2};
    unsigned x;
    unsigned y;
    memset(guarded, 0, sizeof(guarded));
    guarded[0] = guarded[17] = 0xa5;

    buddy_i4_set_pixel(pixels, 8, 0, 0, 3);
    buddy_i4_set_pixel(pixels, 8, 1, 0, 12);
    buddy_i4_set_pixel(pixels, 8, 7, 1, 15);

    assert(buddy_i4_get_pixel(pixels, 8, 0, 0) == 3);
    assert(buddy_i4_get_pixel(pixels, 8, 1, 0) == 12);
    assert(buddy_i4_get_pixel(pixels, 8, 7, 1) == 15);
    assert(buddy_i4_get_pixel(pixels, 8, 2, 0) == 0);

    memset(pixels, 0, 16);
    buddy_i4_surface_init(&surface, pixels, 8, 4, 4);
    buddy_i4_fill_rect(&surface, &clip, -4, -4, 20, 20, 7);
    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 8; ++x) {
            bool inside = x >= 2 && x < 6 && y >= 1 && y < 3;
            assert(buddy_i4_get_pixel(pixels, 8, x, y) == (inside ? 7 : 0));
        }
    }
    buddy_i4_line(&surface, &clip, -20, 2, 20, 2, 9);
    for (x = 2; x < 6; ++x) assert(buddy_i4_get_pixel(pixels, 8, x, 2) == 9);

    /* Blit: 4x2 image, index 0 transparent, clipped to the same rectangle. */
    {
        static const uint8_t image[] = {0x12, 0x03, 0x45, 0x60};
        memset(pixels, 0, 16);
        buddy_i4_fill_rect(&surface, NULL, 0, 0, 8, 4, 15);
        buddy_i4_blit(&surface, &clip, image, 4, 2, 1, 1);
        assert(buddy_i4_get_pixel(pixels, 8, 1, 1) == 15); /* left of clip */
        assert(buddy_i4_get_pixel(pixels, 8, 2, 1) == 2);
        assert(buddy_i4_get_pixel(pixels, 8, 3, 1) == 15); /* transparent */
        assert(buddy_i4_get_pixel(pixels, 8, 4, 1) == 3);
        assert(buddy_i4_get_pixel(pixels, 8, 2, 2) == 5);
        assert(buddy_i4_get_pixel(pixels, 8, 3, 2) == 6);
        assert(buddy_i4_get_pixel(pixels, 8, 4, 2) == 15); /* transparent */
    }
    assert(guarded[0] == 0xa5 && guarded[17] == 0xa5);
    return 0;
}
