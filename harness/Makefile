CC     ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -ffp-contract=off

CORE    = ../core
CORELIB = $(CORE)/libae3synth.a

all: wavdump bgmplay exstdump serender

wavdump: wavdump.o $(CORELIB)
	$(CC) $(CFLAGS) -o $@ wavdump.o $(CORELIB) -lm

# EXST (.x) stream inspect/decode -- public-API consumer like bgmplay
exstdump: exstdump.o $(CORELIB)
	$(CC) $(CFLAGS) -o $@ exstdump.o $(CORELIB) -lm

serender: serender.o $(CORELIB)
	$(CC) $(CFLAGS) -o $@ serender.o $(CORELIB) -lm

# GUI player/visualizer harness (SDL2 via homebrew pkg-config)
bgmplay: bgmplay.o $(CORELIB)
	$(CC) $(CFLAGS) -o $@ bgmplay.o $(CORELIB) $(shell pkg-config --libs sdl2) -lm

wavdump.o: wavdump.c $(CORE)/ae3synth.h $(CORE)/internal.h
	$(CC) $(CFLAGS) -I$(CORE) -c wavdump.c

bgmplay.o: bgmplay.c $(CORE)/ae3synth.h
	$(CC) $(CFLAGS) -I$(CORE) $(shell pkg-config --cflags sdl2) -c bgmplay.c

exstdump.o: exstdump.c $(CORE)/ae3synth.h
	$(CC) $(CFLAGS) -I$(CORE) -c exstdump.c

serender.o: serender.c $(CORE)/ae3synth.h
	$(CC) $(CFLAGS) -I$(CORE) -c serender.c

$(CORELIB):
	$(MAKE) -C $(CORE)

clean:
	rm -f *.o wavdump bgmplay exstdump serender

.PHONY: all clean
