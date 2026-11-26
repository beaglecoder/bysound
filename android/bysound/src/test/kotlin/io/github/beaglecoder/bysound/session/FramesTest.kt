package io.github.beaglecoder.bysound.session

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Checksums
import io.github.beaglecoder.bysound.modem.Codec
import io.github.beaglecoder.bysound.modem.Frame
import io.github.beaglecoder.bysound.modem.HeardReference
import io.github.beaglecoder.bysound.modem.Modem
import io.github.beaglecoder.bysound.modem.Modulator
import io.github.beaglecoder.bysound.modem.Profile
import io.github.beaglecoder.bysound.modem.Receiver
import io.github.beaglecoder.bysound.modem.ReceiverEvents
import io.github.beaglecoder.bysound.modem.TestAudio
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.util.Locale

private fun vectors(name: String): List<JsonObject> =
    JsonParser.parseString(File(TestAudio.root, "test-vectors/$name").readText()).asJsonObject.getAsJsonArray("vectors").map { it.asJsonObject }

private fun band(id: String) = Band.entries.first { it.id == id }
private fun symbols(v: JsonObject) = v["symbols"].asJsonArray.map { row -> row.asJsonArray.map { it.asInt } }
private fun checkFrame(label: String, f: Frame, v: JsonObject) {
    assertEquals(label, v["header"].asString, hex(f.header))
    assertEquals(label, v["body"].asString, hex(f.body))
    assertEquals(label, symbols(v), f.symbols.map { it.toList() })
    if (v.has("frameSeconds")) assertEquals(label, v["frameSeconds"].asDouble, String.format(Locale.ROOT, "%.6f", f.frameSeconds).toDouble(), 0.0)
}

class FrameVectorsTest {
    @Test fun acksMatchV4Acks() {
        val all = vectors("v4-acks.json")
        assertTrue(all.isNotEmpty())
        for (v in all) {
            val id = ackId(v["text"].asString)
            val receipt = v["receipt"].asBoolean
            val label = "${v["band"].asString} ${if (receipt) "receipt" else "ack"}"
            val f = ackFrame(AckFrame(receipt, id), band(v["band"].asString))
            checkFrame(label, f, v)
            assertEquals(label, AckFrame(receipt, id), parseAck(f.body.copyOf(7)))
        }
    }

    @Test fun acksSayWhereTheDataWasRead() {
        val all = vectors("session-acks.json")
        assertTrue(all.isNotEmpty())
        for (v in all) {
            val heard = v["heard"]?.takeIf { !it.isJsonNull }?.asJsonObject?.let { AckHeard(band(it["band"].asString), if (it["profile"].asString == "robust") Profile.ROBUST else Profile.NORMAL) }
            val ack = AckFrame(v["receipt"].asBoolean, ackId(v["text"].asString), heard)
            val label = "${v["band"].asString} ${ack.receipt} $heard"
            val f = ackFrame(ack, band(v["band"].asString))
            checkFrame(label, f, v)
            assertEquals(label, v["data"].asString, hex(ackBytes(ack)))
            assertEquals(label, ack, parseAck(f.body.copyOf(7)))
        }
    }

    @Test fun robustFlagWithoutABandIsInvalid() {
        fun withByte0(b0: Int): AckFrame? {
            val d = intArrayOf(b0, 0, 0, 0, 0)
            val c = Checksums.crc16(d)
            return parseAck(d + intArrayOf(c shr 8, c and 255))
        }
        assertNull(withByte0(0x82))
        assertEquals(AckFrame(false, "00000000", AckHeard(Band.WIDE, Profile.NORMAL)), withByte0(0x72))
    }

    @Test fun risksMatchV4Risk() {
        val all = vectors("v4-risk.json")
        assertTrue(all.size >= 10)
        for (v in all) {
            val r = RiskFrame(
                RiskReason.entries[v["reason"].asInt - 1], if (v["source"].asInt == 1) Role.RECEIVER else Role.SENDER, v["hops"].asInt,
                v["own"].asJsonArray.joinToString("") { "%02x".format(it.asInt) }, v["intruder"].asJsonArray.joinToString("") { "%02x".format(it.asInt) },
            )
            val label = "${v["band"].asString} reason ${v["reason"].asInt} source ${v["source"].asInt} hops ${v["hops"].asInt}"
            val f = riskFrame(r, band(v["band"].asString))
            checkFrame(label, f, v)
            assertEquals(label, r, parseRisk(f.body.copyOf(10)))
        }
    }

    @Test fun referenceFramesMatchV4Frames() {
        val refs = vectors("v4-frames.json").filter { it["mode"].asString == "reference" }
        assertTrue(refs.isNotEmpty())
        for (v in refs) {
            val profile = if (v["profile"].asString == "robust") Profile.ROBUST else Profile.NORMAL
            val f = Codec.referenceFrame(fromHex(fingerprint(v["text"].asString)), band(v["band"].asString), v["set"].asInt, profile)
            checkFrame("${v["name"].asString} ${v["band"].asString} set ${v["set"].asInt} $profile", f, v)
        }
    }

