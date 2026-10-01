#include "ck_sound.h"
#include "../vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ck_sound_load_chunk(CkWaveSound *s, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    s->gain = 1, s->pitch = 1;
    if (ck_seek(&r, 0x1000)) {             /* CKSound (FUN_240187dc): dword, file name */
        ck_read_dword(&r);
        s->file = strdup(ck_read_string(&r));
    }
    if (ck_seek(&r, 0x100000)) {
        free(s->file);
        s->file = strdup(ck_read_string(&r));
    }
    if (ck_seek(&r, 0x400000) && c->data_version >= 3) {
        s->flags = ck_read_dword(&r);
        s->priority = ck_read_float(&r);
        s->gain = ck_read_float(&r);
        s->pan = ck_read_float(&r);
        s->pitch = ck_read_float(&r);
        /* 3D settings (cone, distances, attached entity, position, direction) follow */
    }
    s->voice.gain = s->gain;
    s->voice.pan = s->pan;
    s->voice.pitch = s->pitch;
    s->voice.loop = (s->flags & CKWS_LOOP) != 0;
}

void ck_sound_free(CkWaveSound *s)
{
    free(s->file);
    wave_free(&s->wave);
}

/* The file through the path manager: as named, then in the sound folders */
static void ensure_loaded(CkWaveSound *s)
{
    if (s->loaded || s->load_failed || !s->file || !*s->file) return;
    static const char *dirs[] = {"", "Sounds/", "Sounds_low/"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s%s", dirs[i], s->file);
        size_t n;
        uint8_t *d = vfs_read_all(path, &n);
        if (!d) continue;
        bool ok = wav_decode(d, n, &s->wave);
        free(d);
        if (ok) {
            s->loaded = true;
            s->voice.wave = &s->wave;
            return;
        }
    }
    s->load_failed = true;
}

void ck_sound_rewind(CkWaveSound *s) { s->voice.pos = 0; }

/* CKWaveSound::SetSoundFileName + a (synchronous) load: false if the file can't be decoded */
bool ck_sound_set_file(CkWaveSound *s, const char *file)
{
    s->voice.playing = false;
    s->voice.wave = NULL;
    wave_free(&s->wave);
    free(s->file);
    s->file = strdup(file ? file : "");
    s->loaded = s->load_failed = false;
    s->need_rewind = false;
    s->voice.pos = 0;
    ensure_loaded(s);
    return s->loaded;
}

/* CKWaveSound::Play 0x24018fae */
void ck_sound_play(CkContext *ctx, CkWaveSound *s, float fade_in, float gain)
{
    ensure_loaded(s);
    if (!s->loaded) return;
    s->flags &= ~(uint32_t)(CKWS_FADEIN | CKWS_FADEOUT);
    s->fade_elapsed = 0;
    s->fade_duration = fade_in;
    if (fade_in > 0) {
        s->flags |= CKWS_FADEIN;
        s->gain = gain;
        s->voice.gain = 0;
    }
    if (s->need_rewind) ck_sound_rewind(s);
    s->voice.paused = false;
    s->need_rewind = false;
    s->voice.loop = (s->flags & CKWS_LOOP) != 0;
    s->voice.playing = true;
}

/* CKWaveSound::InternalStop 0x24019ec8 */
static void internal_stop(CkWaveSound *s)
{
    s->voice.playing = false;
    s->need_rewind = true;
}

/* CKWaveSound::Stop 0x24019281 */
void ck_sound_stop(CkContext *ctx, CkWaveSound *s, float fade_out)
{
    if (!s->loaded) return;
    if (fade_out == 0) internal_stop(s);
    if (!(s->flags & CKWS_FADEIN)) {
        s->fade_duration = fade_out;
        s->fade_elapsed = 0;
    } else {                                /* fading in: continue as a proportional fade out */
        float e = s->fade_elapsed;
        s->flags &= ~(uint32_t)CKWS_FADEIN;
        s->fade_elapsed = ((s->fade_duration - s->fade_elapsed) / s->fade_duration) * (fade_out + e);
        s->fade_duration = fade_out + e;
    }
    if (s->fade_duration > 0) s->flags |= CKWS_FADEOUT;
    s->voice.paused = false;
}

