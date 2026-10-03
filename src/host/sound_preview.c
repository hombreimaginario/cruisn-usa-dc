/*
 * sound_preview.c - Escucha en el ordenador lo que sonaria en Dreamcast.
 *
 * Usa el mismo decodificador del protocolo DCS que la consola
 * (src/sound/dcs_proto.c) con un mezclador sencillo: 5 voces (4 pistas y el
 * motor) y la musica, a 44100 Hz mono, una porcion por cada interrupcion de
 * video. El resultado se escribe en un WAV (CUSA_SOUNDWAV en cusa_recomp).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sound/dcs_proto.h"
#include "sound_preview.h"

#define OUT_RATE 44100
#define NVOICES (DCS_NTRACKS + 1)

static uint8_t *bank;
static size_t bank_size;
static float *pcm[DCS_MAX_ENTRIES];     /* muestras decodificadas por entrada */
static uint32_t pcm_len[DCS_MAX_ENTRIES];

typedef struct {
    int entry;                          /* -1 = libre */
    double pos, step;
    float vol;
} voice;

static voice voices[NVOICES], music = { -1, 0, 0, 0 };
static FILE *out;
static uint32_t out_samples;
static double frame_acc;

/* Mismo modelo de ADPCM que el codificador (tools/dcsdsp.c) */
static float *decode_adpcm(const uint8_t *d, uint32_t bytes, uint32_t *n)
{
    static const int step_table[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    float *o = malloc(bytes * 2 * sizeof(float));
    int hist = 0, ss = 127;
    uint32_t i;
    for (i = 0; i < bytes * 2; i++) {
        int s = (d[i >> 1] >> ((i & 1) * 4)) & 15, delta = s & 7;
        int diff = ((1 + (delta << 1)) * ss) >> 3;
        int nss = (step_table[delta] * ss) >> 8;
        if (diff > 32767) diff = 32767;
        hist += (s & 8) ? -diff : diff;
        if (hist > 32767) hist = 32767;
        if (hist < -32768) hist = -32768;
        ss = nss < 127 ? 127 : nss > 24576 ? 24576 : nss;
        o[i] = hist / 32768.0f;
    }
    *n = bytes * 2;
    return o;
}

static float *entry_pcm(int idx)
{
    const snd_entry *e = &dcs_entries[idx];
    if (!pcm[idx] && e->offset + e->bytes <= bank_size) {
        if (e->flags & SF_PCM16) {
            uint32_t i, n = e->bytes / 2;
            const int16_t *s = (const int16_t *)(bank + e->offset);
            pcm[idx] = malloc(n * sizeof(float));
            for (i = 0; i < n; i++)
                pcm[idx][i] = s[i] / 32768.0f;
            pcm_len[idx] = n;
        } else {
            pcm[idx] = decode_adpcm(bank + e->offset, e->bytes, &pcm_len[idx]);
        }
    }
    return pcm[idx];
}

static void start(voice *v, int idx, int vol, int freq)
{
    if (!entry_pcm(idx)) {
        v->entry = -1;
        return;
    }
    v->entry = idx;
    v->pos = 0;
    v->step = (double)freq / OUT_RATE;
    v->vol = vol / 255.0f;
}

static void be_play(int voice_n, int idx, int vol, int freq)
{
    start(&voices[voice_n], idx, vol, freq);
}

static void be_update(int voice_n, int vol, int freq)
{
    voices[voice_n].vol = vol / 255.0f;
    voices[voice_n].step = (double)freq / OUT_RATE;
}

static void be_stop(int voice_n)
{
    voices[voice_n].entry = -1;
}

static void be_music(int idx, int vol)
{
    if (idx < 0)
        music.entry = -1;
    else
        start(&music, idx, vol, (int)dcs_entries[idx].rate);
}

static void be_music_volume(int vol)
{
    music.vol = vol / 255.0f;
}

static const dcs_backend preview_backend = {
    be_play, be_update, be_stop, be_music, be_music_volume
};

static float next_sample(voice *v)
{
    const snd_entry *e;
    uint32_t i, len, end;
    float a, b, f;

    if (v->entry < 0)
        return 0;
    e = &dcs_entries[v->entry];
    len = pcm_len[v->entry];
    end = (e->flags & SF_LOOP) && e->loopend ? e->loopend : len;
    if (end > len)
        end = len;
    if (v->pos >= end) {
        if (!(e->flags & SF_LOOP) || end <= e->loopstart) {
            v->entry = -1;
            return 0;
        }
        v->pos = e->loopstart + fmod(v->pos - end, end - e->loopstart);
    }
    i = (uint32_t)v->pos;
    f = (float)(v->pos - i);
    a = pcm[v->entry][i];
    b = i + 1 < end ? pcm[v->entry][i + 1] : a;
    v->pos += v->step;
    return (a + (b - a) * f) * v->vol;
}

static void put_header(void)
{
    uint32_t data = out_samples * 2, riff = 36 + data, rate = OUT_RATE, brate = OUT_RATE * 2;
    uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    uint32_t sixteen = 16;
    fseek(out, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, out); fwrite(&riff, 4, 1, out); fwrite("WAVEfmt ", 1, 8, out);
    fwrite(&sixteen, 4, 1, out); fwrite(&fmt, 2, 1, out); fwrite(&ch, 2, 1, out);
    fwrite(&rate, 4, 1, out); fwrite(&brate, 4, 1, out); fwrite(&align, 2, 1, out);
    fwrite(&bits, 2, 1, out); fwrite("data", 1, 4, out); fwrite(&data, 4, 1, out);
    fseek(out, 0, SEEK_END);
}

int sound_preview_init(const char *bank_path, const char *wav_path)
{
    FILE *f = fopen(bank_path, "rb");
    int i;

    if (!f) {
        fprintf(stderr, "sonido: falta %s\n", bank_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    bank_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    bank = malloc(bank_size);
    if (fread(bank, 1, bank_size, f) != bank_size || (dcs_nentries = dcs_parse_header(bank)) < 0) {
        fclose(f);
        fprintf(stderr, "sonido: %s no es un banco valido\n", bank_path);
        return -1;
    }
    fclose(f);
    memcpy(dcs_entries, bank + 16, dcs_nentries * sizeof(snd_entry));
    for (i = 0; i < NVOICES; i++)
        voices[i].entry = -1;
    out = fopen(wav_path, "wb");
    if (!out)
        return -1;
    put_header();
    dcs_proto_init(&preview_backend, NULL);
    return 0;
}

void sound_preview_byte(uint32_t v)
{
    dcs_proto_write(v);
}

void sound_preview_frame(void)
{
    int n, k, j;
    if (!out)
        return;
    frame_acc += OUT_RATE / 57.0;
    n = (int)frame_acc;
    frame_acc -= n;
    for (k = 0; k < n; k++) {
        float s = next_sample(&music);
        int16_t o;
        for (j = 0; j < NVOICES; j++)
            s += next_sample(&voices[j]);
        if (s > 1) s = 1;
        if (s < -1) s = -1;
        o = (int16_t)(s * 32767);
        fwrite(&o, 2, 1, out);
    }
    out_samples += n;
}

void sound_preview_close(void)
{
    if (out) {
        put_header();
        fclose(out);
        out = NULL;
    }
}
