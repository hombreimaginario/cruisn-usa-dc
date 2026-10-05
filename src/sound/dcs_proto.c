/*
 * dcs_proto.c - Decodificador del protocolo DCS (ver dcs_proto.h).
 *
 *   55AA vv ~vv      volumen general
 *   55AB+n vv ~vv    volumen de la pista n; SNDFX lo manda justo antes del codigo
 *   55CC rr vv       motor del jugador: revoluciones y volumen
 *   0000             parar todo menos el motor
 *   03E3-03E6        parar la pista 0-3 (KILLCHAN0-3)
 *   otro             sonido del banco en su pista (un sonido nuevo corta el
 *                    anterior de la misma pista; la musica se reinicia)
 */
#include <stdlib.h>
#include <string.h>

#include "dcs_proto.h"

#define MAX_ENGINE 8

snd_entry dcs_entries[DCS_MAX_ENTRIES];
int dcs_nentries;

static const dcs_backend *be;
static int16_t by_code[0x400];          /* codigo -> entrada, -1 si no hay */
static int engine_idx[MAX_ENGINE], nengine;
static int ready;

static int skip_byte;                   /* tras el reset se descarta un byte */
static int hi = -1;                     /* byte alto pendiente */
static int expect;                      /* 0, 1 = volumen, 2 = motor */
static int vol_target;                  /* -1 general, 0-3 pista */
static int pending_track = -1;          /* pista cuyo volumen se acaba de fijar */
static int master_vol = 255;
static int track_vol[DCS_NTRACKS] = { 255, 255, 255, 255 };
static int engine_cur = -1;             /* entrada sonando en la voz del motor */
static int engine_speed, engine_vol;
static int music_on;

int dcs_parse_header(const uint8_t *hdr)
{
    int32_t n;
    if (memcmp(hdr, "CUSASND1", 8))
        return -1;
    memcpy(&n, hdr + 8, 4);
    if (n < 0)
        return -1;
    return n > DCS_MAX_ENTRIES ? DCS_MAX_ENTRIES : n;
}

void dcs_proto_init(const dcs_backend *backend, int (*usable)(int entry))
{
    int i;
    be = backend;
    memset(by_code, 0xFF, sizeof(by_code));
    nengine = 0;
    for (i = 0; i < dcs_nentries; i++) {
        const snd_entry *e = &dcs_entries[i];
        if (usable && !(e->flags & SF_MUSIC) && !usable(i))
            continue;
        if (e->flags & SF_ENGINE) {
            if (nengine < MAX_ENGINE)
                engine_idx[nengine++] = i;
        } else if (e->code < sizeof(by_code) / sizeof(by_code[0])) {
            by_code[e->code] = (int16_t)i;
        }
    }
    ready = 1;
}

static int scale_vol(int v, int track)
{
    v = v * master_vol / 255;
    if (track >= 0)
        v = v * track_vol[track] / 255;
    return v;
}

static void track_stop(int track)
{
    if (track == 0 && music_on) {
        be->music(-1, 0);
        music_on = 0;
    }
    be->stop(track);
}

void (*dcs_code_hook)(int code);

static void play_code(int code)
{
    int idx, track, vol;
    const snd_entry *e;

    if (dcs_code_hook)
        dcs_code_hook(code);
    if (code == 0) {
        int t;
        for (t = 0; t < DCS_NTRACKS; t++)
            track_stop(t);
        return;
    }
    if (code >= 995 && code <= 998) {
        track_stop(code - 995);
        return;
    }
    if (code >= (int)(sizeof(by_code) / sizeof(by_code[0])) || (idx = by_code[code]) < 0)
        return;
    e = &dcs_entries[idx];
    track = pending_track >= 0 ? pending_track : e->track;
    vol = scale_vol(255, track);
    if (e->flags & SF_MUSIC) {
        be->stop(0);
        be->music(idx, vol);
        music_on = 1;
        return;
    }
    if (track == 0 && music_on) {
        be->music(-1, 0);
        music_on = 0;
    }
    be->play(track, idx, vol, (int)e->rate);
}

/* El tono del motor sube casi en linea recta con las revoluciones (medido en
 * MAME: f0 ~ 0.14 * rpm + 4.4 Hz). Se elige la muestra mas cercana y se
 * ajusta la frecuencia de reproduccion. */
static void engine_update(void)
{
    int i, best = -1, bestd = 1 << 30, freq, vol;
    const snd_entry *e;

    if (!nengine)
        return;
    vol = scale_vol(engine_vol, -1);
    if (vol <= 0) {
        if (engine_cur >= 0)
            be->stop(DCS_VOICE_ENGINE);
        engine_cur = -1;
        return;
    }
    for (i = 0; i < nengine; i++) {
        int d = abs((int)dcs_entries[engine_idx[i]].param - engine_speed);
        if (d < bestd) {
            bestd = d;
            best = engine_idx[i];
        }
    }
    e = &dcs_entries[best];
    freq = (int)(e->rate * (0.14f * engine_speed + 4.4f) / (0.14f * e->param + 4.4f));
    if (freq < 1000)
        freq = 1000;
    if (freq > 88000)
        freq = 88000;
    if (best != engine_cur) {
        be->play(DCS_VOICE_ENGINE, best, vol, freq);
        engine_cur = best;
    } else {
        be->update(DCS_VOICE_ENGINE, vol, freq);
    }
}

static void word(int w)
{
    if (expect == 1) {
        int v = (w >> 8) & 0xFF;
        expect = 0;
        if (((w ^ (w >> 8)) & 0xFF) != 0xFF)
            return;                      /* comprobacion vv ~vv fallida */
        if (vol_target < 0) {
            master_vol = v;
        } else {
            track_vol[vol_target] = v;
            pending_track = vol_target;
        }
        if (music_on && vol_target <= 0)
            be->music_volume(scale_vol(255, 0));
        return;
    }
    if (expect == 2) {
        expect = 0;
        engine_speed = (w >> 8) & 0xFF;
        engine_vol = w & 0xFF;
        engine_update();
        return;
    }
    if (w >= 0x55AA && w <= 0x55AE) {
        expect = 1;
        vol_target = w == 0x55AA ? -1 : w - 0x55AB;
        return;
    }
    if (w == 0x55CC) {
        expect = 2;
        return;
    }
    play_code(w);
    pending_track = -1;
}

void dcs_proto_write(uint32_t v)
{
    if (!ready)
        return;
    if (v & 0x100) {                     /* linea de reset del DCS */
        if (v & 1) {
            skip_byte = 1;
            hi = -1;
            expect = 0;
        } else {
            int t;
            for (t = 0; t < DCS_NTRACKS; t++)
                track_stop(t);
            if (engine_cur >= 0)
                be->stop(DCS_VOICE_ENGINE);
            engine_cur = -1;
        }
        return;
    }
    if (skip_byte) {
        skip_byte = 0;
        return;
    }
    if (hi < 0) {
        hi = v & 0xFF;
        return;
    }
    word((hi << 8) | (v & 0xFF));
    hi = -1;
}
