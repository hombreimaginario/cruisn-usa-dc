# Cruis'n USA DC - compilacion para Dreamcast con KallistiOS.
# Requiere el entorno de KOS cargado (source $KOS_BASE/environ.sh).

TARGET = cruisn-usa-dc.elf

OBJS = src/platform/main_dc.o \
       src/c3x/interp.o \
       src/vunit/mem.o \
       src/vunit/video.o \
       src/platform/pvr_render.o \
       src/platform/sound_dc.o \
       src/sound/dcs_proto.o \
       src/platform/cmos_vmu.o \
       src/platform/watchdog.o \
       src/platform/vmu_lcd.o \
       src/platform/rumble.o \
       src/platform/textfont.o

CUSA_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null || echo local)$(shell git diff --quiet 2>/dev/null || echo +)
# version.h con el commit (se reescribe solo si cambia)
$(shell echo '#define CUSA_VERSION "$(CUSA_VERSION)"' > src/platform/version.h.new; \
        cmp -s src/platform/version.h.new src/platform/version.h || cp src/platform/version.h.new src/platform/version.h; \
        rm -f src/platform/version.h.new)
KOS_CFLAGS += -std=gnu99 -O2 -Wall -Wextra -DVU_NO_RAM2 -DVU_NO_UNMAPPED_LOG

# Si existe el codigo recompilado (tools/c31recomp.py -> generated/recomp),
# se usa en lugar del interprete.
RECOMP_DIR ?= generated/recomp
ifneq ($(wildcard $(RECOMP_DIR)/files.mk),)
include $(RECOMP_DIR)/files.mk
OBJS += src/recomp/rt.o src/recomp/hle.o $(RECOMP_SRCS:.c=.o)
KOS_CFLAGS += -DCUSA_RECOMP -I$(CURDIR)/src/recomp -Wno-unused-label
endif

src/platform/main_dc.o src/platform/watchdog.o: src/platform/version.h

# Bucles calientes (HLE y render): -O3 con desenrollado da ~10% mas de
# imagenes por segundo en carrera; el resto queda en -O2 por tamano.
HLE_OPT ?= -O3 -funroll-loops
src/recomp/hle.o src/platform/pvr_render.o: KOS_CFLAGS += $(HLE_OPT)

# Codigo traducido del C31 con -Os: con -O2 eran ~6 MB y la RAM de la
# consola (16 MB) no daba para todo (fallaban reservas de memoria). Con -Os
# ocupa 1,8 MB menos y en carrera va igual o algo mejor (menos fallos de
# cache de instrucciones).
$(RECOMP_SRCS:.c=.o): KOS_CFLAGS += -Os
# Ficheros del codigo traducido que van con -O2: rc_000 tiene el bucle del
# juego y DIRQ (lo mas caliente); con -O2 la prueba determinista de Iowa va
# un 3 % mas rapida a cambio de ~400 KB. Los demas no ganan nada.
RECOMP_HOT ?= $(RECOMP_DIR)/rc_000.o
ifneq ($(RECOMP_HOT),)
HOT_OPT ?= -O2
$(RECOMP_HOT): KOS_CFLAGS += $(HOT_OPT)
endif

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS)

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

# Imagen de CD con los datos generados por tools/romtool.py (no se versiona).
# El nombre lleva el commit (cruisn-usa-dc-<commit>.cdi) para distinguir versiones.
MKDCDISC ?= mkdcdisc
# Las imagenes se nombran por fecha (como piden los testers):
# cruisn-usa-dc-AAAA-MM-DD[-variante].cdi, y el titulo del disco (lo que
# muestra el menu de GDemu) lleva lo mismo. El commit sigue en version.h.
CDI_DATE ?= $(shell date +%Y-%m-%d)
CDI_VARIANT ?=
CDI_OUT ?= cruisn-usa-dc-$(CDI_DATE)$(if $(CDI_VARIANT),-$(CDI_VARIANT)).cdi
CDI_TITLE ?= CRUISN USA $(CDI_DATE)$(if $(CDI_VARIANT), $(shell echo $(CDI_VARIANT) | tr a-z A-Z))
GENERATED ?= generated
cdi: $(TARGET)
	$(MKDCDISC) -e $(TARGET) -o $(CDI_OUT) --allow-overwrite -n "$(CDI_TITLE)" \
		-f $(GENERATED)/program.bin -f $(GENERATED)/gfx.bin \
		$(if $(wildcard $(GENERATED)/cmos.bin),-f $(GENERATED)/cmos.bin) \
		$(if $(wildcard $(GENERATED)/sound.bin),-f $(GENERATED)/sound.bin) \
		$(if $(wildcard $(GENERATED)/loading.bin),-f $(GENERATED)/loading.bin) \
		$(if $(wildcard $(GENERATED)/texts.bin),-f $(GENERATED)/texts.bin)

.PHONY: all clean rm-elf run cdi
