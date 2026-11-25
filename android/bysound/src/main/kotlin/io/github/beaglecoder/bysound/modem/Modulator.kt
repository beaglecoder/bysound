package io.github.beaglecoder.bysound.modem

/** Audio synthesis of a frame: one tone per group, a 3 ms raised-cosine ramp at each symbol edge and a silent gap at the end. */
internal object Modulator {
    fun render(symbols: Array<IntArray>, ch: Channel, rate: Int, gapSeconds: Double): FloatArray {
        val out = FloatArray(ch.symbolStart(symbols.size, rate) + samples(gapSeconds, rate))
        val ramp = samples(Protocol.RAMP_SECONDS, rate)
        // Each tone is computed once for the longest symbol and reused: a frame repeats the same 16 tones per group in every symbol.
        var longest = 0
        for (k in symbols.indices) longest = maxOf(longest, ch.symbolStart(k + 1, rate) - ch.symbolStart(k, rate))
        val sines = HashMap<Int, DoubleArray>()
        fun sine(g: Int, t: Int): DoubleArray = sines.getOrPut(g * TONES + t) {
            val omega = 2 * Math.PI * ch.toneHz(g, t) / rate
            DoubleArray(longest) { Math.sin(omega * it) }
        }
        for (k in symbols.indices) {
            val start = ch.symbolStart(k, rate)
            val len = ch.symbolStart(k + 1, rate) - start
            val waves = Array(symbols[k].size) { sine(it, symbols[k][it]) }
            for (n in 0 until len) {
                var env = 1.0
                if (n < ramp) env = 0.5 * (1 - Math.cos(Math.PI * n / ramp))
                else if (n >= len - ramp) env = 0.5 * (1 - Math.cos(Math.PI * (len - 1 - n) / ramp))
                var s = 0.0
                for (w in waves) s += w[n]
                out[start + n] = (s * env * ch.amp).toFloat()
            }
        }
        return out
    }

    /** Scales down only if the peak exceeds 1 (clipping dirties neighbouring tones), then applies [level]. */
    fun fitLevel(audio: FloatArray, level: Float): FloatArray {
        var peak = 0f
        for (v in audio) peak = maxOf(peak, Math.abs(v))
        val gain = level * (if (peak > 1f) 1f / peak else 1f)
        if (gain == 1f) return audio
        return FloatArray(audio.size) { audio[it] * gain }
    }
}
