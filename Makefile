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
       src/platform/watchdog.o

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
src/recomp/hle.o src/platform/pvr_render.o: KOS_CFLAGS += -O3 -funroll-loops

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
GENERATED ?= generated
cdi: $(TARGET)
	$(MKDCDISC) -e $(TARGET) -o cruisn-usa-dc-$(CUSA_VERSION).cdi --allow-overwrite -n "CRUISN USA DC" \
		-f $(GENERATED)/program.bin -f $(GENERATED)/gfx.bin \
		$(if $(wildcard $(GENERATED)/cmos.bin),-f $(GENERATED)/cmos.bin) \
		$(if $(wildcard $(GENERATED)/sound.bin),-f $(GENERATED)/sound.bin)

.PHONY: all clean rm-elf run cdi
