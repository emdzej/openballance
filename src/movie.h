/* AVI movies for movie textures (Textures/atari.avi): RIFF AVI with a Microsoft Video 1 ("CRAM") video
   stream, 16-bit RGB555 or 8-bit palettized. Frames are decoded in order (the codec codes changes against
   the previous frame); seeking backwards restarts from the first frame. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Movie Movie;

Movie *movie_open(const char *path);          /* through the file layer; NULL if unsupported */
void movie_close(Movie *m);
uint32_t movie_width(const Movie *m);
uint32_t movie_height(const Movie *m);
uint32_t movie_frames(const Movie *m);
float movie_fps(const Movie *m);
/* RGBA8 of frame i (rows top to bottom), valid until the next call; NULL on error. */
const uint8_t *movie_frame(Movie *m, uint32_t i);
