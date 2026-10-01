/* Sounds.dll Building Blocks: playback and control of wave sounds (CKWaveSound, ck_sound.h). */
#include "bb.h"
#include "../ck/ck_sound.h"

/* The old layouts take the sound as pIn 0 (the callbacks switch the function when pIn 0 is a Wave Sound);
   the game's use the target. */
static CkWaveSound *sound_of(CkContext *ctx, CkBehavior *b, uint32_t *first)
{
    CkParameter *p0 = b->pin.n ? ck_param(ctx, b->pin.v[0]) : NULL;
    CkGuid ws = {0x4bf74e5eu, 0x45f409efu};
    if (p0 && ck_guid_eq(p0->type, ws)) {
        *first = 1;
        return ck_wavesound(ctx, bb_in_object(ctx, b, 0));
    }
    *first = 0;
    return ck_wavesound(ctx, bb_target(ctx, b));
}

/* ---- Wave Player 5bde0e45:2e2107d5 (FUN_257022a0): inputs Play, Stop, Pause/Resume; outputs Start
   Playing, End Playing, Paused/Resumed; pIn Fade In, Fade Out, Loop. Stays active while playing; a
   looping Play returns at once. ---- */
static int bb_wave_player(CkContext *ctx, CkBehavior *b)
{
    uint32_t k;
    CkWaveSound *s = sound_of(ctx, b, &k);
    if (!s) return CKBR_OK;
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        float fade_in = 0;
        int32_t loop = 0;
        bb_get_in(ctx, b, k + 0, &fade_in, 4);
        bb_get_in(ctx, b, k + 2, &loop, 4);
        ck_sound_set_loop(s, loop != 0);
        ck_sound_play(ctx, s, fade_in, s->gain);
        ck_activate_output(ctx, b, 0, true);
        return loop == 0 ? CKBR_ACTIVATENEXTFRAME : CKBR_OK;
    }
    if (b->in.n > 2 && ck_input_active(ctx, b, 2)) {
        ck_activate_input(ctx, b, 2, false);
        ck_activate_output(ctx, b, 2, true);
        if (ck_sound_is_playing(s)) {
            ck_sound_pause(s);
            return CKBR_ACTIVATENEXTFRAME;
        }
        if (ck_sound_is_paused(s)) {
            ck_sound_resume(s);
            return CKBR_OK;
        }
    }
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        float fade_out = 0;
        bb_get_in(ctx, b, k + 1, &fade_out, 4);
        ck_sound_stop(ctx, s, fade_out);
        if (fade_out == 0) {
            ck_activate_output(ctx, b, 1, true);
            return CKBR_OK;
        }
    } else if (!ck_sound_is_playing(s)) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Volume Control 33613f21:776b185b (FUN_25701c10) / Pitch Control 7b7c731e:175b27a3 (FUN_257017b0) /
   Panning Control 6a5f17af:63480864 (FUN_257019d0) ---- */
static int bb_sound_control(CkContext *ctx, CkBehavior *b, int what)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    uint32_t k;
    CkWaveSound *s = sound_of(ctx, b, &k);
    if (!s) return CKBR_OK;
    float v = what == 2 ? 0.0f : 1.0f;
    bb_get_in(ctx, b, k, &v, 4);
    if (what == 0) ck_sound_set_gain(s, v);
    else if (what == 1) ck_sound_set_pitch(s, v);
    else ck_sound_set_pan(s, v);
    return CKBR_OK;
}
static int bb_volume_control(CkContext *ctx, CkBehavior *b) { return bb_sound_control(ctx, b, 0); }
static int bb_pitch_control(CkContext *ctx, CkBehavior *b) { return bb_sound_control(ctx, b, 1); }
static int bb_panning_control(CkContext *ctx, CkBehavior *b) { return bb_sound_control(ctx, b, 2); }

/* ---- Play Sound Instance 00283a35:38ef6b48 (FUN_25701e20): pIn 2D, Object, Position, Direction, Min Delay,
   Volume. A minion of the target sound; 3D placement is not modelled yet (played 2D). ---- */
static int bb_play_sound_instance(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkWaveSound *s = ck_wavesound(ctx, bb_target(ctx, b));
    if (!s) return CKBR_OK;
    float volume = 1;
    bb_get_in(ctx, b, 5, &volume, 4);
    ck_sound_play_minion(ctx, s, volume);
    return CKBR_OK;
}

/* ---- TT_GetSoundProperties 30fa6a70:3b3f728a (TT_Gravity_RT FUN_10002f20): Gain, Pitch, Pan of the
   target sound ---- */
static int bb_tt_get_sound_properties(CkContext *ctx, CkBehavior *b)
{
    CkObj *o = ck_obj(ctx, bb_target(ctx, b));
    if (o && o->cid == 25) {
        CkWaveSound *s = (CkWaveSound *)o;
        bb_set_out(ctx, b, 0, &s->gain, 4);
        bb_set_out(ctx, b, 1, &s->pitch, 4);
        bb_set_out(ctx, b, 2, &s->pan, 4);
    }
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

BB_DECL(d_wave_player, 5bde0e45, 2e2107d5, "Wave Player", bb_wave_player);
BB_DECL(d_volume_control, 33613f21, 776b185b, "Volume Control", bb_volume_control);
BB_DECL(d_pitch_control, 7b7c731e, 175b27a3, "Pitch Control", bb_pitch_control);
BB_DECL(d_panning_control, 6a5f17af, 63480864, "Panning Control", bb_panning_control);
BB_DECL(d_play_sound_instance, 00283a35, 38ef6b48, "Play Sound Instance", bb_play_sound_instance);
BB_DECL(d_tt_get_sound_properties, 30fa6a70, 3b3f728a, "TT_GetSoundProperties", bb_tt_get_sound_properties);

const CkBBDecl *const bb_sounds[] = {&d_wave_player, &d_volume_control, &d_pitch_control, &d_panning_control,
                                     &d_play_sound_instance, &d_tt_get_sound_properties};
const unsigned bb_sounds_count = sizeof bb_sounds / sizeof *bb_sounds;
