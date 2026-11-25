package io.github.beaglecoder.bysound.modem

/** Soft decisions: for every received hex digit, the probability of each of the 16 values. */
internal typealias Soft = List<DoubleArray>

internal val UNIFORM = DoubleArray(16) { 1.0 / 16 }

internal fun hard(p: DoubleArray): Int {
    var best = 0
    for (v in 1 until p.size) if (p[v] > p[best]) best = v
    return best
}

/** Difference between the two most likely values: how sure a digit is. */
internal fun margin(p: DoubleArray): Double {
    var a = -1.0
    var b = -1.0
    for (v in p) { if (v > a) { b = a; a = v } else if (v > b) b = v }
    return a - b
}

internal fun hardBytes(soft: Soft): IntArray {
    val out = IntArray(soft.size / 2)
    for (i in out.indices) out[i] = (hard(soft[2 * i]) shl 4) or hard(soft[2 * i + 1])
    return out
}

/** Sum of the readings of several laps, digit by digit. */
internal fun summed(readings: List<Soft>): Soft {
    val first = readings[0]
    return List(first.size) { i -> DoubleArray(16) { v -> var s = 0.0; for (r in readings) s += r[i][v]; s } }
}

/** For each digit, the reading with the biggest margin among the laps. */
internal fun surest(readings: List<Soft>): Soft =
    List(readings[0].size) { i ->
        var best = readings[0][i]
        for (r in readings) if (margin(r[i]) > margin(best)) best = r[i]
        best
    }

/** Byte indices ordered from least to most reliable. */
internal fun leastSureBytes(soft: Soft, bytes: Int): List<Int> {
    val sure = soft.map(::margin)
    fun byte(i: Int) = minOf(sure[2 * i], sure[2 * i + 1])
    return (0 until bytes).sortedBy { byte(it) }
}

internal fun <T> List<T>.clip(from: Int, to: Int): List<T> = subList(minOf(from, size), minOf(to, size))

/**
 * Energy of each tone in [x] with the Goertzel algorithm, for when the grid does not fall on FFT bins (44.1 kHz). Four tones per
 * pass: each does exactly the same arithmetic as alone, but the four chains advance together.
 */
internal fun goertzel(x: DoubleArray, coeffs: DoubleArray, out: DoubleArray) {
    val n = x.size
    var t = 0
    while (t + 4 <= coeffs.size) {
        val c0 = coeffs[t]; val c1 = coeffs[t + 1]; val c2 = coeffs[t + 2]; val c3 = coeffs[t + 3]
        var a1 = 0.0; var a2 = 0.0; var b1 = 0.0; var b2 = 0.0; var d1 = 0.0; var d2 = 0.0; var e1 = 0.0; var e2 = 0.0
        for (i in 0 until n) {
            val v = x[i]
            val a0 = v + c0 * a1 - a2; a2 = a1; a1 = a0
            val b0 = v + c1 * b1 - b2; b2 = b1; b1 = b0
            val d0 = v + c2 * d1 - d2; d2 = d1; d1 = d0
            val e0 = v + c3 * e1 - e2; e2 = e1; e1 = e0
        }
        out[t] = a1 * a1 + a2 * a2 - c0 * a1 * a2
        out[t + 1] = b1 * b1 + b2 * b2 - c1 * b1 * b2
        out[t + 2] = d1 * d1 + d2 * d2 - c2 * d1 * d2
        out[t + 3] = e1 * e1 + e2 * e2 - c3 * e1 * e2
        t += 4
    }
    while (t < coeffs.size) {
        val c = coeffs[t]
        var s1 = 0.0; var s2 = 0.0
        for (i in 0 until n) { val s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0 }
        out[t] = s1 * s1 + s2 * s2 - c * s1 * s2
        t++
    }
}

/**
 * Tone energies of the grid in the window [x]: with the FFT if the tones fall exactly on its bins (at 48 kHz the 1/46.875 s window
 * is 1024 samples and grid tone b is FFT bin b), otherwise with Goertzel. Same window, same scale (|X(k)|^2).
 */
