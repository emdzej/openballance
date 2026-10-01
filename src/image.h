/* Image decoders for the game's textures: BMP (8/24/32-bit, uncompressed) and TGA (true-colour,
   uncompressed or RLE). Output is RGBA8, rows top to bottom. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t w, h;
    uint8_t *rgba;            /* malloc'd, w*h*4 */
} Image;

bool image_decode(const uint8_t *data, size_t len, Image *out);   /* BMP or TGA by content */
bool image_load(const char *path, Image *out);                    /* through the file layer */
void image_free(Image *img);
/* Next mip level (box filter, 2x2 -> 1; odd sizes clamp). Returns false at 1x1. */
bool image_downsample(const Image *src, Image *dst);
