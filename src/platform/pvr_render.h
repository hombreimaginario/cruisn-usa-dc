/* pvr_render.h - Render de los poligonos del V-Unit con el PowerVR. */
#ifndef PVR_RENDER_H
#define PVR_RENDER_H

void pvrr_init(void);
void pvrr_frame(int shot);      /* llamar en cada interrupcion de video */
void pvrr_stats(unsigned *converted, unsigned *polys);

#endif
