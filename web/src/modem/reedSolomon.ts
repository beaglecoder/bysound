// Reed-Solomon over GF(2^8) (0x11D, generator 2, fcr 0), with errors and erasures. Port of ReedSolomon.swift (ios/Sources/BySound).
const EXP = new Array<number>(512).fill(0);
const LOG = new Array<number>(256).fill(0);
{
  let x = 1;
  for (let i = 0; i < 255; i++) { EXP[i] = x; LOG[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
  for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255]!;
}
const mul = (a: number, b: number): number => (a === 0 || b === 0 ? 0 : EXP[LOG[a]! + LOG[b]!]!);
const div = (a: number, b: number): number => (a === 0 ? 0 : EXP[(LOG[a]! + 255 - LOG[b]!) % 255]!);
const pow = (a: number, n: number): number => { const e = (LOG[a]! * n) % 255; return EXP[e < 0 ? e + 255 : e]!; };
const inverse = (a: number): number => EXP[255 - LOG[a]!]!;
const polyScale = (p: number[], x: number) => p.map((v) => mul(v, x));

function polyAdd(p: number[], q: number[]): number[] {
  const r = new Array<number>(Math.max(p.length, q.length)).fill(0);
  p.forEach((v, i) => { r[i + r.length - p.length] = v; });
  q.forEach((v, i) => { r[i + r.length - q.length]! ^= v; });
  return r;
}
function polyMul(p: number[], q: number[]): number[] {
  const r = new Array<number>(p.length + q.length - 1).fill(0);
  for (let j = 0; j < q.length; j++) for (let i = 0; i < p.length; i++) r[i + j]! ^= mul(p[i]!, q[j]!);
  return r;
}
function polyEval(p: number[], x: number): number {
  let y = p[0]!;
  for (let i = 1; i < p.length; i++) y = mul(y, x) ^ p[i]!;
  return y;
}
const generators = new Map<number, number[]>();
function generator(nsym: number): number[] {
  let g = generators.get(nsym);
  if (!g) {
    g = [1];
    for (let i = 0; i < nsym; i++) g = polyMul(g, [1, pow(2, i)]);
    generators.set(nsym, g);
  }
  return g;
}

export function rsEncode(data: number[], nsym: number): number[] {
  if (data.length + nsym > 255) throw new Error('RS block too long');
  const gen = generator(nsym);
  const out = data.concat(new Array<number>(nsym).fill(0));
  for (let i = 0; i < data.length; i++) {
    const coef = out[i];
    if (coef) for (let j = 1; j < gen.length; j++) out[i + j]! ^= mul(gen[j]!, coef);
  }
  return data.concat(out.slice(data.length));
}

function syndromes(msg: number[], nsym: number): number[] {
  const s = new Array<number>(nsym + 1).fill(0);
  for (let i = 0; i < nsym; i++) s[i + 1] = polyEval(msg, pow(2, i));
  return s;
}
function forneySyndromes(synd: number[], erasures: number[], n: number): number[] {
  const f = synd.slice(1);
  for (const p of erasures) {
    const x = pow(2, n - 1 - p);
    for (let j = 0; j < f.length - 1; j++) f[j] = mul(f[j]!, x!) ^ f[j + 1]!;
  }
  return f;
}
function errorLocator(synd: number[], nsym: number, erasures = 0): number[] | null {
  let errLoc = [1];
  let oldLoc = [1];
  const shift = synd.length - nsym;
  for (let i = 0; i < nsym - erasures; i++) {
    const k = i + shift;
    let delta = synd[k]!;
    for (let j = 1; j < errLoc.length; j++) delta ^= mul(errLoc[errLoc.length - 1 - j]!, synd[k - j]!);
    oldLoc.push(0);
    if (delta !== 0) {
      if (oldLoc.length > errLoc.length) {
        const newLoc = polyScale(oldLoc, delta);
        oldLoc = polyScale(errLoc, inverse(delta));
        errLoc = newLoc;
      }
      errLoc = polyAdd(errLoc, polyScale(oldLoc, delta));
    }
  }
  while (errLoc.length && errLoc[0] === 0) errLoc.shift();
  return (errLoc.length - 1) * 2 + erasures > nsym ? null : errLoc;
}
function findErrors(reversed: number[], n: number): number[] | null {
  const pos: number[] = [];
  for (let i = 0; i < n; i++) if (polyEval(reversed, pow(2, i)) === 0) pos.push(n - 1 - i);
  return pos.length === reversed.length - 1 ? pos : null;
}
function polyDivRemainder(dividend: number[], divisor: number[]): number[] {
  const out = dividend.slice();
  for (let i = 0; i < dividend.length - (divisor.length - 1); i++) {
    const coef = out[i];
    if (coef) for (let j = 1; j < divisor.length; j++) if (divisor[j]) out[i + j]! ^= mul(divisor[j]!, coef);
  }
  return out.slice(out.length - (divisor.length - 1));
}
function correctErrata(msg: number[], synd: number[], errPos: number[]): number[] {
  const coefPos = errPos.map((p) => msg.length - 1 - p);
  let errLoc = [1];
  for (const p of coefPos) errLoc = polyMul(errLoc, polyAdd([1], [pow(2, p), 0]));
  const divisor = new Array<number>(errLoc.length + 1).fill(0);
  divisor[0] = 1;
  const errEval = polyDivRemainder(polyMul(synd.slice().reverse(), errLoc), divisor).reverse();
  const x = coefPos.map((p) => pow(2, -(255 - p)));
  const e = new Array<number>(msg.length).fill(0);
  for (let i = 0; i < x.length; i++) {
    const xiInv = inverse(x[i]!);
    let prime = 1;
    for (let j = 0; j < x.length; j++) if (j !== i) prime = mul(prime, 1 ^ mul(xiInv, x[j]!));
    const y = mul(x[i]!, polyEval(errEval.slice().reverse(), xiInv));
    if (prime === 0) return msg;
    e[errPos[i]!] = div(y, prime);
  }
  return msg.map((v, i) => v ^ e[i]!);
}

/** Corrected data and number of bytes touched, or null. With e errors and f erasures it corrects if 2e + f <= nsym. */
export function rsDecode(codeword: number[], nsym: number, erasures: number[] = []): { data: number[]; corrected: number } | null {
  const synd = syndromes(codeword, nsym);
  const dataSize = codeword.length - nsym;
  if (synd.every((v) => v === 0)) return { data: codeword.slice(0, dataSize), corrected: 0 };
  if (erasures.length > nsym) return null;
  let errPos: number[];
  if (erasures.length === 0) {
    const loc = errorLocator(synd, nsym);
    const found = loc && findErrors(loc.slice().reverse(), codeword.length);
    if (!found) return null;
    errPos = found;
  } else {
    const loc = errorLocator(forneySyndromes(synd, erasures, codeword.length), nsym, erasures.length);
    const found = loc && findErrors(loc.slice().reverse(), codeword.length);
    if (!found) return null;
    errPos = erasures.concat(found.filter((p) => !erasures.includes(p)));
  }
  const fixed = correctErrata(codeword, synd, errPos);
  if (!syndromes(fixed, nsym).every((v) => v === 0)) return null;
  return { data: fixed.slice(0, dataSize), corrected: fixed.filter((v, i) => v !== codeword[i]).length };
}
