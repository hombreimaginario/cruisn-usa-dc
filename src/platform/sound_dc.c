/*
 * sound_dc.c - Sonido en Dreamcast a partir del protocolo de la placa DCS.
 *
 * El codigo del arcade sigue mandando bytes al puerto 0x9A0000 como si la
 * placa DCS estuviera ahi (docs/SONIDO.md). Aqui se decodifican esos comandos
 * y se reproducen con el AICA usando el banco pre-renderizado con MAME
 * (tools/mame/render_dcs.lua + tools/dcs_bank.py -> /cd/sound.bin):
 *
 *   - efectos y voces: ADPCM en la RAM del AICA, un canal por pista del DCS
 *     (un sonido nuevo en una pista corta el anterior, como en la placa);
 *   - musica: ADPCM leido del CD por un hilo y reproducido como stream;
 *   - motor: muestras en bucle a varias revoluciones; 55CC cambia el tono y
 *     el volumen sobre la marcha.
 */
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <dc/sound/aica_comm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sound_dc.h"

/* ---- banco ---- */

#define SF_LOOP   1
#define SF_PCM16  2
#define SF_MUSIC  4
#define SF_ENGINE 8

typedef struct {
    uint16_t code;
    uint8_t track;
    uint8_t flags;
    uint32_t rate;
    uint32_t offset;
    uint32_t bytes;
    uint32_t loopstart;     /* muestras */
    uint32_t loopend;       /* muestras; 0 = hasta el final */
    uint16_t param;         /* motor: revoluciones de la muestra */
    uint16_t pad;
    uint32_t pad2;
} snd_entry;

#define MAX_ENTRIES 320
#define MAX_ENGINE  8
#define NTRACKS     4

static snd_entry entries[MAX_ENTRIES];
static sfxhnd_t handles[MAX_ENTRIES];
static int nentries;
static int16_t by_code[0x400];          /* codigo -> entrada, -1 si no hay */
static int engine_idx[MAX_ENGINE], nengine;
static file_t bank_file = FILEHND_INVALID;
static int ready;

/* ---- estado del protocolo ---- */

static int skip_byte;                   /* tras el reset se descarta un byte */
static int hi = -1;                     /* byte alto pendiente */
static int expect;                      /* 0, 1 = volumen, 2 = motor */
static int vol_target;                  /* -1 general, 0-3 pista */
static int pending_track = -1;          /* pista cuyo volumen se acaba de fijar */
static int master_vol = 255;
static int track_vol[NTRACKS] = { 255, 255, 255, 255 };

/* ---- canales del AICA ---- */

static int track_chn[NTRACKS];
static int engine_chn = -1;
static int engine_cur = -1;             /* entrada sonando en el canal del motor */
static int engine_speed, engine_vol;

static void chn_update(int chn, int freq, int vol)
{
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = chn;
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ | AICA_CH_UPDATE_SET_VOL;
    chan->freq = freq;
    chan->vol = vol;
    snd_sh4_to_aica(tmp, cmd->size);
}

static int scale_vol(int v, int track)
{
    v = v * master_vol / 255;
    if (track >= 0)
        v = v * track_vol[track] / 255;
    return v;
}

/* ---- musica en streaming ---- */

#define MUS_HALF   (64 * 1024)

static snd_stream_hnd_t mus_hnd = SND_STREAM_INVALID;
static uint8_t mus_buf[2][MUS_HALF] __attribute__((aligned(32)));
static volatile int mus_len[2];         /* bytes validos de cada mitad */
static volatile int mus_pos;            /* lectura en la mitad actual */
static volatile int mus_cur;            /* mitad que se esta reproduciendo */
static volatile int mus_entry = -1;
static volatile uint32_t mus_file_pos;  /* siguiente byte a leer (relativo) */
static semaphore_t mus_sem;
static mutex_t mus_lock = MUTEX_INITIALIZER;
static int mus_playing;
static volatile int mus_start_req;      /* 1 = rellenar las dos mitades, 2 = listas */
static int mus_start_vol;
/* Depuracion: mitades leidas. No usar printf desde el hilo lector: en
 * Flycast deja colgado el juego en cuanto arranca la musica. */
static volatile int mus_fills;

static void mus_fill(int half)
{
    const snd_entry *e;
    uint32_t total, loop_b;
    int got = 0;

    mutex_lock(&mus_lock);
    if (mus_entry < 0) {
        mus_len[half] = 0;
        mutex_unlock(&mus_lock);
        return;
    }
    e = &entries[mus_entry];
    total = e->bytes;
    loop_b = e->loopstart / 2;
    while (got < MUS_HALF) {
        uint32_t n = total - mus_file_pos;
        if (n == 0) {
            if (!(e->flags & SF_LOOP))
                break;
            mus_file_pos = loop_b;
            continue;
        }
        if (n > (uint32_t)(MUS_HALF - got))
            n = MUS_HALF - got;
        fs_seek(bank_file, e->offset + mus_file_pos, SEEK_SET);
        if (fs_read(bank_file, mus_buf[half] + got, n) != (ssize_t)n)
            break;
        got += n;
        mus_file_pos += n;
    }
    mus_len[half] = got;
    mutex_unlock(&mus_lock);
    mus_fills++;
}

