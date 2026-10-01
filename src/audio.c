#include "audio.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool wav_decode(const uint8_t *d, size_t n, Wave *w)
{
    memset(w, 0, sizeof *w);
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    uint32_t fmt = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t sz = rd32(d + p + 4);
        const uint8_t *body = d + p + 8;
        if (sz > n - p - 8) sz = (uint32_t)(n - p - 8);
        if (!memcmp(d + p, "fmt ", 4) && sz >= 16) {
            fmt = rd16(body);
            ch = rd16(body + 2);
            rate = rd32(body + 4);
            bits = rd16(body + 14);
        } else if (!memcmp(d + p, "data", 4)) {
            data = body;
            dlen = sz;
        }
        p += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || !data || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || !rate) return false;
    uint32_t bpf = ch * bits / 8;
    w->frames = dlen / bpf;
    w->rate = rate;
    w->channels = ch;
    w->samples = malloc((size_t)w->frames * ch * sizeof *w->samples + 2);
    for (uint32_t i = 0; i < w->frames * ch; i++)
        w->samples[i] = bits == 16 ? (int16_t)rd16(data + i * 2) : (int16_t)((data[i] - 128) << 8);
    return true;
}

void wave_free(Wave *w)
{
    free(w->samples);
    memset(w, 0, sizeof *w);
}

void audio_mix(Voice *const *voices, uint32_t n, float *out, uint32_t frames)
{
    memset(out, 0, (size_t)frames * AUDIO_CHANNELS * sizeof *out);
    for (uint32_t v = 0; v < n; v++) {
        Voice *s = voices[v];
        if (!s || !s->playing || s->paused || !s->wave || !s->wave->frames) continue;
        const Wave *w = s->wave;
        if (s->gain <= 0) {
            /* silent, but time still passes */
            s->pos += (double)w->rate * s->pitch / AUDIO_RATE * frames;
            if (s->pos >= w->frames) {
                if (s->loop) s->pos = s->pos - (double)w->frames * (uint64_t)(s->pos / w->frames);
                else s->playing = false;
            }
            continue;
        }
        float l = s->gain, r = s->gain;
        if (s->pan > 0) l *= 1 - s->pan;
        else if (s->pan < 0) r *= 1 + s->pan;
        double step = (double)w->rate * (s->pitch > 0 ? s->pitch : 1) / AUDIO_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            if (s->pos >= w->frames) {
                if (!s->loop) {
                    s->playing = false;
                    break;
                }
                s->pos -= w->frames;
            }
            uint32_t k = (uint32_t)s->pos;
            float a, b;
            if (w->channels == 2) {
                a = w->samples[k * 2] / 32768.0f;
                b = w->samples[k * 2 + 1] / 32768.0f;
            } else {
                a = b = w->samples[k] / 32768.0f;
            }
            out[i * 2] += a * l;
            out[i * 2 + 1] += b * r;
            s->pos += step;
        }
    }
    for (uint32_t i = 0; i < frames * AUDIO_CHANNELS; i++) {
        if (out[i] > 1) out[i] = 1;
        else if (out[i] < -1) out[i] = -1;
    }
}
