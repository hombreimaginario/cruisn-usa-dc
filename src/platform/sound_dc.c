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
#include "../sound/dcs_proto.h"

#define entries dcs_entries
#define nentries dcs_nentries
#define NTRACKS DCS_NTRACKS

static sfxhnd_t handles[DCS_MAX_ENTRIES];
static file_t bank_file = FILEHND_INVALID;
static int ready;

/* ---- canales del AICA: una voz por pista del DCS y otra para el motor ---- */

static int voice_chn[DCS_NTRACKS + 1];

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

/* ---- backend del decodificador (src/sound/dcs_proto.c) ---- */

static void be_play(int voice, int idx, int vol, int freq)
{
    const snd_entry *e = &entries[idx];
    sfx_play_data_t d;

#ifdef CUSA_SNDLOG
    if (voice < DCS_NTRACKS)
        printf("snd %04X pista %d vol %d\n", e->code, voice, vol);
#endif
    if (handles[idx] == SFXHND_INVALID)
        return;
    memset(&d, 0, sizeof(d));
    d.chn = voice_chn[voice];
    d.idx = handles[idx];
    d.vol = vol;
    d.pan = 128;
    d.freq = freq;
    d.loop = (e->flags & SF_LOOP) != 0;
    d.loopstart = e->loopstart;
    d.loopend = e->loopend;
    snd_sfx_play_ex(&d);
}

static void be_update(int voice, int vol, int freq)
{
    chn_update(voice_chn[voice], freq, vol);
}

static void be_stop(int voice)
{
    snd_sfx_stop(voice_chn[voice]);
}

static void be_music(int idx, int vol)
{
#ifdef CUSA_SNDLOG
    printf("snd musica %d vol %d\n", idx >= 0 ? entries[idx].code : -1, vol);
#endif
    if (idx < 0)
        music_stop();
    else
        music_play(idx, vol);
}

static void be_music_volume(int vol)
{
    mus_start_vol = vol;
    if (mus_playing)
        snd_stream_volume(mus_hnd, vol);
}

static const dcs_backend aica_backend = {
    be_play, be_update, be_stop, be_music, be_music_volume
};

static int be_usable(int idx)
{
    return handles[idx] != SFXHND_INVALID;
}

void sound_dcs_write(uint32_t v)
{
    if (ready)
        dcs_proto_write(v);
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

    bank_file = fs_open(path, O_RDONLY);
    if (bank_file == FILEHND_INVALID) {
        printf("sonido: falta %s\n", path);
        return -1;
    }
    if (fs_read(bank_file, hdr, 16) != 16 || (nentries = dcs_parse_header((uint8_t *)hdr)) < 0) {
        printf("sonido: %s no es un banco valido\n", path);
        nentries = 0;
        return -1;
    }
    fs_read(bank_file, entries, nentries * sizeof(snd_entry));

    snd_stream_init();
    for (i = 0; i < nentries; i++) {
        snd_entry *e = &entries[i];
        handles[i] = SFXHND_INVALID;
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
    }
    free(tmp);

    for (i = 0; i <= DCS_NTRACKS; i++)
        voice_chn[i] = snd_sfx_chn_alloc();
    dcs_proto_init(&aica_backend, be_usable);

    mus_hnd = snd_stream_alloc(mus_callback, SND_STREAM_BUFFER_MAX_ADPCM);
    sem_init(&mus_sem, 0);
    thd_create(1, mus_reader, NULL);

    printf("sonido: %d entradas, %u bytes libres en el AICA\n", nentries,
           (unsigned)snd_mem_available());
    ready = 1;
    return 0;
}
