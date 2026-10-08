/* pvr_render.h - Render de los poligonos del V-Unit con el PowerVR. */
#ifndef PVR_RENDER_H
#define PVR_RENDER_H

/* Estadisticas por el puerto serie una vez por segundo: solo en las
 * versiones de prueba. En la consola el puerto serie espera a enviar cada
 * caracter y el frame de ese segundo se alargaba unos 25 ms (un tiron). */
#if defined(CUSA_AUTOPLAY) || defined(CUSA_PROF) || defined(CUSA_SHOT)
#define CUSA_STATS 1
#endif

void pvrr_init(void);
void pvrr_frame(int shot);      /* llamar en cada interrupcion de video */
/* Depuracion: reservar ya la textura de las capturas (CUSA_SHOT) */
void pvrr_reserve_shot(void);
void pvrr_stats(unsigned *converted, unsigned *polys);
/* Imagenes nuevas mostradas desde la ultima llamada */
unsigned pvrr_pages(void);

#endif
