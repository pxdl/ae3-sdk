CC     ?= cc
# -ffp-contract=off: no fused multiply-add in the sequencer's sample-position
# doubles -- event timing stays bit-identical across compilers/arches, which the
# bit-exactness gates (native check + WASM A/B) depend on.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -ffp-contract=off

OBJS = bank.o seq.o voice.o pitch.o gauss.o vol.o reverb.o synth.o cue.o exst.o

libae3synth.a: $(OBJS)
	ar rcs $@ $(OBJS)

%.o: %.c ae3synth.h internal.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o libae3synth.a

.PHONY: clean