    @Test fun corruptFramesAreRejected() {
        val ok = ackBytes(AckFrame(false, "00112233"))
        assertNotNull(parseAck(ok))
        val bad = ok.copyOf(); bad[2] = bad[2] xor 1
        assertNull(parseAck(bad))
        assertNull(parseAck(ok.copyOf(6)))
        assertNull(parseRisk(ok))
    }

    @Test fun riskBandsAvoidWhatTheyReport() {
        assertEquals(listOf(Band.MID, Band.TOP), riskBands(Band.TOP))
        assertEquals(listOf(Band.MID), riskBands(Band.MID))
        assertEquals(listOf(Band.MID), riskBands(Band.AUDIBLE))
        assertEquals(listOf(Band.MID), riskBands(Band.TOP, Band.HIGH))
        assertEquals(listOf(Band.TOP), riskBands(Band.TOP, Band.MID))
        assertEquals(listOf(Band.MID, Band.XHIGH), riskBands(Band.XHIGH, Band.WIDE)) // nothing is clean: the default list
        assertFalse(bandsOverlap(Band.MID, Band.TOP))
        assertTrue(bandsOverlap(Band.UPPER, Band.MID))
    }

    @Test fun identifiers() {
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", payloadId("abc"))
        assertEquals("ba7816bf8f01", fingerprint("abc").take(12))
        assertEquals(24, fingerprint("abc").length)
        assertEquals("ba7816bf", ackId("abc"))
        assertEquals("ba7816", shortId("abc"))
    }
}

/** The modem receiver now reports acks, risk frames and references itself (docs/SESSION.md section 11). */
class ControlReceptionTest {
    private class Seen : ReceiverEvents {
        val controls = ArrayList<Triple<Int, List<Int>, Band>>()
        val references = ArrayList<HeardReference>()
        val payloads = ArrayList<String>()
        val noise = ArrayList<Map<Band, Double>>()
        override fun onFrame(heard: io.github.beaglecoder.bysound.modem.Heard) { payloads.add(heard.text) }
        override fun onControl(magic: Int, data: IntArray, band: Band) { controls.add(Triple(magic, data.toList(), band)) }
        override fun onReference(heard: HeardReference) { references.add(heard) }
        override fun onNoise(noise: Map<Band, Double>) { this.noise.add(noise) }
    }

    private fun listen(x: FloatArray, rate: Int, profile: Profile = Profile.NORMAL): Seen {
        val seen = Seen()
        val rx = Receiver(rate, profile, seen)
        var i = 0
        while (i < x.size) { rx.feed(x.copyOfRange(i, minOf(i + 1024, x.size))); i += 1024 }
        rx.feed(FloatArray(rate))
        return seen
    }

    private fun render(f: Frame, rate: Int) = Modulator.render(f.symbols, f.channel, rate, Modem.GAP_SECONDS)
    private val risk = RiskFrame(RiskReason.OVERLAP, Role.RECEIVER, 1, shortId("a"), shortId("b"))