static void *mus_reader(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait(&mus_sem);
        if (mus_start_req == 1) {
            mus_fill(0);
            mus_fill(1);
            mus_start_req = 2;
        } else {
            mus_fill(mus_cur ^ 1);
        }
    }
    return NULL;
}

static void *mus_callback(snd_stream_hnd_t hnd, int req, int *recv)
{
    int avail;
    (void)hnd;

    avail = mus_len[mus_cur] - mus_pos;
    if (avail <= 0) {
        /* Mitad agotada: pasar a la otra y pedir que se rellene esta */
        if (mus_len[mus_cur ^ 1] <= 0) {
            *recv = 0;
            return NULL;
        }
        mus_cur ^= 1;
        mus_pos = 0;
        sem_signal(&mus_sem);
        avail = mus_len[mus_cur];
    }
    if (req > avail)
        req = avail;
    req &= ~31;
    if (req <= 0) {
        /* resto de menos de 32 bytes: se descarta */
        mus_pos = mus_len[mus_cur];
        *recv = 0;
        return NULL;
    }
    *recv = req;
    mus_pos += req;
    return mus_buf[mus_cur] + mus_pos - req;
}

static void music_stop(void)
{
    mus_start_req = 0;
    if (mus_playing) {
        snd_stream_stop(mus_hnd);
        mus_playing = 0;
    }
    mutex_lock(&mus_lock);
    mus_entry = -1;
    mutex_unlock(&mus_lock);
}

static void music_play(int idx, int vol)
{
    music_stop();
    mutex_lock(&mus_lock);
    mus_entry = idx;
    mus_file_pos = 0;
    mutex_unlock(&mus_lock);
    mus_cur = 0;
    mus_pos = 0;
    mus_len[0] = mus_len[1] = 0;
    /* La lectura del CD la hace el hilo para no parar el juego; el stream
     * arranca en sound_frame() cuando las dos mitades estan llenas. */
    mus_start_vol = vol;
    mus_start_req = 1;
    sem_signal(&mus_sem);
}

/* ---- reproduccion ---- */

static void track_stop(int track)
{
    if (track == 0)
        music_stop();
    snd_sfx_stop(track_chn[track]);
}

static void play_code(int code)
{
    int idx, track, vol;
    const snd_entry *e;

    if (code == 0) {                     /* parar todo (el motor sigue) */
        int t;
        for (t = 0; t < NTRACKS; t++)
            track_stop(t);
        return;
    }
    if (code >= 995 && code <= 998) {    /* KILLCHAN0-3 */
        track_stop(code - 995);
        return;
    }
    if (code >= (int)(sizeof(by_code) / sizeof(by_code[0])) || (idx = by_code[code]) < 0)
        return;
    e = &entries[idx];
    track = pending_track >= 0 ? pending_track : e->track;
    vol = scale_vol(255, track);
#ifdef CUSA_SNDLOG
    printf("snd %04X pista %d vol %d%s\n", code, track, vol, (e->flags & SF_MUSIC) ? " musica" : "");
#endif
    if (e->flags & SF_MUSIC) {
        snd_sfx_stop(track_chn[0]);
        music_play(idx, vol);
        return;
    }
    if (track == 0)
        music_stop();
    if (handles[idx] == SFXHND_INVALID)
        return;
    {
        sfx_play_data_t d;
        memset(&d, 0, sizeof(d));
        d.chn = track_chn[track];
        d.idx = handles[idx];
        d.vol = vol;
        d.pan = 128;
        d.loop = (e->flags & SF_LOOP) != 0;
        d.loopstart = e->loopstart;
        d.loopend = e->loopend;
        snd_sfx_play_ex(&d);
    }
}

/* El tono del motor sube casi en linea recta con las revoluciones (medido en
 * MAME: f0 ~ 0.14 * rpm + 4.4 Hz). Se elige la muestra mas cercana y se
 * ajusta la frecuencia de reproduccion. */
