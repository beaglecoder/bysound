# bysound v4 vs ggwave benchmark. `./fetch-ggwave.sh && make && make run`
CC      ?= clang
CXX     ?= clang++
JOBS    ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)
SBS     := ../c
GG      := third_party/ggwave
# bysound: FFT engine, robust profile and the audible channel compiled in (as in ../c/Makefile's demodulator tests)
BYS_DEFS := -DBYS_DEMOD_FFT=1 -DBYS_DEMOD_ROBUST=1 -DBYS_DEMOD_LOW_BANK=1
CFLAGS   := -std=c99 -O2 -Wall -Wextra $(BYS_DEFS) -I$(SBS)/include -I$(SBS)/src
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra $(BYS_DEFS) -I$(SBS)/include -I$(GG)/include -I$(GG)/src
BUILD   := build
BYS_SRC := $(wildcard $(SBS)/src/*.c)
BYS_OBJ := $(patsubst $(SBS)/src/%.c,$(BUILD)/sbs/%.o,$(BYS_SRC))
PLANS   := snr size cond rate44 mtfixed idle inaudible inaudible-idle
# Inaudible comparison only (bysound top vs ggwave DT/MT moved to 18 kHz): `make inaudible`
INAUDIBLE_PLANS := inaudible inaudible-idle

all: $(BUILD)/bench

$(BUILD)/sbs/%.o: $(SBS)/src/%.c $(wildcard $(SBS)/include/*.h $(SBS)/src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ggwave.o: $(GG)/src/ggwave.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

$(GG)/src/ggwave.cpp:
	@echo "ggwave is missing: run ./fetch-ggwave.sh first"; exit 1

$(BUILD)/bench: src/bench.cpp $(BYS_OBJ) $(BUILD)/ggwave.o
	$(CXX) $(CXXFLAGS) -o $@ src/bench.cpp $(BYS_OBJ) $(BUILD)/ggwave.o

# Every plan, sharded over $(JOBS) processes; shards are merged into results/<plan>.csv, then summary.md and the SVG charts.
run: $(BUILD)/bench
	@mkdir -p results/shards
	$(BUILD)/bench info results
	@$(MAKE) --no-print-directory plans PLANS="$(PLANS)"
	$(MAKE) scans scans-inaudible
	python3 scripts/report.py results

inaudible: $(BUILD)/bench
	@mkdir -p results/shards
	$(BUILD)/bench info results
	@$(MAKE) --no-print-directory plans PLANS="$(INAUDIBLE_PLANS)"
	$(MAKE) scans-inaudible
	python3 scripts/report.py results

plans: $(BUILD)/bench
	@for p in $(PLANS); do \
	  echo "plan $$p: $$($(BUILD)/bench jobs $$p) jobs on $(JOBS) processes"; \
	  start=$$(date +%s); \
	  seq 0 $$(($(JOBS) - 1)) | xargs -P $(JOBS) -I{} $(BUILD)/bench run $$p {} $(JOBS) results/shards/$$p.{}.csv || exit 1; \
	  { head -1 results/shards/$$p.0.csv; for f in results/shards/$$p.*.csv; do tail -n +2 $$f; done; } > results/$$p.csv; \
	  echo "  done in $$(( $$(date +%s) - start )) s"; \
	done

# Noiseless delivery scan (both systems, many payloads, no channel) and bysound symbol-level codec round trip (no audio).
scans: $(BUILD)/bench
	@echo "noiseless scans"
	@$(BUILD)/bench scan 8 500 > results/noiseless_scan_8B.txt & $(BUILD)/bench scan 32 500 > results/noiseless_scan_32B.txt & \
	 $(BUILD)/bench scan 128 300 > results/noiseless_scan_128B.txt & \
	 { for s in 8 32 128 512 2048; do $(BUILD)/bench codec $$s 300; done > results/bysound_codec_scan.txt; } & wait

# Same scan for bysound top and ggwave DT/MT moved to 18 kHz (receiver restricted to those protocols), and the check of ggwave DT's
# clean-point failures (each failing clean trial re-run with noise at +60 dB SNR, same seed).
scans-inaudible: $(BUILD)/bench
	@echo "noiseless scans, inaudible channels"
	@$(BUILD)/bench scan 8 500 inaudible > results/noiseless_scan_inaudible_8B.txt & $(BUILD)/bench scan 32 500 inaudible > results/noiseless_scan_inaudible_32B.txt & \
	 $(BUILD)/bench scan 128 300 inaudible > results/noiseless_scan_inaudible_128B.txt & \
	 $(BUILD)/bench dither snr size cond inaudible > results/dt_clean_check.txt & wait

report:
	python3 scripts/report.py results

# Smoke test: every configuration, 2 SNRs + clean, 2 trials.
quick: $(BUILD)/bench
	@mkdir -p results/shards
	seq 0 $$(($(JOBS) - 1)) | xargs -P $(JOBS) -I{} $(BUILD)/bench run quick {} $(JOBS) results/shards/quick.{}.csv
	@{ head -1 results/shards/quick.0.csv; for f in results/shards/quick.*.csv; do tail -n +2 $$f; done; } > results/quick.csv

.PHONY: all run plans inaudible report quick scans scans-inaudible
