#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

enum { WD_TA_TIMEOUT, WD_VBUF_FULL, WD_CD_GFX, WD_CD_MUSIC, WD_INTERP, WD_CD_ERR, WD_NCOUNT };

/* Que esta haciendo el hilo principal / los hilos de musica y VMU */
extern volatile const char *wd_phase;
extern volatile const char *wd_thread_phase;
extern volatile uint32_t wd_heartbeat;          /* +1 por interrupcion de video */
extern volatile uint32_t wd_counters[WD_NCOUNT];
extern volatile uint32_t wd_heap_kb;            /* memoria libre al arrancar */

void watchdog_start(void);

#endif
