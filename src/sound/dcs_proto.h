/*
 * dcs_proto.h - Decodificador del protocolo de la placa de sonido DCS,
 * independiente de la plataforma (docs/SONIDO.md).
 *
 * Recibe los bytes que el juego escribe en 0x9A0000 (y los cambios de la
 * linea de reset) y decide que sonar con el banco pre-renderizado
 * (tools/dcs_bank.py). La reproduccion la hace un "backend": el AICA en
 * Dreamcast (src/platform/sound_dc.c) o un mezclador en el ordenador que
 * escribe un WAV (src/host/sound_preview.c).
 */
#ifndef DCS_PROTO_H
#define DCS_PROTO_H

#include <stdint.h>

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

#define DCS_MAX_ENTRIES 320
#define DCS_NTRACKS     4
#define DCS_VOICE_ENGINE DCS_NTRACKS    /* voz 0-3: pistas del DCS, 4: motor */

typedef struct {
    /* voz (pista o motor): entrada del banco, volumen 0-255, frecuencia */
    void (*play)(int voice, int entry, int vol, int freq);
    void (*update)(int voice, int vol, int freq);
    void (*stop)(int voice);
    /* musica en streaming (pista 0): entrada o -1 para parar */
    void (*music)(int entry, int vol);
    void (*music_volume)(int vol);
} dcs_backend;

extern snd_entry dcs_entries[DCS_MAX_ENTRIES];
extern int dcs_nentries;

/* Cabecera del banco: 16 bytes + entradas. Devuelve el numero de entradas o -1. */
int dcs_parse_header(const uint8_t *hdr16);
/* Tras rellenar dcs_entries: indices por codigo y muestras de motor.
 * usable(i) dice si la entrada i se pudo cargar (NULL = todas). */
void dcs_proto_init(const dcs_backend *be, int (*usable)(int entry));
/* Se llama con cada codigo de sonido que pide el juego (vibracion, etc.) */
extern void (*dcs_code_hook)(int code);
/* Byte del puerto del DCS, o 0x100 | bit para la linea de reset */
void dcs_proto_write(uint32_t v);

#endif
