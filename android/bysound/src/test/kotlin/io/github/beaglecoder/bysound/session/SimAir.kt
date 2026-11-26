package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Codec
import io.github.beaglecoder.bysound.modem.Modem
import io.github.beaglecoder.bysound.modem.Modulator
import io.github.beaglecoder.bysound.modem.Profile
import io.github.beaglecoder.bysound.modem.TestAudio
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException

/**
 * Simulated air for the session tests (web/src/session/testAir.ts): several devices, each with a speaker and a microphone, on one
 * sample clock. What each device hears is the sum of what every device plays (scaled by a gain per pair) plus white noise.
 */
class Device(val air: Air, val name: String, private var seed: Long) : Platform {
    var ears: Ears? = null
    /** What the speaker does to the sound (a broken tweeter, for example). */
    var speaker: ((FloatArray) -> FloatArray)? = null
    override val sampleRate get() = air.rate
    override fun now() = air.t.toDouble() / air.rate
    override fun after(seconds: Double, fn: () -> Unit): () -> Unit {
        val t = Air.Timer(air.t + maxOf(0L, Math.round(seconds * air.rate)), fn)
        air.timers.add(t)
        return { t.cancelled = true }
    }
    override fun play(audio: FloatArray, onEnd: () -> Unit): () -> Unit {
        val s = Air.Sound(this, speaker?.invoke(audio) ?: audio, air.t, onEnd)
        air.sounds.add(s)
        return { s.stopped = true }
    }
    override fun random(): Double {
        seed = (seed * 1103515245 + 12345) % 2147483648L
        return seed / 2147483648.0
    }
    /** Sends what this device's microphone decodes to [hear]. */
    fun listen(robust: Boolean = false, hear: (AirEvent) -> Unit) {
        ears = Ears(air.rate, { e -> hear(e) }, robust)
    }
    /** Plays raw audio in a loop for [seconds]: a device that does not follow the session protocol (a rogue sender, a replay). */
    fun loop(audio: FloatArray, seconds: Double): () -> Unit {
        var stopped = false
        val until = now() + seconds
        fun again() { if (!stopped && now() < until) play(audio) { again() } }
        again()
        return { stopped = true }
    }
}

class Air(val rate: Int = 48000, private val noiseLevel: Double = 0.003, seed: Int = 1, private val chunk: Int = 1024) {
    class Sound(val from: Device, val audio: FloatArray, val start: Long, val onEnd: () -> Unit) { var stopped = false }
    class Timer(val due: Long, val fn: () -> Unit) { var cancelled = false }

    var t = 0L
    val devices = ArrayList<Device>()
    val sounds = ArrayList<Sound>()
    val timers = ArrayList<Timer>()
    private val gains = HashMap<String, Double>()
    private val noise = TestAudio.gaussian(seed)
    val seconds get() = t.toDouble() / rate

    fun device(name: String) = Device(this, name, devices.size * 7919L + 17).also { devices.add(it) }

    /** How loud [from] is in the microphone of [to] (default 1; 0: out of range). Symmetric unless [oneWay]. */
    fun gain(from: Device, to: Device, g: Double, oneWay: Boolean = false) {
        gains["${from.name}>${to.name}"] = g
        if (!oneWay) gains["${to.name}>${from.name}"] = g
    }

