/* Reed-Solomon over GF(2^8) (0x11D, roots alpha^0..alpha^(nsym-1)), errors and erasures. All on the stack, no malloc. */
#include "bysound.h"
#include "bys_tables.h"
#include <string.h>

#define W (BYS_RS_MAX_NSYM + 2)

static uint8_t gmul(uint8_t a, uint8_t b) { return (a && b) ? BYS_GF_EXP[BYS_GF_LOG[a] + BYS_GF_LOG[b]] : 0; }
static uint8_t ginv(uint8_t a) { return BYS_GF_EXP[255 - BYS_GF_LOG[a]]; }
static uint8_t gpow(unsigned e) { return BYS_GF_EXP[e % 255]; }

void bys_rs_encode(const uint8_t *data, size_t n, uint8_t *parity, size_t nsym) {
  uint8_t gen[W] = { 1 }; /* gen[0] = 1 is the highest-degree coefficient */
  size_t glen = 1;
  for (size_t i = 0; i < nsym; i++) { /* gen *= (x + alpha^i) */
    uint8_t r = gpow((unsigned)i);
    gen[glen] = 0;
    for (size_t j = glen; j > 0; j--) gen[j] ^= gmul(gen[j - 1], r);
    glen++;
  }
  /* gen[j] is now the coefficient of x^(nsym-j): the same one the TS uses in its long division */
  memset(parity, 0, nsym);
  for (size_t i = 0; i < n; i++) {
    uint8_t fb = data[i] ^ parity[0];
    memmove(parity, parity + 1, nsym - 1);
    parity[nsym - 1] = 0;
    if (fb) for (size_t j = 0; j < nsym; j++) parity[j] ^= gmul(gen[j + 1], fb);
  }
}

static uint8_t peval(const uint8_t *p, size_t len, uint8_t x) { /* p[0] + p[1] x + ... */
  uint8_t y = 0;
  for (size_t i = len; i-- > 0;) y = (uint8_t)(gmul(y, x) ^ p[i]);
  return y;
}

static int syndromes(const uint8_t *cw, size_t n, size_t nsym, uint8_t *s) {
  int any = 0;
  for (size_t j = 0; j < nsym; j++) {
    uint8_t x = gpow((unsigned)j), y = 0;
    for (size_t i = 0; i < n; i++) y = (uint8_t)(gmul(y, x) ^ cw[i]);
    s[j] = y;
    any |= y;
  }
  return any;
}

int bys_rs_decode(uint8_t *cw, size_t n, size_t nsym, const uint8_t *erasures, size_t nerr) {
  if (n > 255 || nsym > BYS_RS_MAX_NSYM || n <= nsym || nerr > nsym) return BYS_E_RS;
  uint8_t S[BYS_RS_MAX_NSYM];
  if (!syndromes(cw, n, nsym, S)) return 0;

  /* Erasure locator G(x) = prod (1 + X x), X = alpha^(n-1-pos); coefficients in ascending order. */
  uint8_t G[W] = { 1 };
  size_t gl = 1;
  for (size_t e = 0; e < nerr; e++) {
    if (erasures[e] >= n) return BYS_E_RS;
    uint8_t X = gpow((unsigned)(n - 1 - erasures[e]));
    G[gl] = 0;
    for (size_t j = gl; j > 0; j--) G[j] ^= gmul(G[j - 1], X);
    gl++;
  }
  /* Modified syndromes T_j = sum_k G_k S_(j+f-k), j = 0..nsym-f-1 */
  uint8_t T[BYS_RS_MAX_NSYM];
  size_t m = nsym - nerr;
  for (size_t j = 0; j < m; j++) {
    uint8_t t = 0;
    for (size_t k = 0; k <= nerr; k++) t ^= gmul(G[k], S[j + nerr - k]);
    T[j] = t;
  }
  /* Berlekamp-Massey over T */
  uint8_t C[W] = { 1 }, B[W] = { 1 }, tmp[W];
  size_t L = 0, sh = 1;
  uint8_t b = 1;
  for (size_t i = 0; i < m; i++) {
    uint8_t d = T[i];
    for (size_t j = 1; j <= L; j++) d ^= gmul(C[j], T[i - j]);
    if (!d) { sh++; continue; }
    uint8_t coef = gmul(d, ginv(b));
    int grow = 2 * L <= i;
    if (grow) memcpy(tmp, C, W);
    for (size_t j = 0; j + sh < W && j < W; j++) C[j + sh] ^= gmul(coef, B[j]);
    if (grow) { memcpy(B, tmp, W); L = i + 1 - L; b = d; sh = 1; } else sh++;
  }
  if (2 * L > m || L + nerr >= W) return BYS_E_RS;
  /* Psi = C * G */
  uint8_t P[W] = { 0 };
  size_t pl = 0; /* degree of P */
  for (size_t i = 0; i <= L; i++) for (size_t j = 0; j <= nerr; j++) P[i + j] ^= gmul(C[i], G[j]);
  for (size_t i = 0; i < W; i++) if (P[i]) pl = i;
  /* Chien search: roots X^-1 */
  uint8_t pos[BYS_RS_MAX_NSYM], val[BYS_RS_MAX_NSYM];
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned p = (unsigned)(n - 1 - i);
    if (peval(P, pl + 1, gpow(255 - p % 255)) == 0) {
      if (cnt >= BYS_RS_MAX_NSYM) return BYS_E_RS;
      pos[cnt++] = (uint8_t)i;
    }
  }
  if (cnt != pl) return BYS_E_RS;
  /* Omega = S * Psi mod x^nsym; Forney (fcr = 0): e = X * Omega(1/X) / Psi'(1/X) */
  uint8_t O[BYS_RS_MAX_NSYM];
  for (size_t i = 0; i < nsym; i++) {
    uint8_t o = 0;
    for (size_t j = 0; j <= pl && j <= i; j++) o ^= gmul(P[j], S[i - j]);
    O[i] = o;
  }
  uint8_t dP[W] = { 0 };
  for (size_t k = 1; k <= pl; k += 2) dP[k - 1] = P[k];
  for (size_t e = 0; e < cnt; e++) {
    unsigned p = (unsigned)(n - 1 - pos[e]);
    uint8_t X = gpow(p), xi = ginv(X);
    uint8_t den = peval(dP, pl, xi);
    if (!den) return BYS_E_RS;
    uint8_t num = gmul(X, peval(O, nsym, xi));
    val[e] = gmul(num, ginv(den));
  }
  for (size_t e = 0; e < cnt; e++) cw[pos[e]] ^= val[e];
  uint8_t chk[BYS_RS_MAX_NSYM];
  if (syndromes(cw, n, nsym, chk)) { /* false correction: undo it */
    for (size_t e = 0; e < cnt; e++) cw[pos[e]] ^= val[e];
    return BYS_E_RS;
  }
  int fixed = 0;
  for (size_t e = 0; e < cnt; e++) fixed += val[e] != 0;
  return fixed;
}
