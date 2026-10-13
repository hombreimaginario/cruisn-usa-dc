#ifndef CMOS_VMU_H
#define CMOS_VMU_H

#include <stddef.h>
#include <stdint.h>

/* Carga la CMOS de la VMU o, si no hay, la del CD. Devuelve 1 si vino de la VMU. */
/* ignore_vmu: no leer la partida guardada (se usa la del CD y al guardar se
 * sustituye) */
int cmos_init(const uint32_t *cd_cmos, size_t cd_size, int ignore_vmu);
/* Llamar cada pocos segundos: guarda en la VMU si la CMOS cambio y ya no cambia */
void cmos_check(void);

#endif
