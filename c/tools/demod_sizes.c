/* Prints sizeof(bys_demod_t) for the build configuration (called by `make sizes`). */
#include "bysound.h"
#include <stdio.h>
int main(int argc, char **argv) {
  printf("engine %-8s max rate %s robust %s audible %s: sizeof(bys_demod_t) = %6zu B  (ring %u samples, history %u x %u tones, bys_rx_t x7 = %zu B)\n",
         argc > 1 ? argv[1] : "?", argc > 2 ? argv[2] : "?", argc > 3 ? argv[3] : "?", argc > 4 ? argv[4] : "?", sizeof(bys_demod_t),
         (unsigned)BYS_DEMOD_RING, (unsigned)BYS_DEMOD_HIST, (unsigned)BYS_DEMOD_BANK_TONES, 7 * sizeof(bys_rx_t));
  return 0;
}
