# Cruis'n USA DC - compilacion para Dreamcast con KallistiOS.
# Requiere el entorno de KOS cargado (source $KOS_BASE/environ.sh).

TARGET = cruisn-usa-dc.elf

OBJS = src/platform/main_dc.o \
       src/c3x/interp.o \
       src/vunit/mem.o \
       src/vunit/video.o

KOS_CFLAGS += -std=gnu99 -O2 -Wall -Wextra

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