    @Test fun readsAnAckAndARiskFrameOnEveryBandInNoise() {
        for (b in listOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID, Band.XHIGH, Band.WIDE)) {
            val a = ackFrame(AckFrame(false, ackId("hello")), b)
            val r = riskFrame(risk, b)
            val x = TestAudio.air(listOf(render(a, 48000), render(r, 48000)), 48000, 0.05, seed = 3)
            val seen = listen(x, 48000)
            assertEquals(b.id, 2, seen.controls.size)
            assertEquals(b.id, AckFrame(false, ackId("hello")), parseAck(seen.controls[0].second.toIntArray()))
            assertEquals(b.id, risk, parseRisk(seen.controls[1].second.toIntArray()))
            assertEquals(listOf(b, b), seen.controls.map { it.third })
        }
    }

    @Test fun works44100() {
        val a = ackFrame(AckFrame(true, ackId("hello")))
        val seen = listen(TestAudio.air(listOf(render(a, 44100)), 44100, 0.03), 44100)
        assertEquals(1, seen.controls.size)
        assertEquals(Band.HIGH, seen.controls[0].third)
    }

    @Test fun readsAnAckOnMidWhilePayloadPlaysOnTop() {
        val p = Modem.encode("a payload that keeps playing", Band.TOP, Profile.NORMAL, 48000)
        val ack = render(ackFrame(AckFrame(false, ackId("x"))), 48000)
        val x = TestAudio.air(listOf(p, p), 48000, 0.02)
        for (i in ack.indices) x[30000 + i] += ack[i]
        val seen = listen(x, 48000)
        assertEquals(1, seen.controls.size)
        assertEquals(listOf("a payload that keeps playing"), seen.payloads.distinct())
    }

    @Test fun ignoresPayloadAndNoise() {
        val seen = listen(TestAudio.air(listOf(Modem.encode("not a control frame", Band.MID, Profile.NORMAL, 48000)), 48000, 0.05), 48000)
        assertTrue(seen.controls.isEmpty())
        assertEquals(listOf("not a control frame"), seen.payloads.distinct())
        assertTrue(listen(TestAudio.air(listOf(FloatArray(48000 * 3)), 48000, 0.2, seed = 9), 48000).controls.isEmpty())
    }

    @Test fun readsReferencesOnTheirBandsAndProfiles() {
        val fp = fingerprint("pay 12.50 to shop 7")
        for ((b, profile) in listOf(Band.TOP to Profile.NORMAL, Band.MID to Profile.NORMAL, Band.XHIGH to Profile.NORMAL, Band.UPPER to Profile.ROBUST)) {
            val f = Codec.referenceFrame(fromHex(fp), b, 0, profile)
            val x = TestAudio.air(listOf(render(f, 48000)), 48000, 0.05)
            val seen = listen(x, 48000, profile)
            assertEquals("${b.id} $profile", fp, seen.references.firstOrNull()?.fingerprint)
            assertEquals(b, seen.references.first().band)
            assertEquals(profile, seen.references.first().profile)
        }
    }

    @Test fun combinesTheTwoParitySetsOfAReference() {
        val fp = fingerprint("pay 12.50 to shop 7")
        val a = render(Codec.referenceFrame(fromHex(fp), Band.TOP, 0, Profile.NORMAL), 48000)
        val b = render(Codec.referenceFrame(fromHex(fp), Band.TOP, 1, Profile.NORMAL), 48000)
        val seen = listen(TestAudio.air(listOf(a, b, a, b), 48000, 0.12, seed = 4), 48000)
        assertTrue(seen.references.isNotEmpty())
        assertTrue(seen.references.all { it.fingerprint == fp })
    }

    @Test fun reportsNoisePerBand() {
        val seen = listen(TestAudio.air(listOf(FloatArray(48000 * 2)), 48000, 0.01, seed = 2), 48000)
        assertTrue(seen.noise.size >= 6)
        val n = seen.noise[3] // the last ones are the silence that listen() appends
        assertEquals(setOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID, Band.XHIGH, Band.WIDE), n.keys)
        // White noise of sigma 0.01 is about 20 log10(sigma) - 24 = -64 dB per tone (docs/SESSION.md section 9.2).
        for (v in n.values) assertEquals(-64.0, v, 3.0)
    }
}

class SecurityLevelsTest {
    @Test fun matchTheVectorTable() {
        val table = JsonParser.parseString(File(TestAudio.root, "test-vectors/security-levels.json").readText()).asJsonObject
        assertEquals(table["default"].asString, DEFAULT_SECURITY_LEVEL.id)
        for (level in SecurityLevel.entries) {
            val t = table["levels"].asJsonObject[level.id].asJsonObject
            val s = SECURITY_LEVELS.getValue(level)
            val r = t["riskSignal"].asJsonObject
            assertEquals(t["detectMultipleEmitters"].asBoolean, s.detectMultipleEmitters)
            assertEquals(t["listenWhileSending"].asBoolean, s.listenWhileSending)
            assertEquals(t["listenBeforeSendingSeconds"].asDouble, s.listenBeforeSendingSeconds, 0.0)
            assertEquals(t["scanJitterSeconds"].asDouble, s.scanJitterSeconds, 0.0)
            assertEquals(t["confirmWatch"].asBoolean, s.confirmWatch)
            assertEquals(t["confirmWatchSeconds"].asDouble, s.confirmWatchSeconds, 0.0)
            assertEquals(r["emit"].asString, s.riskSignal.emit.name.lowercase())
            assertEquals(r["listen"].asBoolean, s.riskSignal.listen)
            assertEquals(r["honor"].asBoolean, s.riskSignal.honor)
            assertEquals(r["listenUntil"].asString, s.riskSignal.listenUntil.name.lowercase())
            assertEquals(r["durationMs"].asLong, s.riskSignal.durationMs)
            assertEquals(r["detectorDurationMs"].asLong, s.riskSignal.detectorDurationMs)
            assertEquals(r["quarantineMs"].asLong, s.riskSignal.quarantineMs)
        }
        assertEquals(SECURITY_LEVELS.getValue(SecurityLevel.MAXIMUM), resolveSecurity())
    }

    @Test fun explicitOptionsWinFieldByField() {
        val s = resolveSecurity(SecurityLevel.STANDARD, SecurityOverrides(listenBeforeSendingSeconds = 5.0, riskSignal = RiskSignalOverrides(honor = false)))
        assertEquals(5.0, s.listenBeforeSendingSeconds, 0.0)
        assertEquals(120.0, s.confirmWatchSeconds, 0.0)
        assertEquals(SECURITY_LEVELS.getValue(SecurityLevel.STANDARD).riskSignal.copy(honor = false), s.riskSignal)
    }
}
