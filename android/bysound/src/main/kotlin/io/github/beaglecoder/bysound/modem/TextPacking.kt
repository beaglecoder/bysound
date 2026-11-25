package io.github.beaglecoder.bysound.modem

import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

/**
 * Text compaction of bysound v4: printable ASCII is one byte, a pair of digits is one byte (0x80 + value), and everything else is
 * UTF-8 in runs of up to 255 bytes behind a `0xFF, length` marker (a run never splits a character).
 */
object TextPacking {
    private fun digit(c: Char) = c in '0'..'9'
    private fun ascii(c: Char) = c.code in 0x20..0x7e

    /** UTF-8 bytes of [text] in chunks of at most 255 bytes that never cut a character in half. */
    private fun utf8Chunks(text: String): List<ByteArray> {
        val chunks = ArrayList<ByteArray>()
        var cur = java.io.ByteArrayOutputStream()
        var i = 0
        while (i < text.length) {
            val cp = text.codePointAt(i)
            i += Character.charCount(cp)
            val b = String(Character.toChars(cp)).toByteArray(Charsets.UTF_8)
            if (cur.size() + b.size > 255) {
                chunks.add(cur.toByteArray())
                cur = java.io.ByteArrayOutputStream()
            }
            cur.write(b)
        }
        if (cur.size() > 0) chunks.add(cur.toByteArray())
        return chunks
    }

    /** Packs [text] into content bytes (0..255 each). */
    fun pack(text: String): IntArray {
        val out = ArrayList<Int>()
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (digit(c) && i + 1 < text.length && digit(text[i + 1])) {
                out.add(0x80 + (c - '0') * 10 + (text[i + 1] - '0'))
                i += 2
            } else if (ascii(c)) {
                out.add(c.code)
                i += 1
            } else {
                var j = i
                while (j < text.length && !ascii(text[j])) j++
                for (chunk in utf8Chunks(text.substring(i, j))) {
                    out.add(0xff)
                    out.add(chunk.size)
                    for (b in chunk) out.add(b.toInt() and 0xff)
                }
                i = j
            }
        }
        return out.toIntArray()
    }

    /** The text of packed [data], or null if it is not valid. */
    fun unpack(data: IntArray): String? {
        val s = StringBuilder()
        var i = 0
        while (i < data.size) {
            val b = data[i]
            when {
                b in 0x20..0x7e -> { s.append(b.toChar()); i += 1 }
                b in 0x80..0xe3 -> { val v = b - 0x80; s.append(v / 10).append(v % 10); i += 1 }
                b == 0xff -> {
                    // Consecutive 0xFF runs are one text: they are joined before decoding.
                    val bytes = java.io.ByteArrayOutputStream()
                    while (i < data.size && data[i] == 0xff) {
                        if (i + 1 >= data.size) return null
                        val n = data[i + 1]
                        if (n == 0 || i + 2 + n > data.size) return null
                        for (k in 0 until n) bytes.write(data[i + 2 + k])
                        i += 2 + n
                    }
                    val decoder = Charsets.UTF_8.newDecoder()
                        .onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT)
                    try { s.append(decoder.decode(ByteBuffer.wrap(bytes.toByteArray()))) } catch (e: java.nio.charset.CharacterCodingException) { return null }
                }
                else -> return null
            }
        }
        return s.toString()
    }
}
