# Cruis'n USA DC - compilacion para Dreamcast con KallistiOS.
# Requiere el entorno de KOS cargado (source $KOS_BASE/environ.sh).

TARGET = cruisn-usa-dc.elf

OBJS = src/platform/main_dc.o \
       src/vunit/mem.o

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

.PHONY: all clean rm-elf run