static void engine_update(void)
{
    int i, best = -1, bestd = 1 << 30, freq, vol;
    const snd_entry *e;

    if (!nengine || engine_chn < 0)
        return;
    vol = scale_vol(engine_vol, -1);
    if (vol <= 0) {
        if (engine_cur >= 0)
            snd_sfx_stop(engine_chn);
        engine_cur = -1;
        return;
    }
    for (i = 0; i < nengine; i++) {
        int d = abs((int)entries[engine_idx[i]].param - engine_speed);
        if (d < bestd) {
            bestd = d;
            best = engine_idx[i];
        }
    }
    e = &entries[best];
    freq = (int)(e->rate * (0.14f * engine_speed + 4.4f) / (0.14f * e->param + 4.4f));
    if (freq < 1000)
        freq = 1000;
    if (freq > 88000)
        freq = 88000;
    if (best != engine_cur) {
        sfx_play_data_t d;
        memset(&d, 0, sizeof(d));
        d.chn = engine_chn;
        d.idx = handles[best];
        d.vol = vol;
        d.pan = 128;
        d.loop = 1;
        d.freq = freq;
        d.loopstart = e->loopstart;
        d.loopend = e->loopend;
        snd_sfx_play_ex(&d);
        engine_cur = best;
    } else {
        chn_update(engine_chn, freq, vol);
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
            if (mus_playing)
                snd_stream_volume(mus_hnd, scale_vol(255, 0));
        } else {
            track_vol[vol_target] = v;
            pending_track = vol_target;
            if (vol_target == 0 && mus_playing)
                snd_stream_volume(mus_hnd, scale_vol(255, 0));
        }
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

void sound_dcs_write(uint32_t v)
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
            for (t = 0; t < NTRACKS; t++)
                track_stop(t);
            if (engine_cur >= 0)
                snd_sfx_stop(engine_chn);
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

void sound_frame(void)
{
    if (!ready)
        return;
    if (mus_start_req == 2) {
        mus_start_req = 0;
        snd_stream_start_adpcm(mus_hnd, entries[mus_entry].rate, 0);
        snd_stream_volume(mus_hnd, mus_start_vol);
        mus_playing = 1;
    }
    if (mus_playing)
        snd_stream_poll(mus_hnd);
#ifdef CUSA_SNDLOG
    {
        static int n;
        if (++n % 57 == 0)
            printf("musica: %d mitades leidas, sonando %d, mitad %d pos %d\n",
                   mus_fills, mus_playing, mus_cur, mus_pos);
    }
#endif
}

int sound_init(const char *path)
{
    char hdr[16];
    int i;
    uint8_t *tmp = NULL;
    size_t tmp_size = 0;

    memset(by_code, 0xFF, sizeof(by_code));
    bank_file = fs_open(path, O_RDONLY);
    if (bank_file == FILEHND_INVALID) {
        printf("sonido: falta %s\n", path);
        return -1;
    }
    if (fs_read(bank_file, hdr, 16) != 16 || memcmp(hdr, "CUSASND1", 8)) {
        printf("sonido: %s no es un banco valido\n", path);
        return -1;
    }
    memcpy(&nentries, hdr + 8, 4);
    if (nentries > MAX_ENTRIES)
        nentries = MAX_ENTRIES;
    fs_read(bank_file, entries, nentries * sizeof(snd_entry));

    snd_stream_init();
    for (i = 0; i < nentries; i++) {
        snd_entry *e = &entries[i];
        handles[i] = SFXHND_INVALID;
        if (!(e->flags & SF_ENGINE) && e->code < sizeof(by_code) / sizeof(by_code[0]))
            by_code[e->code] = i;
        if (e->flags & SF_MUSIC)
            continue;
        if (e->bytes > tmp_size) {
            free(tmp);
            tmp_size = e->bytes;
            tmp = memalign(32, tmp_size);
        }
        fs_seek(bank_file, e->offset, SEEK_SET);
        if (!tmp || fs_read(bank_file, tmp, e->bytes) != (ssize_t)e->bytes)
            continue;
        handles[i] = snd_sfx_load_raw_buf((char *)tmp, e->bytes, e->rate,
                                          (e->flags & SF_PCM16) ? 16 : 4, 1);
        if (handles[i] == SFXHND_INVALID)
            printf("sonido: sin memoria para %04X\n", e->code);
        if ((e->flags & SF_ENGINE) && nengine < MAX_ENGINE && handles[i] != SFXHND_INVALID)
            engine_idx[nengine++] = i;
    }
    free(tmp);

    for (i = 0; i < NTRACKS; i++)
        track_chn[i] = snd_sfx_chn_alloc();
    engine_chn = snd_sfx_chn_alloc();

    mus_hnd = snd_stream_alloc(mus_callback, SND_STREAM_BUFFER_MAX_ADPCM);
    sem_init(&mus_sem, 0);
    thd_create(1, mus_reader, NULL);

    printf("sonido: %d entradas, %u bytes libres en el AICA\n", nentries,
           (unsigned)snd_mem_available());
    ready = 1;
    return 0;
}
