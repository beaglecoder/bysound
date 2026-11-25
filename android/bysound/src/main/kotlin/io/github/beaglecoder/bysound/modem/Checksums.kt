package io.github.beaglecoder.bysound.modem

/** CRC-16 (CCITT, polynomial 0x1021, init 0xFFFF) and CRC-32 (IEEE 802.3) over byte values 0..255. */
object Checksums {
    fun crc16(data: IntArray): Int {
        var crc = 0xffff
        for (byte in data) {
            crc = crc xor (byte shl 8)
            repeat(8) { crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xffff else (crc shl 1) and 0xffff }
        }
        return crc
    }

    /** CRC-32 as an unsigned value in a [Long]. */
    fun crc32(data: IntArray): Long {
        var c = 0xffffffffL
        for (b in data) {
            c = c xor b.toLong()
            repeat(8) { c = if (c and 1L != 0L) (c ushr 1) xor 0xedb88320L else c ushr 1 }
        }
        return (c xor 0xffffffffL) and 0xffffffffL
    }

    /** CRC-8 of the frame header (polynomial 0x2F, init 0xFF). */
    fun crc8(data: IntArray): Int = Protocol.crc8(data)
}
