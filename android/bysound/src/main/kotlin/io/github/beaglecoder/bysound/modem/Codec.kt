package io.github.beaglecoder.bysound.modem

import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_BYTES
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_DATA
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_HEADER
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_PARITY
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_PAYLOAD
import io.github.beaglecoder.bysound.modem.Protocol.MAX_BLOCKS

/** A built payload frame: header and body bytes, the symbol table (hex digit per group) and the duration of one lap. */
class Frame internal constructor(
    val header: IntArray,
    val body: IntArray,
    val symbols: Array<IntArray>,
    val frameSeconds: Double,
    internal val channel: Channel,
)

internal class Block(val index: Int, val total: Int, val chunk: IntArray)
internal class DecodedBlock(val block: Block, val corrected: Int)

/** The text does not fit in one transmission of at most [Modem.MAX_FRAME_SECONDS] seconds. [maxPackedBytes] is the limit for the band and profile. */
class TextTooLongException(val packedBytes: Int, val maxPackedBytes: Int, message: String) : IllegalArgumentException(message)

/** Payload frames: text packed with a CRC-32 and split into Reed-Solomon blocks of 48 data bytes. */
internal object Codec {
    /** Bytes as hex digits (high nibble first), padded with zeros to a whole number of groups. */
    fun toSymbols(bytes: IntArray, groups: Int): Array<IntArray> {
        val nibbles = ArrayList<Int>(bytes.size * 2 + groups)
        for (b in bytes) { nibbles.add(b shr 4); nibbles.add(b and 15) }
        while (nibbles.size % groups != 0) nibbles.add(0)
        return Array(nibbles.size / groups) { r -> IntArray(groups) { nibbles[r * groups + it] } }
    }

    /** Exact duration in microseconds of the transmission (frame + gap) of a payload with [packed] packed bytes (before the CRC-32). */
    fun transmissionMicros(ch: Channel, packed: Int): Long {
        val len = packed + 4
        val total = (len + BLOCK_DATA - 1) / BLOCK_DATA
        val body = len + total * (BLOCK_HEADER + BLOCK_PARITY)
        val g = ch.groups
        val symbols = Protocol.PREAMBLE.size + (2 * Protocol.HEADER_BYTES + g - 1) / g + (2 * body + g - 1) / g
        return symbols.toLong() * roundHalfUpLong(ch.sym * 1e6) + roundHalfUpLong(Protocol.GAP_SECONDS * 1e6)
    }

    fun maxPayloadBytes(band: Band, requested: Profile): Int {
        val ch = Channel.of(band, requested)
        val cap = Protocol.MAX_FRAME_SECONDS * 1_000_000L
        var lo = 0
        var hi = BLOCK_DATA * MAX_BLOCKS - 4
        if (transmissionMicros(ch, hi) <= cap) return hi
        while (hi - lo > 1) {
            val mid = (lo + hi) / 2
            if (transmissionMicros(ch, mid) <= cap) lo = mid else hi = mid
        }
        return lo
    }

    fun payloadFrame(text: String, band: Band, requested: Profile): Frame {
        val content = TextPacking.pack(text)
        val c = Checksums.crc32(content)
        val data = content + intArrayOf(((c ushr 24) and 255).toInt(), ((c ushr 16) and 255).toInt(), ((c ushr 8) and 255).toInt(), (c and 255).toInt())
        val total = (data.size + BLOCK_DATA - 1) / BLOCK_DATA
        require(total <= MAX_BLOCKS) { "The text is too long to send by sound" }
        val ch = Channel.of(band, requested)
        if (transmissionMicros(ch, content.size) > Protocol.MAX_FRAME_SECONDS * 1_000_000L) {
            val max = maxPayloadBytes(band, requested)
            throw TextTooLongException(content.size, max, "The text is too long: one transmission cannot last more than ${Protocol.MAX_FRAME_SECONDS} s (maximum for ${band.id}/${ch.profile.name.lowercase()}: $max packed bytes, you have ${content.size})")
        }
        var body = IntArray(0)
        for (b in 0 until total) {
            val chunk = data.copyOfRange(b * BLOCK_DATA, minOf((b + 1) * BLOCK_DATA, data.size))
            body += ReedSolomon.encode(intArrayOf(b, total) + chunk, BLOCK_PARITY)
        }
        val header = Protocol.headerBytes(MAGIC_PAYLOAD, total, band, 0, ch.profile)
        val symbols = Array(Protocol.PREAMBLE.size) { ch.preambleSymbol(it) } + toSymbols(header, ch.groups) + toSymbols(body, ch.groups)
        return Frame(header, body, symbols, symbols.size * ch.sym, ch)
    }

    fun decodeBlock(bytes: IntArray, erasures: IntArray = IntArray(0)): DecodedBlock? {
        if (bytes.size <= BLOCK_HEADER + BLOCK_PARITY || bytes.size > BLOCK_BYTES) return null
        val r = ReedSolomon.decode(bytes, BLOCK_PARITY, erasures) ?: return null
        val index = r.data[0]
        val total = r.data[1]
        if (total < 1 || index >= total) return null
        return DecodedBlock(Block(index, total, r.data.copyOfRange(BLOCK_HEADER, r.data.size)), r.corrected)
    }

    /** The text if the CRC-32 closes; null if not. `chunks` holds all the blocks in order. */
    fun assemble(chunks: List<IntArray>): String? {
        if (chunks.dropLast(1).any { it.size != BLOCK_DATA }) return null
        val data = chunks.fold(IntArray(0)) { a, c -> a + c }
        if (data.size <= 4) return null
        val content = data.copyOfRange(0, data.size - 4)
        val t = data.copyOfRange(data.size - 4, data.size)
        val crc = (t[0].toLong() shl 24) or (t[1].toLong() shl 16) or (t[2].toLong() shl 8) or t[3].toLong()
        return if (Checksums.crc32(content) == crc) TextPacking.unpack(content) else null
    }
}