internal fun toneEnergies(r: Receiver, x: DoubleArray, firstBin: Int, coeffs: DoubleArray, out: DoubleArray, analyzed: Boolean = false) {
    val fft = r.fft
    if (fft == null) return goertzel(x, coeffs, out)
    if (!analyzed) fft.analyze(x)
    val stride = r.binStride
    for (t in out.indices) out[t] = fft.energy((firstBin + t) * stride)
}

private const val HOP_HISTORY = 160

/** Energies of a grid of tones at every search hop: one pass for all the channels of that zone. */
internal class ToneBank(val baseBin: Int, tones: Int, rate: Int) {
    private val coeffs = DoubleArray(tones) { 2 * Math.cos(2 * Math.PI * (baseBin + it) * SPACING_HZ / rate) }
    private val history = Array(HOP_HISTORY) { DoubleArray(tones) }

    /** [analyzed]: the FFT of the window is already done (both banks share it). */
    fun compute(r: Receiver, hop: Int, analyzed: Boolean) = toneEnergies(r, r.windowed, baseBin, coeffs, history[hop % HOP_HISTORY], analyzed)
    fun at(hop: Int): DoubleArray = history[hop % HOP_HISTORY]
}

/** Blocks of the text being recovered, accumulated across laps. */
internal class BlockAssembly {
    var total = 0
    var chunks = HashMap<Int, IntArray>()
    var pending = HashMap<Int, List<Soft>>()
    var pendingLast: List<Soft> = emptyList()
    var loops = 0
    var lastAt = 0L
    var channel: Band? = null

    /** Bytes of the body of the lap that closed the last block: the header does not say. */
    var knownBytes = 0

    /** The last block closed without the other blocks, so the CRC-32 could not confirm its length (see ChannelReader.lastBlockCheck). */
    var lastUnchecked = false

    fun begin(total: Int, at: Long, horizon: Long, channel: Band) {
        if (total != this.total || lastAt < horizon) reset()
        this.total = total
        lastAt = at
        this.channel = channel
    }

    fun reset() {
        total = 0
        chunks = HashMap()
        pending = HashMap()
        pendingLast = emptyList()
        loops = 0
        knownBytes = 0
        lastUnchecked = false
    }
}

private const val SELF_ECHO_SECONDS = 0.3
private const val OVERLAP_MARGIN_SECONDS = 0.15
private const val HEADER_FAIL_WAIT_SECONDS = 0.3
private const val HEADER_FAIL_NEAR_SECONDS = 0.15

/** State shared by the channel readers of one receiver: frame starts (for clash detection), header failures and blocks. */
internal class Pool(private val rate: Int, private val events: ReceiverEvents) {
    class Start(val band: Band, val at: Long, val span: Long)
    class HeaderFail(val at: Long, val noted: Long)

    val starts = ArrayList<Start>()
    val blocks = BlockAssembly()
    var headerFails = ArrayList<HeaderFail>()
    var failures = 0

    fun clash() = events.onClash()

    fun noteStart(band: Band, at: Long, span: Long) {
        val selfEcho = SELF_ECHO_SECONDS * rate
        val margin = OVERLAP_MARGIN_SECONDS * rate
        val overlapped = starts.any { o ->
            val apart = Math.abs(at - o.at)
            (apart >= selfEcho || o.band != band) && apart < (if (o.at < at) o.span else span) - margin
        }
        starts.add(Start(band, at, span))
        if (starts.size > 32) starts.removeAt(0)
        if (overlapped) clash()
    }

    /** Waits a little: the real channel may read its header in the same hop or right after. */
    fun flushHeaderFails(total: Long) {
        if (headerFails.isEmpty()) return
        val due = total - HEADER_FAIL_WAIT_SECONDS * rate
        val near = HEADER_FAIL_NEAR_SECONDS * rate
        val kept = ArrayList<HeaderFail>()
        for (f in headerFails) {
            if (f.noted > due) { kept.add(f); continue }
            if (starts.none { s -> f.at >= s.at - near && f.at <= s.at + s.span }) failures++
        }
        headerFails = kept
    }
}
