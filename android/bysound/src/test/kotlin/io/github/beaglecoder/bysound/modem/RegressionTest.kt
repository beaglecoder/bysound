package io.github.beaglecoder.bysound.modem

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Receiver bugs found by the benchmark in bench/ (see bench/README.md), in a clean channel. */
class RegressionTest {
    /** First text of the series whose last block ends in a parity byte 0x00 (about 1 in 256). */
    private fun zeroParityText(prefix: String): String {
        var i = 0
        while (true) {
            val text = "$prefix $i"
            if (Codec.payloadFrame(text, Band.TOP, Profile.NORMAL).body.last() == 0) return text
            i++
        }
    }

    private fun heard(text: String, profile: Profile, rate: Int, laps: Int = 1): List<String> {
        val lap = Modem.encode(text, Band.TOP, profile, rate)
        return TestAudio.listen(TestAudio.air(List(laps) { lap }, rate, noise = 0.0), rate).map { it.text }
    }

    /** RS also accepts the last block one byte short when its last parity byte is 0x00 (c(x) = x·c'(x)): the CRC-32 picks the length. */
    @Test fun zeroLastParityByte() {
        val short = zeroParityText("zero parity")
        val long = zeroParityText("a payload long enough to need two blocks of forty-eight bytes, zero parity")
        assertTrue(Codec.payloadFrame(short, Band.TOP, Profile.NORMAL).body.size < 64)
        assertTrue(Codec.payloadFrame(long, Band.TOP, Profile.NORMAL).body.size > 64)
        for (text in listOf(short, long)) for (profile in listOf(Profile.NORMAL, Profile.ROBUST)) {
            assertEquals("$text $profile", listOf(text), heard(text, profile, 48000))
        }
    }

    /**
     * On top/robust, body symbols 50..57 of this payload match 6 of the preamble's 16 nibbles (0.375 without noise): that used to be
     * taken as the next lap and cut off the read before the last block, so the payload failed only at high SNR.
     */
    @Test fun preambleLookAlikeInBody() {
        val text = "q(qs@TgOXJfdn<NXK<f,h?<StQ*yJq)b"
        for (rate in intArrayOf(48000, 44100)) assertEquals("$rate", listOf(text), heard(text, Profile.ROBUST, rate))
        assertEquals(listOf(text, text), heard(text, Profile.ROBUST, 48000, laps = 2))
    }
}
