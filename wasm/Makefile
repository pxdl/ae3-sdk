# WASM target of the core: one standalone .wasm, no Emscripten JS glue.
#
# Standalone because the module must instantiate inside an AudioWorkletGlobalScope
# (no document, no fetch of glue scripts) and under Node for the bit-exactness
# harness -- the thin binding in js/ae3synth.mjs is the only JS involved.
#
# Bit-exactness stance (WEB_PORT_PLAN §1): WASM MVP has no FMA, so the
# -ffp-contract=off discipline is the platform default here; keep the flag pinned
# anyway (it governs the host-side compile of the doubles), and never enable
# relaxed SIMD on this target -- it would reintroduce fused ops.
EMCC   ?= emcc
CORE    = ../core
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -ffp-contract=off

SRCS = $(wildcard $(CORE)/*.c) abi.c

# Everything in ae3synth.h plus the abi.c flatteners, plus malloc/free for the
# binding's staging buffers. (get_stats/voice/seq_event/clock cross via abi.c.)
EXPORT_LIST = _malloc _free \
  _ae3_synth_new _ae3_synth_free _ae3_synth_error \
  _ae3_synth_load_bank _ae3_synth_load_seq \
  _ae3_synth_load_pitch_irx _ae3_synth_load_reverb_irx \
  _ae3_synth_gaussian _ae3_synth_reverb_depth \
  _ae3_synth_note_on _ae3_synth_note_off _ae3_synth_program \
  _ae3_synth_song_volume _ae3_synth_set_loop _ae3_synth_event_timing \
  _ae3_synth_cue_enable _ae3_synth_cue_scale _ae3_synth_cue_slider \
  _ae3_synth_cue_dolby _ae3_synth_cue_duck _ae3_synth_cue_duck_config \
  _ae3_synth_cue_songvol _ae3_synth_cue_duck_level \
  _ae3_synth_render _ae3_synth_done \
  _ae3_synth_seq_events _ae3_synth_seq_ppqn \
  _ae3_synth_bank_waveforms _ae3_synth_bank_waveform_pcm \
  _ae3w_pos _ae3w_voice _ae3w_seq_event _ae3w_clock _ae3w_stats \
  _ae3w_bank_waveform
comma   := ,
space   := $(subst ,, )
EXPORTS = $(subst $(space),$(comma),$(strip $(EXPORT_LIST)))

dist/ae3synth.wasm: $(SRCS) $(CORE)/ae3synth.h $(CORE)/internal.h Makefile
	@mkdir -p dist
	$(EMCC) $(CFLAGS) -I$(CORE) --no-entry \
	  -sSTANDALONE_WASM=1 \
	  -sEXPORTED_FUNCTIONS=$(EXPORTS) \
	  -sALLOW_MEMORY_GROWTH=1 \
	  -sINITIAL_MEMORY=33554432 \
	  -o $@ $(SRCS)

clean:
	rm -rf dist

.PHONY: clean
