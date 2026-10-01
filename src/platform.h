/* Platform contract: what the portable core needs from a host (backends: platform_gasm.c). The GPU is
   render/gpu.h, the game data the file layer (vfs.h). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Virtual pad buttons, the gasm ABI layout (SNES positions: A east, B south, X north, Y west). */
enum {
    PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_X = 1 << 2, PAD_Y = 1 << 3, PAD_L = 1 << 4, PAD_R = 1 << 5,
    PAD_SELECT = 1 << 6, PAD_START = 1 << 7, PAD_UP = 1 << 8, PAD_DOWN = 1 << 9, PAD_LEFT = 1 << 10,
    PAD_RIGHT = 1 << 11,
};

void plat_log(const char *msg);
uint32_t plat_pad(int player);          /* stable within a frame */
/* UTF-8 text typed since the previous frame (backspace \b, enter \n); length, or -1 without keyboard */
int plat_text_input(char *dst, size_t cap);
/* Launch parameter (gasm --param / URL query); false if unset. */
bool plat_param(const char *name, char *dst, size_t cap);
/* Persistent user files (saves). */
uint8_t *plat_load_user_file(const char *name, size_t *size);
bool plat_save_user_file(const char *name, const void *data, size_t size);