    /** Advances the clock by [seconds], or until [until] holds (checked every chunk). */
    fun run(seconds: Double, until: () -> Boolean = { false }) {
        val end = t + Math.round(seconds * rate)
        while (t < end && !until()) {
            val n = minOf(chunk.toLong(), end - t).toInt()
            val from = t
            t += n
            val white = FloatArray(n) { (noise() * noiseLevel).toFloat() }
            for (d in devices) {
                val ears = d.ears ?: continue
                val x = white.copyOf()
                for (s in sounds) {
                    if (s.stopped) continue
                    val g = gains["${s.from.name}>${d.name}"] ?: 1.0
                    if (g == 0.0) continue
                    for (i in 0 until n) {
                        val k = (from + i - s.start).toInt()
                        if (k >= 0 && k < s.audio.size) x[i] += (g * s.audio[k]).toFloat()
                    }
                }
                ears.feed(x)
            }
            val snapshot = sounds.toList()
            sounds.clear()
            for (s in snapshot) {
                if (s.stopped) continue
                if (t >= s.start + s.audio.size) s.onEnd() else sounds.add(s)
            }
            val due = timers.toList().sortedBy { it.due }
            timers.clear()
            for (tm in due) {
                if (tm.cancelled) continue
                if (tm.due <= t) tm.fn() else timers.add(tm)
            }
        }
    }
}

/** A speaker that plays nothing above [cutoffHz] (an ideal low-pass, done on the whole sound at once). */
fun brokenTweeter(cutoffHz: Double, rate: Int = 48000): (FloatArray) -> FloatArray = { audio ->
    var n = 2
    while (n < audio.size) n = n shl 1
    val re = DoubleArray(n); val im = DoubleArray(n)
    for (i in audio.indices) re[i] = audio[i].toDouble()
    fft(re, im)
    val k0 = Math.ceil(cutoffHz * n / rate).toInt()
    for (k in k0..n - k0) { re[k] = 0.0; im[k] = 0.0 }
    for (k in 0 until n) im[k] = -im[k]
    fft(re, im)
    FloatArray(audio.size) { (re[it] / n).toFloat() }
}

private fun fft(re: DoubleArray, im: DoubleArray) {
    val n = re.size
    var j = 0
    for (i in 1 until n) {
        var bit = n shr 1
        while (j and bit != 0) { j = j xor bit; bit = bit shr 1 }
        j = j xor bit
        if (i < j) { val a = re[i]; re[i] = re[j]; re[j] = a; val b = im[i]; im[i] = im[j]; im[j] = b }
    }
    var len = 2
    while (len <= n) {
        val ang = -2 * Math.PI / len
        for (i in 0 until n step len) for (k in 0 until len / 2) {
            val wr = Math.cos(ang * k); val wi = Math.sin(ang * k)
            val x = i + k; val y = x + len / 2
            val tr = re[y] * wr - im[y] * wi; val ti = re[y] * wi + im[y] * wr
            re[y] = re[x] - tr; im[y] = im[x] - ti; re[x] += tr; im[x] += ti
        }
        len = len shl 1
    }
}

/** A steady interferer: equal-power tones on every grid bin from [fromHz] to [toHz] with random phases, for [seconds]. */
fun hiss(fromHz: Double, toHz: Double, amplitude: Double, seconds: Double, rate: Int = 48000, seed: Int = 5): FloatArray {
    val rnd = TestAudio.gaussian(seed)
    val out = FloatArray(Math.round(seconds * rate).toInt())
    var hz = fromHz
    while (hz <= toHz) {
        val phase = rnd() * Math.PI; val w = 2 * Math.PI * hz / rate
        for (i in out.indices) out[i] += (amplitude * Math.sin(w * i + phase)).toFloat()
        hz += 46.875
    }
    return out
}

/** One payload frame with its gap, as a rogue device would loop it. */
fun loopAudio(text: String, band: Band, rate: Int = 48000): FloatArray {
    val f = Codec.payloadFrame(text, band, Profile.NORMAL)
    return Modulator.render(f.symbols, f.channel, rate, Modem.GAP_SECONDS)
}

fun <T> CompletableFuture<T>.value(): T? = if (isDone && !isCompletedExceptionally) join() else null
fun <T> CompletableFuture<T>.error(): SessionError? =
    if (!isCompletedExceptionally) null else try { join(); null } catch (e: CompletionException) { e.cause as? SessionError } catch (e: java.util.concurrent.CancellationException) { null }
val <T> CompletableFuture<T>.settled get() = isDone
