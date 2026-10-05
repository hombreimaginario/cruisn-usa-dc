/*
 * watchdog.c - Diagnostico de cuelgues en hardware real.
 *
 * Un hilo de prioridad alta mira 10 veces por segundo si llegan
 * interrupciones de video (el planificador de KOS es preemptivo, asi que se
 * ejecuta aunque el hilo principal este en un bucle). Si pasan 3 segundos sin ninguna, el hilo principal esta parado:
 * se muestra en pantalla (y por el puerto serie) donde estaba, para poder
 * localizar el fallo sin cable de depuracion. La pantalla se refresca cada
 * segundo con la posicion actual por si va cambiando.
 */
#include <kos.h>
#include <dc/biosfont.h>
#include <dc/video.h>

#include <stdio.h>
#include <string.h>

#include "watchdog.h"

volatile const char *wd_phase = "arranque";
volatile const char *wd_thread_phase = "-";
volatile uint32_t wd_heartbeat;
volatile uint32_t wd_counters[WD_NCOUNT];

static kthread_t *main_thread;
static uint32_t last_beat, stale_ticks, shown;

static void line(int row, const char *s)
{
    bfont_draw_str(vram_s + (40 + row * 26) * 640 + 24, 640, 1, s);
}

static void show(void)
{
    char buf[96];
    const irq_context_t *m = main_thread ? &main_thread->context : NULL;
    int r = 0;

    /* fondo negro y la pantalla de la CPU visible encima del PowerVR */
    memset(vram_s, 0, 640 * 480 * 2);
    vid_set_start(0);
#ifdef CUSA_VERSION
    line(r++, "CRUIS'N USA DC " CUSA_VERSION " - EL JUEGO NO RESPONDE");
#else
    line(r++, "CRUIS'N USA DC - EL JUEGO NO RESPONDE");
#endif
    r++;
    snprintf(buf, sizeof(buf), "fase: %s", wd_phase);
    line(r++, buf);
    snprintf(buf, sizeof(buf), "hilo musica/VMU: %s", wd_thread_phase);
    line(r++, buf);
    snprintf(buf, sizeof(buf), "frames de video: %lu", (unsigned long)wd_heartbeat);
    line(r++, buf);
    if (m) {
        snprintf(buf, sizeof(buf), "hilo principal PC %08lX PR %08lX", (unsigned long)m->pc, (unsigned long)m->pr);
        line(r++, buf);
    }
    snprintf(buf, sizeof(buf), "TA agotado %lu  vertices llenos %lu",
             (unsigned long)wd_counters[WD_TA_TIMEOUT], (unsigned long)wd_counters[WD_VBUF_FULL]);
    line(r++, buf);
    snprintf(buf, sizeof(buf), "lecturas CD graf %lu  musica %lu",
             (unsigned long)wd_counters[WD_CD_GFX], (unsigned long)wd_counters[WD_CD_MUSIC]);
    line(r++, buf);
    snprintf(buf, sizeof(buf), "codigo interpretado %lu", (unsigned long)wd_counters[WD_INTERP]);
    line(r++, buf);
    r++;
    line(r++, "Haz una foto de esta pantalla, por favor.");
    printf("watchdog: fase %s, hilo %s, frames %lu, PC %08lx PR %08lx\n", wd_phase, wd_thread_phase,
           (unsigned long)wd_heartbeat, m ? (unsigned long)m->pc : 0, m ? (unsigned long)m->pr : 0);
}

static void *wd_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thd_sleep(100);
        if (wd_heartbeat != last_beat) {
            last_beat = wd_heartbeat;
            stale_ticks = 0;
            continue;
        }
        if (++stale_ticks >= 30 && (stale_ticks - 30) % 10 == 0) {
            show();
            shown++;
        }
    }
    return NULL;
}

void watchdog_start(void)
{
    kthread_t *t;
    main_thread = thd_get_current();
    t = thd_create(1, wd_thread, NULL);
    if (t)
        thd_set_prio(t, PRIO_DEFAULT / 2);
}
