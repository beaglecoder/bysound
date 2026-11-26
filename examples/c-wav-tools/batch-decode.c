/*
 * batch-decode: decode every WAV in a folder and print a table.
 *   batch-decode DIR [--all]        (--all also lists files with nothing decoded)
 * Output: file, band, profile, corrected bytes, text. Handy for a folder of phone recordings. Exit code 0 if anything decoded.
 */
#define _POSIX_C_SOURCE 200809L
#include "wavtools.h"
#include <dirent.h>
#include <strings.h>

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int is_wav(const char *name) { size_t n = strlen(name); return n > 4 && !strcasecmp(name + n - 4, ".wav"); }

int main(int argc, char **argv) {
  (void)wav_write; /* shared header; only noise-test writes WAVs */
  if (argc < 2) { fputs("usage: batch-decode DIR [--all]\n", stderr); return 2; }
  int all = argc > 2 && !strcmp(argv[2], "--all");
  DIR *d = opendir(argv[1]);
  if (!d) { perror(argv[1]); return 2; }
  char **names = NULL;
  size_t count = 0;
  for (struct dirent *e; (e = readdir(d));) {
    if (!is_wav(e->d_name)) continue;
    names = realloc(names, (count + 1) * sizeof *names);
    names[count++] = strdup(e->d_name);
  }
  closedir(d);
  if (count) qsort(names, count, sizeof *names, cmp);

  static wt_result_t res;
  int decoded_files = 0, total_frames = 0;
  printf("%-32s %-8s %-8s %-9s %s\n", "file", "band", "profile", "corrected", "text");
  for (size_t i = 0; i < count; i++) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", argv[1], names[i]);
    size_t n = 0; uint32_t rate = 0;
    int16_t *pcm = wav_read(path, &n, &rate, 1);
    if (!pcm) { printf("%-32s %s\n", names[i], "(not a supported WAV: need 16-bit PCM, 16/44.1/48 kHz)"); free(names[i]); continue; }
    int k = wt_decode(pcm, n, rate, &res);
    if (k) decoded_files++;
    total_frames += k;
    if (!k && all) printf("%-32s %s\n", names[i], "(nothing decoded)");
    for (int j = 0; j < k; j++) {
      const wt_frame_t *f = &res.frames[j];
      if (f->kind == BYS_KIND_PAYLOAD) printf("%-32s %-8s %-8s %-9u %s\n", names[i], BAND_NAMES[f->band], PROFILE_NAMES[f->profile], f->corrected, f->text);
      else printf("%-32s %-8s %-8s %-9u <frame type '%c': ack, risk or reference>\n", names[i], BAND_NAMES[f->band], PROFILE_NAMES[f->profile], f->corrected, f->kind);
    }
    free(pcm); free(names[i]);
  }
  free(names);
  printf("\n%d of %zu file(s) with at least one frame, %d frame(s) in total\n", decoded_files, count, total_frames);
  return total_frames ? 0 : 1;
}
