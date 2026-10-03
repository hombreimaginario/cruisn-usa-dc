# Cruis'n USA DC - compilacion para Dreamcast con KallistiOS.
# Requiere el entorno de KOS cargado (source $KOS_BASE/environ.sh).

TARGET = cruisn-usa-dc.elf

OBJS = src/platform/main_dc.o \
       src/c3x/interp.o \
       src/vunit/mem.o \
       src/vunit/video.o \
       src/platform/pvr_render.o

KOS_CFLAGS += -std=gnu99 -O2 -Wall -Wextra -DVU_NO_RAM2 -DVU_NO_UNMAPPED_LOG

# Si existe el codigo recompilado (tools/c31recomp.py -> generated/recomp),
# se usa en lugar del interprete.
RECOMP_DIR ?= generated/recomp
ifneq ($(wildcard $(RECOMP_DIR)/files.mk),)
include $(RECOMP_DIR)/files.mk
OBJS += src/recomp/rt.o src/recomp/hle.o $(RECOMP_SRCS:.c=.o)
KOS_CFLAGS += -DCUSA_RECOMP -I$(CURDIR)/src/recomp -Wno-unused-label
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
MKDCDISC ?= mkdcdisc
GENERATED ?= generated
cdi: $(TARGET)
	$(MKDCDISC) -e $(TARGET) -o cruisn-usa-dc.cdi --allow-overwrite -n "CRUISN USA DC" \
		-f $(GENERATED)/program.bin -f $(GENERATED)/gfx.bin \
		$(if $(wildcard $(GENERATED)/cmos.bin),-f $(GENERATED)/cmos.bin)

.PHONY: all clean rm-elf run cdi
