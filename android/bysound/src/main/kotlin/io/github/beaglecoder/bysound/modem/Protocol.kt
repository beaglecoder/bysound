package io.github.beaglecoder.bysound.modem

/** Frame profile. [ROBUST] doubles symbol, window and hop times (bands TOP, HIGH, UPPER and MID only) for about 3 dB of margin. */
enum class Profile { NORMAL, ROBUST }

/**
 * A frequency band of bysound v4. Every band is a set of [groups] groups of 16 tones spaced 46.875 Hz; one hex digit per group
 * is sent per symbol. [bin] is the index of the first tone in units of the tone spacing.
 */
enum class Band(
    val id: String,
    internal val bin: Int,
    internal val groups: Int,
    internal val amp: Double,
    internal val sym: Double,
    internal val windowStart: Double,
    internal val channelId: Int,
    internal val normalKey: IntArray,
    internal val robustKey: IntArray?,
) {
    TOP("top", 384, 2, 0.3, 0.032, 0.004, 0, intArrayOf(0, 3), intArrayOf(1, 13)),
    HIGH("high", 368, 3, 0.3, 0.032, 0.004, 1, intArrayOf(3, 9), intArrayOf(3, 11)),
    UPPER("upper", 352, 3, 0.3, 0.032, 0.004, 2, intArrayOf(2, 14), intArrayOf(0, 8)),
    MID("mid", 320, 3, 0.3, 0.032, 0.004, 3, intArrayOf(1, 13), intArrayOf(2, 9)),
    AUDIBLE("audible", 40, 4, 0.2, 0.04, 0.005, 4, intArrayOf(0, 0), null),
    XHIGH("xhigh", 352, 4, 0.25, 0.032, 0.004, 5, intArrayOf(1, 5), null),
    WIDE("wide", 320, 6, 0.2, 0.032, 0.004, 6, intArrayOf(3, 14), null);

    /** True if the band has a [Profile.ROBUST] copy. */
    val hasRobust: Boolean get() = robustKey != null

    /** First tone frequency of the band in Hz. */
    val baseHz: Double get() = bin * SPACING_HZ
}

/** Constants of bysound v4 (see `protocol-v4.json`). Only what payload frames need. */
internal object Protocol {
    const val VERSION = 4
    const val GAP_SECONDS = 0.35
    /** Hard cap: one transmission (preamble + header + body + gap) never lasts longer than this. */
    const val MAX_FRAME_SECONDS = 30
    const val RAMP_SECONDS = 0.003
    const val HOP_SECONDS = 0.004
    const val PREAMBLE_THRESHOLD = 0.32

    const val BLOCK_DATA = 48
    const val BLOCK_PARITY = 12
    const val BLOCK_HEADER = 2
    const val BLOCK_BYTES = BLOCK_HEADER + BLOCK_DATA + BLOCK_PARITY
    const val MAX_BLOCKS = 255
    const val HEADER_BYTES = 4

    const val MAGIC_PAYLOAD = 0x56
    const val MAGIC_REFERENCE = 0x52
    const val MAGIC_RISK = 0x58
    const val MAGIC_ACK = 0x41

    /** Header lengths and body sizes of the frame types this library does not decode (they are recognised and skipped). */
    const val REF_DATA = 14
    const val REF_BODY = 30
    const val RISK_LENGTH = 10
    const val RISK_BODY = 18
    const val ACK_LENGTH = 7
    const val ACK_BODY = 15

    val PREAMBLE: Array<IntArray> = arrayOf(
        intArrayOf(0, 8, 4, 12), intArrayOf(15, 7, 11, 3), intArrayOf(2, 10, 6, 14), intArrayOf(13, 5, 9, 1),
        intArrayOf(4, 12, 0, 8), intArrayOf(11, 3, 15, 7), intArrayOf(6, 14, 2, 10), intArrayOf(9, 1, 13, 5),
    )

    /** CRC-8 of the header: polynomial 0x2F, init 0xFF. */
    fun crc8(data: IntArray): Int {
        var c = 0xff
        for (byte in data) {
            c = c xor byte
            repeat(8) { c = if (c and 0x80 != 0) ((c shl 1) xor 0x2f) and 0xff else (c shl 1) and 0xff }
        }
        return c
    }

    /** Header: `[type, length, CRC-8, version | channel | set]`. The CRC also covers the profile (0 normal, 1 robust), which is not sent. */
    fun headerBytes(magic: Int, length: Int, band: Band, set: Int, profile: Profile): IntArray {
        val version = (VERSION shl 4) or (band.channelId shl 1) or (set and 1)
        val p = if (profile == Profile.ROBUST) 1 else 0
        return intArrayOf(magic, length and 255, crc8(intArrayOf(magic, length and 255, version, p)), version)
    }
}

/** Spacing between tones in Hz. */
const val SPACING_HZ = 46.875

/** Tones per group (one hex digit). */
const val TONES = 16

/** A [Band] with the timing of a [Profile]: robust doubles the symbol and the start of the analysis window. */
internal class Channel private constructor(val band: Band, val profile: Profile) {
    val groups = band.groups
    val bin = band.bin
    val amp = band.amp
    val sym = if (profile == Profile.ROBUST) band.sym * 2 else band.sym
    val windowStart = if (profile == Profile.ROBUST) band.windowStart * 2 else band.windowStart

    fun symbolStart(k: Int, rate: Int): Int = roundHalfUp(k * sym * rate)
    fun toneHz(group: Int, tone: Int): Double = (bin + group * TONES + tone) * SPACING_HZ

    /** The preamble symbol k of this channel: each channel and profile has its own pattern. */
    fun preambleSymbol(k: Int): IntArray {
        val key = (if (profile == Profile.ROBUST) band.robustKey else null) ?: band.normalKey
        return IntArray(groups) { g -> (Protocol.PREAMBLE[k][(g + key[0]) % 4] + key[1]) and 15 }
    }

    companion object {
        /** Robust applies only to bands that have it; the others stay normal. */
        fun of(band: Band, profile: Profile): Channel =
            Channel(band, if (profile == Profile.ROBUST && band.hasRobust) Profile.ROBUST else Profile.NORMAL)
    }
}

internal fun samples(seconds: Double, rate: Int): Int = roundHalfUp(seconds * rate)

/** JavaScript `Math.round`: halves go up. */
internal fun roundHalfUp(x: Double): Int = Math.floor(x + 0.5).toInt()
internal fun roundHalfUpLong(x: Double): Long = Math.floor(x + 0.5).toLong()
