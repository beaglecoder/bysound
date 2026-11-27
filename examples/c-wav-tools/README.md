# c-wav-tools

Two small C programs on top of `../../c` (`bysound.h`), for recordings made with a phone.

```sh
cd examples/c-wav-tools && make           # build/batch-decode, build/noise-test
make demo                                 # builds the CLI, makes three WAVs, decodes the folder, sweeps noise
```

- `build/batch-decode DIR [--all]` decodes every `*.wav` (16-bit PCM, 16/44.1/48 kHz, first audio channel) and prints `file  band  profile  corrected  text`. `--all` also lists files with nothing decoded.
- `build/noise-test in.wav --snr 6 [--seed N] [-o noisy.wav]` or `--sweep FROM:TO:STEP` adds white Gaussian noise (SNR relative to the loud part of the file, over the full 0..Nyquist band, so it is more lenient than an in-band SNR) and reports whether it still decodes. int16 clipping makes anything below about -12 dB meaningless. Use it relatively: compare channels, profiles and recordings.

`wavtools.h` holds the WAV reader/writer and a `wt_decode()` helper (both profiles, every channel under Nyquist). Needs a C99 compiler (`CC=clang` or the default `cc`); `C_DIR=path/to/c` points at another checkout of the library. Build flags `-DBYS_DEMOD_ROBUST=1 -DBYS_DEMOD_LOW_BANK=1` match the library's CLI.
