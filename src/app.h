/* The application: one call per frame from the platform backend (60 Hz). */
#pragma once
#include <stdbool.h>

bool app_init(void);        /* the file layer is mounted; false = fatal (already logged) */
bool app_frame(void);       /* false = quit */
void app_exit(void);
/* 44.1 kHz stereo float: the frame's share of the mix (called after app_frame). */
void app_audio(float *out, unsigned frames);
