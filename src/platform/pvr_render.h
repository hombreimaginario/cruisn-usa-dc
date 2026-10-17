/* pvr_render.h - Render de los poligonos del V-Unit con el PowerVR. */
#ifndef PVR_RENDER_H
#define PVR_RENDER_H

/* Estadisticas por el puerto serie una vez por segundo: solo en las
 * versiones de prueba. En la consola el puerto serie espera a enviar cada
 * caracter y el frame de ese segundo se alargaba unos 25 ms (un tiron). */
#if defined(CUSA_AUTOPLAY) || defined(CUSA_PROF) || defined(CUSA_SHOT)
#define CUSA_STATS 1
#endif

/* 0 si todo bien; -1 sin memoria para las listas de poligonos */
int pvrr_init(void);
void pvrr_frame(int shot);
/* Imagen de la pantalla de carga (logo 256x256 + texto 512x64, ARGB1555):
 * se muestra hasta el primer frame con poligonos y despues se libera */
void pvrr_set_loading(const uint16_t *img);
/* rotulos de tools/make_texts.py (4 anchos + textura 512x128); fila a mostrar (-1: ninguna) */
void pvrr_set_texts(const uint32_t *buf);
extern int pvrr_text_row;
extern float pvrr_text_y;                     /* en pixeles del arcade (400) */
extern float pvrr_text_scale;                 /* 1 = tamano natural */      /* llamar en cada interrupcion de video */
/* Depuracion: reservar ya la textura de las capturas (CUSA_SHOT) */
void pvrr_reserve_shot(void);
void pvrr_stats(unsigned *converted, unsigned *polys);
/* Imagenes nuevas mostradas desde la ultima llamada */
unsigned pvrr_pages(void);

#endif
