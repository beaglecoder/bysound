# bysound example tools, built against ../../c. `make` builds build/batch-decode and build/noise-test; `make demo` runs both on generated WAVs.
CC     ?= clang
C_DIR  ?= ../../c
# The demodulator must support what the CLI supports: robust profile and the audible band (these macros change the struct size).
DEFS   := -DBYS_DEMOD_FFT=1 -DBYS_DEMOD_ROBUST=1 -DBYS_DEMOD_LOW_BANK=1
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror -pedantic -I$(C_DIR)/include -I$(C_DIR)/src
LIB_SRC := $(wildcard $(C_DIR)/src/*.c)
BUILD  := build

all: $(BUILD)/batch-decode $(BUILD)/noise-test

$(BUILD)/%: %.c wavtools.h $(LIB_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LIB_SRC) -lm

$(BUILD):
	mkdir -p $(BUILD)

# Makes a few WAVs with the library's own CLI, then decodes the folder and sweeps the noise.
demo: all
	$(MAKE) -C $(C_DIR) cli
	mkdir -p $(BUILD)/wavs
	$(C_DIR)/build/bysound encode "hello top" -o $(BUILD)/wavs/top.wav
	$(C_DIR)/build/bysound encode "hello mid robust" -o $(BUILD)/wavs/mid-robust.wav --band mid --profile robust
	$(C_DIR)/build/bysound encode "hello audible" -o $(BUILD)/wavs/audible.wav --band audible --rate 44100
	$(BUILD)/batch-decode $(BUILD)/wavs
	$(BUILD)/noise-test $(BUILD)/wavs/top.wav --sweep 0:-16:4

clean:
	rm -rf $(BUILD)

.PHONY: all demo clean
