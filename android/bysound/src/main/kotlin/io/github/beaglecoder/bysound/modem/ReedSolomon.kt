package io.github.beaglecoder.bysound.modem

/** Result of a successful Reed-Solomon decode: the data bytes and how many bytes of the codeword were changed. */
class RsResult(val data: IntArray, val corrected: Int)

/**
 * Reed-Solomon over GF(2^8) (primitive polynomial 0x11D, generator 2, first root 0) with error and erasure decoding.
 * With `e` errors and `f` erasures it corrects when `2e + f <= nsym`. Bytes are ints in 0..255.
 */
object ReedSolomon {
    private val EXP = IntArray(512)
    private val LOG = IntArray(256)

    init {
        var x = 1
        for (i in 0 until 255) {
            EXP[i] = x
            LOG[x] = i
            x = x shl 1
            if (x and 0x100 != 0) x = x xor 0x11d
        }
        for (i in 255 until 512) EXP[i] = EXP[i - 255]
    }

    private fun mul(a: Int, b: Int) = if (a == 0 || b == 0) 0 else EXP[LOG[a] + LOG[b]]
    private fun div(a: Int, b: Int) = if (a == 0) 0 else EXP[(LOG[a] + 255 - LOG[b]) % 255]
    private fun pow(a: Int, n: Int): Int {
        val e = (LOG[a] * n) % 255
        return EXP[if (e < 0) e + 255 else e]
    }
    private fun inverse(a: Int) = EXP[255 - LOG[a]]
    private fun scale(p: IntArray, x: Int) = IntArray(p.size) { mul(p[it], x) }

    private fun add(p: IntArray, q: IntArray): IntArray {
        val r = IntArray(maxOf(p.size, q.size))
        for (i in p.indices) r[i + r.size - p.size] = p[i]
        for (i in q.indices) r[i + r.size - q.size] = r[i + r.size - q.size] xor q[i]
        return r
    }

    private fun mulPoly(p: IntArray, q: IntArray): IntArray {
        val r = IntArray(p.size + q.size - 1)
        for (j in q.indices) for (i in p.indices) r[i + j] = r[i + j] xor mul(p[i], q[j])
        return r
    }

    private fun eval(p: IntArray, x: Int): Int {
        var y = p[0]
        for (i in 1 until p.size) y = mul(y, x) xor p[i]
        return y
    }

    private val generators = HashMap<Int, IntArray>()
    private fun generator(nsym: Int): IntArray = synchronized(generators) {
        generators.getOrPut(nsym) {
            var g = intArrayOf(1)
            for (i in 0 until nsym) g = mulPoly(g, intArrayOf(1, pow(2, i)))
            g
        }
    }

    /** `data` followed by `nsym` parity bytes. */
    fun encode(data: IntArray, nsym: Int): IntArray {
        require(data.size + nsym <= 255) { "Reed-Solomon block too long" }
        val gen = generator(nsym)
        val out = data.copyOf(data.size + nsym)
        for (i in data.indices) {
            val coef = out[i]
            if (coef != 0) for (j in 1 until gen.size) out[i + j] = out[i + j] xor mul(gen[j], coef)
        }
        // The division works in place and spoils the data part of `out`: keep the original data and take only the parity.
        val result = data.copyOf(data.size + nsym)
        System.arraycopy(out, data.size, result, data.size, nsym)
        return result
    }

    private fun syndromes(msg: IntArray, nsym: Int): IntArray {
        val s = IntArray(nsym + 1)
        for (i in 0 until nsym) s[i + 1] = eval(msg, pow(2, i))
        return s
    }

    private fun forneySyndromes(synd: IntArray, erasures: IntArray, n: Int): IntArray {
        val f = synd.copyOfRange(1, synd.size)
        for (p in erasures) {
            val x = pow(2, n - 1 - p)
            for (j in 0 until f.size - 1) f[j] = mul(f[j], x) xor f[j + 1]
        }
        return f
    }

