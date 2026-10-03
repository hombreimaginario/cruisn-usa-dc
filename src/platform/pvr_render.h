/* pvr_render.h - Render de los poligonos del V-Unit con el PowerVR. */
#ifndef PVR_RENDER_H
#define PVR_RENDER_H

void pvrr_init(void);
void pvrr_frame(int shot);      /* llamar en cada interrupcion de video */
/* Depuracion: reservar ya la textura de las capturas (CUSA_SHOT) */
void pvrr_reserve_shot(void);
void pvrr_stats(unsigned *converted, unsigned *polys);
/* Imagenes nuevas mostradas desde la ultima llamada */
unsigned pvrr_pages(void);

#endif