void ck_sound_pause(CkWaveSound *s)
{
    s->voice.paused = true;
}

void ck_sound_resume(CkWaveSound *s)
{
    s->voice.paused = false;
}

bool ck_sound_is_playing(const CkWaveSound *s) { return s->voice.playing && !s->voice.paused; }
bool ck_sound_is_paused(const CkWaveSound *s) { return s->voice.playing && s->voice.paused; }

void ck_sound_set_loop(CkWaveSound *s, bool loop)
{
    if (loop) s->flags |= CKWS_LOOP;
    else s->flags &= ~(uint32_t)CKWS_LOOP;
    s->voice.loop = loop;
}

void ck_sound_set_gain(CkWaveSound *s, float gain)
{
    s->gain = gain;
    s->voice.gain = gain;
}

void ck_sound_set_pan(CkWaveSound *s, float pan) { s->pan = s->voice.pan = pan; }
void ck_sound_set_pitch(CkWaveSound *s, float pitch) { s->pitch = s->voice.pitch = pitch; }

float ck_sound_length_ms(CkContext *ctx, CkWaveSound *s)
{
    ensure_loaded(s);
    return s->loaded && s->wave.rate ? (float)s->wave.frames * 1000.0f / (float)s->wave.rate : 0;
}

/* CKWaveSound::UpdateFade 0x24019faa, for every sound */
void ck_sound_update(CkContext *ctx)
{
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *o = ctx->objs[i];
        if (!o || o->cid != 25) continue;
        CkWaveSound *s = (CkWaveSound *)o;
        if (!(s->flags & (CKWS_FADEIN | CKWS_FADEOUT))) continue;
        s->fade_elapsed += ctx->delta_ms;
        if (s->fade_elapsed > s->fade_duration) s->fade_elapsed = s->fade_duration;
        float v = (s->flags & CKWS_FADEIN) ? s->gain / s->fade_duration * s->fade_elapsed
                                           : (1.0f - s->fade_elapsed / s->fade_duration) * s->gain;
        s->voice.gain = v;
        if (s->fade_elapsed == s->fade_duration) {
            if (s->flags & CKWS_FADEOUT) internal_stop(s);
            s->flags &= ~(uint32_t)(CKWS_FADEIN | CKWS_FADEOUT);
        }
    }
}

enum { MAX_MINIONS = 32 };

void ck_sound_play_minion(CkContext *ctx, CkWaveSound *s, float volume)
{
    ensure_loaded(s);
    if (!s->loaded) return;
    if (!ctx->minions) ctx->minions = calloc(MAX_MINIONS, sizeof(Voice));
    Voice *m = ctx->minions;
    uint32_t i = 0;
    while (i < MAX_MINIONS && m[i].playing) i++;
    if (i == MAX_MINIONS) return;          /* all busy: the original also drops it (CKSoundManager::CreateMinion) */
    m[i] = s->voice;
    m[i].pos = 0;
    m[i].gain = volume;
    m[i].loop = false;
    m[i].paused = false;
    m[i].playing = true;
}

void ck_sound_mix(CkContext *ctx, float *out, uint32_t frames)
{
    Voice *voices[256 + MAX_MINIONS];
    uint32_t n = 0;
    for (uint32_t i = 0; ctx->minions && i < MAX_MINIONS; i++)
        if (((Voice *)ctx->minions)[i].playing) voices[n++] = &((Voice *)ctx->minions)[i];
    for (uint32_t i = 0; i < ctx->nobjs && n < 256; i++) {
        CkObj *o = ctx->objs[i];
        if (o && o->cid == 25 && ((CkWaveSound *)o)->voice.playing) voices[n++] = &((CkWaveSound *)o)->voice;
    }
    audio_mix(voices, n, out, frames);
}