    private fun errorLocator(synd: IntArray, nsym: Int, erasures: Int = 0): IntArray? {
        var errLoc = intArrayOf(1)
        var oldLoc = intArrayOf(1)
        val shift = synd.size - nsym
        for (i in 0 until nsym - erasures) {
            val k = i + shift
            var delta = synd[k]
            for (j in 1 until errLoc.size) delta = delta xor mul(errLoc[errLoc.size - 1 - j], synd[k - j])
            oldLoc += 0
            if (delta != 0) {
                if (oldLoc.size > errLoc.size) {
                    val newLoc = scale(oldLoc, delta)
                    oldLoc = scale(errLoc, inverse(delta))
                    errLoc = newLoc
                }
                errLoc = add(errLoc, scale(oldLoc, delta))
            }
        }
        var start = 0
        while (start < errLoc.size && errLoc[start] == 0) start++
        errLoc = errLoc.copyOfRange(start, errLoc.size)
        return if ((errLoc.size - 1) * 2 + erasures > nsym) null else errLoc
    }

    private fun findErrors(reversed: IntArray, n: Int): IntArray? {
        val pos = ArrayList<Int>()
        for (i in 0 until n) if (eval(reversed, pow(2, i)) == 0) pos.add(n - 1 - i)
        return if (pos.size == reversed.size - 1) pos.toIntArray() else null
    }

    private fun divRemainder(dividend: IntArray, divisor: IntArray): IntArray {
        val out = dividend.copyOf()
        for (i in 0 until dividend.size - (divisor.size - 1)) {
            val coef = out[i]
            if (coef != 0) for (j in 1 until divisor.size) if (divisor[j] != 0) out[i + j] = out[i + j] xor mul(divisor[j], coef)
        }
        return out.copyOfRange(out.size - (divisor.size - 1), out.size)
    }

    private fun correctErrata(msg: IntArray, synd: IntArray, errPos: IntArray): IntArray {
        val coefPos = IntArray(errPos.size) { msg.size - 1 - errPos[it] }
        var errLoc = intArrayOf(1)
        for (p in coefPos) errLoc = mulPoly(errLoc, add(intArrayOf(1), intArrayOf(pow(2, p), 0)))
        val divisor = IntArray(errLoc.size + 1).also { it[0] = 1 }
        val errEval = divRemainder(mulPoly(synd.reversedArray(), errLoc), divisor).reversedArray()
        val x = IntArray(coefPos.size) { pow(2, -(255 - coefPos[it])) }
        val e = IntArray(msg.size)
        for (i in x.indices) {
            val xiInv = inverse(x[i])
            var prime = 1
            for (j in x.indices) if (j != i) prime = mul(prime, 1 xor mul(xiInv, x[j]))
            val y = mul(x[i], eval(errEval.reversedArray(), xiInv))
            if (prime == 0) return msg
            e[errPos[i]] = div(y, prime)
        }
        return IntArray(msg.size) { msg[it] xor e[it] }
    }

    /** Corrected data and number of bytes touched, or null if the codeword cannot be repaired. */
    fun decode(codeword: IntArray, nsym: Int, erasures: IntArray = IntArray(0)): RsResult? {
        val synd = syndromes(codeword, nsym)
        val dataSize = codeword.size - nsym
        if (synd.all { it == 0 }) return RsResult(codeword.copyOf(dataSize), 0)
        if (erasures.size > nsym) return null
        val errPos: IntArray
        if (erasures.isEmpty()) {
            val loc = errorLocator(synd, nsym) ?: return null
            errPos = findErrors(loc.reversedArray(), codeword.size) ?: return null
        } else {
            val loc = errorLocator(forneySyndromes(synd, erasures, codeword.size), nsym, erasures.size) ?: return null
            val found = findErrors(loc.reversedArray(), codeword.size) ?: return null
            errPos = erasures + found.filter { it !in erasures }
        }
        val fixed = correctErrata(codeword, synd, errPos)
        if (!syndromes(fixed, nsym).all { it == 0 }) return null
        var changed = 0
        for (i in fixed.indices) if (fixed[i] != codeword[i]) changed++
        return RsResult(fixed.copyOf(dataSize), changed)
    }
}
