# bysound on the host (clang). `make test`, `make cli`, `make cli-test`, `make size`, `make vectors` (regenerates the .h files with Node).
CC      ?= clang
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -pedantic -Iinclude -Isrc
SRC     := $(wildcard src/*.c)
BUILD   := build

test: $(BUILD)/test_all demod
	$(BUILD)/test_all

# Demodulator: one binary per engine (float32 FFT and integer Goertzel), same tests; the two transcripts must match.
DEMOD_DEFS := -DBYS_DEMOD_LOW_BANK=1 -DBYS_DEMOD_ROBUST=1 -DGOLDEN_DIR='"test/golden"' -DVECTORS_DIR='"../test-vectors"' -DRECORDINGS_DIR='"test/recordings"'
DEMOD_CFLAGS := $(CFLAGS) -O2 -g -fsanitize=address,undefined
DEMOD_DEPS := $(SRC) test/test_demod.c $(wildcard include/*.h src/*.h test/*.h)
demod: $(BUILD)/test_demod_fft $(BUILD)/test_demod_gz
	$(BUILD)/test_demod_fft $(BUILD)/transcript_fft.txt
	$(BUILD)/test_demod_gz $(BUILD)/transcript_gz.txt
	@cmp $(BUILD)/transcript_fft.txt $(BUILD)/transcript_gz.txt && echo "FFT and Goertzel: identical transcripts ($$(wc -l < $(BUILD)/transcript_fft.txt) lines)"

$(BUILD)/test_demod_fft: $(DEMOD_DEPS) | $(BUILD)
	$(CC) $(DEMOD_CFLAGS) $(DEMOD_DEFS) -DBYS_DEMOD_FFT=1 -o $@ $(SRC) test/test_demod.c -lm
# (Goertzel runs with UBSan only: under ASan the 1024-sample-per-tone loop takes 3x longer and the FFT run already checks memory)
$(BUILD)/test_demod_gz: $(DEMOD_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -O2 -g -fsanitize=undefined $(DEMOD_DEFS) -DBYS_DEMOD_GOERTZEL=1 -o $@ $(SRC) test/test_demod.c -lm

# Full noise sweep and CPU cost (indicative), optimized and without sanitizers.
FAST := $(CFLAGS) -O3 $(DEMOD_DEFS)
fast: $(DEMOD_DEPS) | $(BUILD)
	$(CC) $(FAST) -DBYS_DEMOD_FFT=1 -o $(BUILD)/fast_fft $(SRC) test/test_demod.c -lm
	$(CC) $(FAST) -DBYS_DEMOD_GOERTZEL=1 -o $(BUILD)/fast_gz $(SRC) test/test_demod.c -lm
sweep: fast
	$(BUILD)/fast_fft /dev/null --sweep
	$(BUILD)/fast_gz /dev/null --sweep
bench: fast
	$(BUILD)/fast_fft /dev/null --bench
	$(BUILD)/fast_gz /dev/null --bench

# bys_demod_t size per engine, rate and options (on a 64-bit host).
sizes: | $(BUILD)
	@for e in FFT GOERTZEL; do for r in 44100 48000; do for x in "0 0" "1 0" "0 1"; do set -- $$x; \
	  $(CC) $(CFLAGS) -DBYS_DEMOD_$$e=1 -DBYS_DEMOD_MAX_RATE=$$r -DBYS_DEMOD_ROBUST=$$1 -DBYS_DEMOD_LOW_BANK=$$2 -o $(BUILD)/sizes tools/demod_sizes.c -lm && $(BUILD)/sizes $$e $$r $$1 $$2; done; done; done

$(BUILD)/test_all: $(SRC) test/test_all.c $(wildcard include/*.h src/*.h test/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -O1 -g -DGOLDEN_DIR='"test/golden"' -fsanitize=address,undefined -o $@ $(SRC) test/test_all.c -lm

$(BUILD):
	mkdir -p $(BUILD)

# Indicative code size (-Os, object by object).
size: | $(BUILD)
	@for f in $(SRC); do $(CC) $(CFLAGS) -Os -c $$f -o $(BUILD)/$$(basename $$f .c).o; done
	@size $(BUILD)/*.o | awk 'NR==1 || !/test_all/'
	@echo "(__TEXT includes the const tables: about 770 B of GF table in rs.o and 520 B of sine in tx.o)"

# Command-line tool (encode/decode WAV files) and its round-trip test.
CLI_DEFS := -DBYS_DEMOD_FFT=1 -DBYS_DEMOD_ROBUST=1 -DBYS_DEMOD_LOW_BANK=1
cli: $(BUILD)/bysound
$(BUILD)/bysound: examples/bysound_cli.c $(SRC) $(wildcard include/*.h src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -O2 $(CLI_DEFS) -o $@ $(SRC) examples/bysound_cli.c -lm

cli-test: $(BUILD)/bysound
	sh examples/cli_test.sh $(BUILD)/bysound test/recordings/payload-top-v4.wav $(BUILD)

vectors:
	node tools/gen_tables.mjs
	node tools/gen_vectors.mjs
	node tools/gen_audio_expected.mjs

clean:
	rm -rf $(BUILD)

.PHONY: cli cli-test test demod fast sweep bench sizes size vectors clean
