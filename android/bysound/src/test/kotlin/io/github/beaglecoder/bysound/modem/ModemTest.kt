package io.github.beaglecoder.bysound.modem

import org.junit.Assert.*
import org.junit.Test

class VectorsTest {
    private fun hex(a: IntArray) = a.joinToString("") { "%02x".format(it) }
    private fun band(id: String) = Band.entries.first { it.id == id }

    @Test fun payloadVectorsMatch() {
        val payloads = TestAudio.vectors().filter { it["mode"].asString == "payload" }
        assertTrue(payloads.size >= 20)
        for (v in payloads) {
            val profile = if (v["profile"].asString == "robust") Profile.ROBUST else Profile.NORMAL
            val f = Codec.payloadFrame(v["text"].asString, band(v["band"].asString), profile)
            val label = "${v["name"].asString} ${v["band"].asString} ${v["profile"].asString}"
            assertEquals(label, v["header"].asString, hex(f.header))
            assertEquals(label, v["body"].asString, hex(f.body))
            val expected = v["symbols"].asJsonArray.map { row -> row.asJsonArray.map { it.asInt } }
            assertEquals(label, expected, f.symbols.map { it.toList() })
            assertEquals(label, v["frameSeconds"].asDouble, String.format(java.util.Locale.ROOT, "%.6f", f.frameSeconds).toDouble(), 0.0)
            assertEquals(label, expected, Modem.symbols(v["text"].asString, band(v["band"].asString), profile).map { it.toList() })
        }
    }
}

class MaxDurationTest {
    @Test fun capMatchesJson() {
        val spec = com.google.gson.JsonParser.parseString(java.io.File(TestAudio.root, "protocol-v4.json").readText()).asJsonObject
        assertEquals(spec["maxFrameSeconds"].asInt, Protocol.MAX_FRAME_SECONDS)
        assertEquals(spec["gapSeconds"].asDouble, Protocol.GAP_SECONDS, 0.0)
        assertEquals(spec["maxBlocks"].asInt, Protocol.MAX_BLOCKS)
    }

    @Test fun maximumEncodesAndOneMoreIsRejected() {
        for (band in Band.entries) for (profile in Profile.entries) {
            if (profile == Profile.ROBUST && !band.hasRobust) continue
            val label = "${band.id} $profile"
            val m = Modem.maxPayloadBytes(band, profile)
            val ok = Codec.payloadFrame("a".repeat(m), band, profile)
            assertTrue(label, ok.frameSeconds + Modem.GAP_SECONDS <= Modem.MAX_FRAME_SECONDS + 1e-9)
            val e = try { Codec.payloadFrame("a".repeat(m + 1), band, profile); null } catch (e: TextTooLongException) { e }
            assertNotNull(label, e)
            assertEquals(label, m, e!!.maxPackedBytes)
        }
        assertEquals(700, Modem.maxPayloadBytes(Band.entries.first { it.id == "top" }))
        assertEquals(2114, Modem.maxPayloadBytes(Band.entries.first { it.id == "wide" }))
    }

    @Test fun bandAboveNyquistIsRejected() {
        val e = try { Modem.encode("hi", Band.TOP, Profile.NORMAL, 16000); null } catch (e: IllegalArgumentException) { e }
        assertNotNull(e)
        assertTrue(e!!.message!!.contains("Nyquist"))
        assertTrue(Modem.encode("hi", Band.TOP, Profile.NORMAL, 48000).isNotEmpty())
    }
}

class ReedSolomonTest {
    @Test fun roundTripWithErrorsAndErasures() {
        val data = IntArray(50) { (it * 37 + 5) and 255 }
        val code = ReedSolomon.encode(data, 12)
        assertEquals(62, code.size)
        assertArrayEquals(data, ReedSolomon.decode(code, 12)!!.data)
        val bad = code.copyOf()
        for (p in intArrayOf(1, 20, 33)) bad[p] = bad[p] xor 0x5a
        val r = ReedSolomon.decode(bad, 12)!!
        assertArrayEquals(data, r.data)
        assertEquals(3, r.corrected)
        val erased = code.copyOf()
        val pos = intArrayOf(0, 5, 9, 14, 30, 40, 41, 50, 55, 60)
        for (p in pos) erased[p] = 0
        assertArrayEquals(data, ReedSolomon.decode(erased, 12, pos)!!.data)
        val dead = code.copyOf()
        for (p in 0 until 8) dead[p] = dead[p] xor 0xff
        assertNull(ReedSolomon.decode(dead, 12))
    }

