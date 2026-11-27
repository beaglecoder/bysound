package io.github.beaglecoder.bysound.modem

private const val RING_SECONDS = 12
/** Interval of the noise reports ([ReceiverEvents.onNoise]). */
internal const val NOISE_REPORT_SECONDS = 0.25
/** Bands whose noise is reported: every one that lives in the high tone bank (all but AUDIBLE). */
internal val NOISE_BANDS = listOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID, Band.XHIGH, Band.WIDE)
/** Spare room in the ring for the block being processed. */
private const val RING_SLACK = 4096

/**
 * Streaming receiver of bysound v4 frames (payload, reference, ack and risk) on all seven bands at once. Feed it mono audio in
 * -1..1 at [rate] Hz in chunks of any size; verified frames come out through [events]. Not thread-safe: use one thread.
 *
 * [profile] ROBUST listens only to robust frames (bands TOP, HIGH, UPPER and MID) with a window and hop twice as long. To hear
 * both profiles run two receivers on the same audio, as [ReceiverHost] does.
 */
class Receiver(val rate: Int, val profile: Profile, private val events: ReceiverEvents) {
    /** Convenience: only frames. */
    constructor(sampleRate: Int, profile: Profile = Profile.NORMAL, onFrame: (Heard) -> Unit) :
        this(sampleRate, profile, object : ReceiverEvents { override fun onFrame(heard: Heard) = onFrame(heard) })

    internal val winSize: Int
    internal val hopSize: Int
    internal val windowSeconds: Double
    internal val hopSeconds: Double
    internal val ringSize = Math.round(RING_SECONDS.toDouble() * rate).toInt()
    internal val windowed: DoubleArray

    /** FFT of the window when the grid tones fall exactly on its bins (48 kHz); null otherwise (44.1 kHz uses Goertzel). */
    internal val fft: Fft?
    internal val binStride: Int
    private val taper: DoubleArray

    /** Total samples received so far. */
    internal var total = 0L
        private set
    private var ring: FloatArray
    private var hops = 0L
    private var sinceHop = 0
    private val highBank: ToneBank
    private val lowBank: ToneBank?
    private val readers: List<ChannelReader>
    private val pool: Pool
    private val noiseSum = DoubleArray(6 * TONES)
    private var noiseHops = 0
    private var noiseEvery = 0
    /** Energy of a full-scale tone of the grid in this window: (sum of the window / 2)^2. */
    private var fullScale = 1.0

    init {
        val scale = if (profile == Profile.ROBUST) 2 else 1
        windowSeconds = (1 / SPACING_HZ) * scale
        hopSeconds = Protocol.HOP_SECONDS * scale
        winSize = samples(windowSeconds, rate)
        hopSize = samples(hopSeconds, rate)
        ring = FloatArray(ringSize + RING_SLACK)
        windowed = DoubleArray(winSize)
        taper = SymbolWindow.of(winSize)
        fullScale = Math.pow(taper.sum() / 2, 2.0)
        noiseEvery = roundHalfUp(NOISE_REPORT_SECONDS / hopSeconds)
        val stride = winSize * SPACING_HZ / rate
        val aligned = Fft.isPowerOfTwo(winSize) && Math.abs(stride - Math.round(stride)) < 1e-9 && Math.round(stride) >= 1
        binStride = if (aligned) Math.round(stride).toInt() else 0
        fft = if (aligned && (Band.WIDE.bin + 6 * TONES) * binStride < winSize / 2) Fft(winSize) else null
        highBank = ToneBank(Band.MID.bin, 6 * TONES, rate)
        // Audible has no robust profile: the robust receiver does not compute its bank.
        lowBank = if (profile == Profile.ROBUST) null else ToneBank(Band.AUDIBLE.bin, 4 * TONES, rate)
        pool = Pool(rate, events)
        val bands = if (profile == Profile.ROBUST) Band.entries.filter { it.hasRobust } else Band.entries
        readers = bands.map { ChannelReader(this, Channel.of(it, profile), if (it == Band.AUDIBLE) lowBank!! else highBank, pool, events) }
    }

    /** Processes a chunk of audio. */
    fun feed(x: FloatArray) {
        // The block is stored at once, then the hops are advanced. While processing, the ring is ahead by up to one block.
        if (ringSize + x.size > ring.size) grow(ringSize + x.size)
        val size = ring.size
        var p = (total % size).toInt()
        for (v in x) {
            ring[p] = v
            if (++p == size) p = 0
        }
        var left = x.size
        while (left > 0) {
            val n = minOf(left.toLong(), maxOf(maxOf((hopSize - sinceHop).toLong(), winSize - total), 1L)).toInt()
            total += n
            sinceHop += n
            left -= n
            if (sinceHop >= hopSize && total >= winSize) {
                sinceHop = 0
                onHop()
            }
        }
    }

    /** A block bigger than the spare room: the ring grows keeping what is stored (sample p lives at p % size). */
    private fun grow(size: Int) {
        val old = ring
        val bigger = FloatArray(size)
        var at = maxOf(0L, total - old.size)
        while (at < total) { bigger[(at % size).toInt()] = old[(at % old.size).toInt()]; at++ }
        ring = bigger
    }

    /** Copies the window [start, start + winSize) into [windowed], with tapered edges. */
    internal fun window(start: Long) {
        val size = ring.size
        var p = (((start % size) + size) % size).toInt()
        for (n in 0 until winSize) {
            windowed[n] = ring[p] * taper[n]
            if (++p == size) p = 0
        }
    }

    private fun onHop() {
        val index = hops++
        window(index * hopSize)
        // With the FFT, one transform per hop feeds both banks.
        fft?.analyze(windowed)
        highBank.compute(this, index, fft != null)
        lowBank?.compute(this, index, fft != null)
        if (profile == Profile.NORMAL) noteNoise(highBank.at(index))
        for (r in readers) r.onHop(index)
        pool.flushHeaderFails(total)
    }

    /** Adds the energy of every high tone; every [NOISE_REPORT_SECONDS] reports the mean per band, in dB relative to a full-scale tone. */
    private fun noteNoise(e: DoubleArray) {
        for (t in noiseSum.indices) noiseSum[t] += e[t]
        if (++noiseHops < noiseEvery) return
        val out = java.util.EnumMap<Band, Double>(Band::class.java)
        for (band in NOISE_BANDS) {
            val from = band.bin - Band.MID.bin
            val n = band.groups * TONES
            var sum = 0.0
            for (t in from until from + n) sum += noiseSum[t]
            out[band] = 10 * Math.log10(sum / n / noiseHops / fullScale + 1e-30)
        }
        noiseSum.fill(0.0)
        noiseHops = 0
        events.onNoise(out)
    }

    /** Frames that were heard but could not be completed (a diagnostic counter). */
    val failures: Int get() = pool.failures

    private val active get() = readers.firstOrNull { it.state == State.BODY } ?: readers.firstOrNull { it.state == State.HEADER }

    /** Progress 0..1 of the frame being read, or -1 if none. */
    val progress: Double get() = active?.progress ?: -1.0
}
