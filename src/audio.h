/* Audio: WAV decoding and the mixer behind CKWaveSound. Output is 44.1 kHz stereo float, rendered per frame
   by the platform (render into its buffer, then push).

   Volume model = DX7SoundManager.dll's: DirectSound volume 2000*log10(gain) hundredths of dB (so the
   amplitude is the gain; gain <= 0 is silence), pan attenuates the other channel by (1 - |pan|)
   (FUN_24bc2949), pitch multiplies the playback frequency. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { AUDIO_RATE = 44100, AUDIO_CHANNELS = 2 };

typedef struct {
    int16_t *samples;         /* interleaved */
    uint32_t frames;
    uint32_t rate;
    uint32_t channels;        /* 1 or 2 */
} Wave;

bool wav_decode(const uint8_t *data, size_t len, Wave *out);   /* PCM 8/16-bit */
void wave_free(Wave *w);

typedef struct {
    const Wave *wave;
    double pos;               /* in source frames */
    float gain, pan, pitch;
    bool playing, paused, loop;
} Voice;

/* Mixes voices into out (frames * 2 floats, overwritten); voices that reach the end stop unless looping. */
void audio_mix(Voice *const *voices, uint32_t nvoices, float *out, uint32_t frames);
