package io.github.beaglecoder.bysound.modem

import com.google.gson.JsonParser
import java.io.File

/** Helpers for the tests: synthetic air and the repository files. */
object TestAudio {
    val root: File = File(".").absoluteFile.normalize()

    fun gaussian(seed: Int): () -> Double {
        var s = seed
        fun u(): Double {
            s += 0x6d2b79f5
            var t = (s xor (s ushr 15)) * (1 or s)
            t = (t + (t xor (t ushr 7)) * (61 or t)) xor t
            return ((t xor (t ushr 14)).toLong() and 0xffffffffL) / 4294967296.0
        }
        return { Math.sqrt(-2 * Math.log(1 - u())) * Math.cos(2 * Math.PI * u()) }
    }

    /** Joins the laps after [lead] seconds of silence and adds echo, gain and white noise. */
    fun air(laps: List<FloatArray>, rate: Int, noise: Double, gain: Double = 1.0, echo: Double = 0.0, lead: Double = 0.5, seed: Int = 1): FloatArray {
        val l = Math.round(lead * rate).toInt()
        val out = FloatArray(l + laps.sumOf { it.size } + Math.round(0.3 * rate).toInt())
        var at = l
        for (lap in laps) { lap.copyInto(out, at); at += lap.size }
        val delay = Math.round(0.005 * rate).toInt()
        if (echo != 0.0) for (i in out.size - 1 downTo delay) out[i] += (echo * out[i - delay]).toFloat()
        val n = gaussian(seed)
        for (i in out.indices) out[i] = (out[i] * gain + n() * noise).toFloat()
        return out
    }

    /** Runs a host over [x] in 1024-sample chunks and returns what it heard. */
    fun listen(x: FloatArray, rate: Int, robust: Boolean = true): List<Heard> {
        val heard = ArrayList<Heard>()
        val host = ReceiverHost(rate, null, robust) { heard.add(it) }
        var i = 0
        while (i < x.size) { host.feed(x.copyOfRange(i, minOf(i + 1024, x.size))); i += 1024 }
        return heard
    }

    fun pcm16(file: File, skip: Int = 0): FloatArray {
        val b = file.readBytes()
        return FloatArray((b.size - skip) / 2) { (((b[skip + 2 * it].toInt() and 0xff) or (b[skip + 2 * it + 1].toInt() shl 8)).toShort()) / 32768f }
    }

    fun vectors() = JsonParser.parseString(File(root, "test-vectors/v4-frames.json").readText()).asJsonObject.getAsJsonArray("vectors").map { it.asJsonObject }
}
