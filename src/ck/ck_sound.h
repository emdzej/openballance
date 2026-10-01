/* CKWaveSound (CK2.dll 0x24018889-0x2401ae62): a WAV file played through the sound manager, with gain, pan,
   pitch, loop, pause and linear fades. Chunk: CKSound 0x1000 (dword, file name), 0x800000 (length),
   0x400000 (flags, priority, gain, pan, pitch, 3D settings...). Loaded with FUN_2401a255. */
#pragma once
#include "../audio.h"
#include "ck.h"

enum { CKWS_LOOP = 0x8, CKWS_FADEIN = 0x20, CKWS_FADEOUT = 0x40, CKWS_STREAMING = 0x2000 };

typedef struct {
    CkBeObject be;
    char *file;
    uint32_t flags;           /* +0x60 */
    float priority, gain, pan, pitch;
    float fade_duration, fade_elapsed;   /* +0x90, +0x94 */
    bool need_rewind;         /* +0x62 bit 0: stopped, rewind on the next Play */
    Wave wave;
    bool loaded, load_failed;
    Voice voice;
} CkWaveSound;

static inline CkWaveSound *ck_wavesound(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == 25 ? (CkWaveSound *)o : NULL;
}

void ck_sound_load_chunk(CkWaveSound *s, const CkChunk *c);
void ck_sound_free(CkWaveSound *s);

void ck_sound_play(CkContext *ctx, CkWaveSound *s, float fade_in, float gain);   /* CKWaveSound::Play */
void ck_sound_stop(CkContext *ctx, CkWaveSound *s, float fade_out);            /* CKWaveSound::Stop */
void ck_sound_pause(CkWaveSound *s);
void ck_sound_resume(CkWaveSound *s);
void ck_sound_rewind(CkWaveSound *s);
bool ck_sound_is_playing(const CkWaveSound *s);
bool ck_sound_set_file(CkWaveSound *s, const char *file);   /* the file, loaded now */
bool ck_sound_is_paused(const CkWaveSound *s);
void ck_sound_set_loop(CkWaveSound *s, bool loop);
void ck_sound_set_gain(CkWaveSound *s, float gain);                             /* stored and applied */
void ck_sound_set_pan(CkWaveSound *s, float pan);
void ck_sound_set_pitch(CkWaveSound *s, float pitch);
float ck_sound_length_ms(CkContext *ctx, CkWaveSound *s);

/* CKWaveSound::PlayMinion 0x240189c7: a one-shot instance of the sound at the given volume. */
void ck_sound_play_minion(CkContext *ctx, CkWaveSound *s, float volume);
/* Per frame (the sound manager's processing): fades advance by the frame time (CKWaveSound::UpdateFade). */
void ck_sound_update(CkContext *ctx);
/* Renders every playing sound into out (frames * 2 floats). */
void ck_sound_mix(CkContext *ctx, float *out, uint32_t frames);
