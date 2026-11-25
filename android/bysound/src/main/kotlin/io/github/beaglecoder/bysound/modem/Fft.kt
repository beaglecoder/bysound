package io.github.beaglecoder.bysound.modem

/** Radix-2 FFT with precomputed twiddles and bit-reversal swaps; work buffers are reused (use from one thread). */
internal class Fft(val n: Int) {
    val re = DoubleArray(n)
    val im = DoubleArray(n)
    private val swaps: IntArray
    private val cos = DoubleArray(n)
    private val sin = DoubleArray(n)

    init {
        require(isPowerOfTwo(n)) { "FFT size $n must be a power of two" }
        val list = ArrayList<Int>()
        var j = 0
        for (i in 1 until n) {
            var bit = n shr 1
            while (j and bit != 0) { j = j xor bit; bit = bit shr 1 }
            j = j xor bit
            if (i < j) { list.add(i); list.add(j) }
        }
        swaps = list.toIntArray()
        var len = 2
        while (len <= n) {
            val a = -2 * Math.PI / len
            for (k in 0 until len / 2) {
                cos[len / 2 - 1 + k] = Math.cos(a * k)
                sin[len / 2 - 1 + k] = Math.sin(a * k)
            }
            len = len shl 1
        }
    }

    private fun transform() {
        var p = 0
        while (p < swaps.size) {
            val i = swaps[p]; val j = swaps[p + 1]
            val tr = re[i]; re[i] = re[j]; re[j] = tr
            val ti = im[i]; im[i] = im[j]; im[j] = ti
            p += 2
        }
        var len = 2
        while (len <= n) {
            val half = len shr 1
            val base = half - 1
            var i = 0
            while (i < n) {
                for (k in 0 until half) {
                    val wr = cos[base + k]; val wi = sin[base + k]
                    val x = i + k; val y = x + half
                    val tr = re[y] * wr - im[y] * wi
                    val ti = re[y] * wi + im[y] * wr
                    re[y] = re[x] - tr; im[y] = im[x] - ti
                    re[x] = re[x] + tr; im[x] = im[x] + ti
                }
                i += len
            }
            len = len shl 1
        }
    }

    /** Transforms the real window [x] (length n) into the internal buffers; then [energy] gives each bin. */
    fun analyze(x: DoubleArray) {
        System.arraycopy(x, 0, re, 0, n)
        java.util.Arrays.fill(im, 0.0)
        transform()
    }

    /** Energy of bin [k] of the last [analyze], on the Goertzel scale: |X(k)|^2, unnormalised. */
    fun energy(k: Int): Double = re[k] * re[k] + im[k] * im[k]

    companion object {
        fun isPowerOfTwo(n: Int) = n >= 2 && (n and (n - 1)) == 0
    }
}

/** Tukey window with a cosine edge: a lightly tapered window cuts spectral leakage from a strong nearby noise. */
internal object SymbolWindow {
    /** Fraction of the window that is tapered, split between both edges. 0 is rectangular. */
    const val TAPER_V4 = 0.1

    fun of(size: Int, taper: Double = TAPER_V4): DoubleArray {
        val edge = taper * (size - 1) / 2
        val out = DoubleArray(size) { 1.0 }
        if (edge <= 0) return out
        for (i in 0 until size) {
            val distance = minOf(i, size - 1 - i)
            if (distance < edge) out[i] = 0.5 - 0.5 * Math.cos(Math.PI * distance / edge)
        }
        return out
    }
}