    @Test fun checksums() {
        val d = "123456789".map { it.code }.toIntArray()
        assertEquals(0x29b1, Checksums.crc16(d))
        assertEquals(0xcbf43926L, Checksums.crc32(d))
    }

    @Test fun textPacking() {
        for (t in listOf("hello 12 34 5", "ñandú 🙂", "", "0123", "a".repeat(300) + "é".repeat(200))) {
            assertEquals(t, TextPacking.unpack(TextPacking.pack(t)))
        }
        assertNull(TextPacking.unpack(intArrayOf(0xff, 2, 0xc3)))
    }
}

class LoopbackTest {
    private val short = """{"m":"Almacén","a":1250.5}"""
    private val long = "00020101021226470017com.example.pagos01220000003100099988877766520454115303032540812500.505802AR5918ALMACEN LA ESQUINA6007CORDOBA62250108F-0001230509PEDIDO-776304F4AF ñandú 🙂"

    private fun roundTrip(text: String, band: Band, rate: Int, profile: Profile = Profile.NORMAL, noise: Double, seed: Int = 1) {
        val lap = Modem.encode(text, band, profile, rate)
        val heard = TestAudio.listen(TestAudio.air(listOf(lap, lap), rate, noise, seed = seed), rate)
        val first = heard.firstOrNull()
        assertNotNull("nothing heard on ${band.id} at $rate", first)
        assertEquals(text, first!!.text)
        assertEquals(band, first.band)
        assertEquals(profile, first.profile)
    }

    @Test fun shortAllBands() { for (rate in intArrayOf(48000, 44100)) for (b in Band.entries) roundTrip(short, b, rate, noise = 0.3, seed = 2) }
    @Test fun longAllBands() { for (rate in intArrayOf(48000, 44100)) for (b in Band.entries) roundTrip(long, b, rate, noise = 0.15, seed = 3) }
    @Test fun robustBands() {
        for (rate in intArrayOf(48000, 44100)) for (b in Band.entries.filter { it.hasRobust }) roundTrip(short, b, rate, Profile.ROBUST, noise = 0.2)
    }
    @Test fun quietMicWithEcho() {
        val lap = Modem.encode(short, Band.TOP, Profile.NORMAL, 48000)
        val heard = TestAudio.listen(TestAudio.air(listOf(lap, lap, lap), 48000, noise = 0.15, gain = 0.2, echo = 0.3, seed = 7), 48000)
        assertEquals(short, heard.firstOrNull()?.text)
    }
    @Test fun silenceHearsNothing() {
        assertTrue(TestAudio.listen(TestAudio.air(listOf(FloatArray(48000 * 4)), 48000, 0.3), 48000).isEmpty())
    }
    @Test fun nonAsciiAndBlocks() {
        val lap = Modem.encode(long, Band.WIDE, Profile.NORMAL, 44100)
        assertTrue(lap.size > 44100)
        assertTrue(lap.all { Math.abs(it) <= 1f })
    }
}

class CrossImplementationTest {
    @Test fun cGoldenPayloadWide44100() {
        val expected = TestAudio.vectors().first { it["mode"].asString == "payload" && it["band"].asString == "wide" && it["name"].asString == "ejemplo" }["text"].asString
        val pcm = TestAudio.pcm16(java.io.File(TestAudio.root, "c/test/golden/payload_wide_44100.i16"))
        val heard = TestAudio.listen(TestAudio.air(listOf(pcm), 44100, 0.0, lead = 0.5), 44100)
        assertEquals(expected, heard.firstOrNull()?.text)
    }

    @Test fun recordingFalso48k() {
        val pcm = TestAudio.pcm16(java.io.File(TestAudio.root, "c/test/recordings/payload-top-v4.wav"), skip = 44)
        val heard = TestAudio.listen(pcm, 48000)
        assertTrue("nothing heard", heard.isNotEmpty())
        assertTrue(heard[0].text, heard[0].text.startsWith("000201010212264700"))
    }
}
