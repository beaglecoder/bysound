package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Modem
import io.github.beaglecoder.bysound.modem.Profile
import io.github.beaglecoder.bysound.modem.TextTooLongException
import org.junit.Assert.*
import org.junit.Test

private const val TEXT = "pay 12.50 to shop 7"
private val CHAIN = listOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID)

class AdaptRulesTest {
    @Test fun channelOrderGoesDownFromTheStartThenUp() {
        assertEquals(listOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID), channelOrder(Band.TOP, CHAIN))
        assertEquals(listOf(Band.UPPER, Band.MID, Band.HIGH, Band.TOP), channelOrder(Band.UPPER, CHAIN))
        assertEquals(listOf(Band.MID, Band.UPPER, Band.HIGH, Band.TOP), channelOrder(Band.MID, CHAIN))
    }

    @Test fun ambientSkipsNoisyChannelsAndFlagsALoudPlace() {
        val a = readAmbient(listOf(
            mapOf(Band.TOP to -50.0, Band.HIGH to -52.0, Band.UPPER to -80.0, Band.MID to -78.0),
            mapOf(Band.TOP to -49.0, Band.HIGH to -51.0, Band.UPPER to -81.0, Band.MID to -77.0),
            mapOf(Band.TOP to -51.0, Band.HIGH to -53.0, Band.UPPER to -79.0, Band.MID to -79.0),
        ), CHAIN)!!
        assertEquals(listOf(Band.TOP, Band.HIGH), a.noisy)
        assertFalse(a.loud)
        assertTrue(readAmbient(listOf(mapOf(Band.TOP to LOUD_PLACE_DB, Band.HIGH to LOUD_PLACE_DB + 1, Band.UPPER to LOUD_PLACE_DB, Band.MID to LOUD_PLACE_DB)), CHAIN)!!.loud)
        assertNull(readAmbient(emptyList(), listOf(Band.TOP)))
        val p = plan(calibration = null, ambient = a)
        assertEquals(listOf(Band.UPPER, Band.MID), p.route)
        assertNull(p.startRobust)
    }

    @Test fun planCalibrationExpiryFixedBandAndRobustStart() {
        val cal = Calibration(Band.MID, Profile.NORMAL, 3)
        val p = plan(calibration = cal, ambient = null)
        assertEquals(listOf(Band.MID, Band.UPPER, Band.HIGH, Band.TOP), p.route)
        assertEquals(6, p.every)
        assertEquals(cal, p.calibration)
        assertEquals(Band.TOP, plan(calibration = cal.copy(uses = CALIBRATION_USES), ambient = null).route[0])
        assertEquals(listOf(Band.XHIGH), plan(band = Band.XHIGH, calibration = cal.copy(uses = 0), ambient = null).route)
        val robust = Calibration(Band.UPPER, Profile.ROBUST, 0)
        assertEquals(StartRobust.CALIBRATION, plan(calibration = robust, ambient = null).startRobust)
        assertNull(plan(profile = Profile.NORMAL, calibration = robust, ambient = null).startRobust)
        val noisy = Ambient(mapOf(Band.TOP to ROBUST_NOISE_DB + 1, Band.HIGH to ROBUST_NOISE_DB, Band.UPPER to ROBUST_NOISE_DB, Band.MID to ROBUST_NOISE_DB), emptyList(), false)
        assertEquals(StartRobust.NOISE, plan(calibration = null, ambient = noisy).startRobust)
        // The calibrated channel stays in the route even if it measures noisy.
        assertEquals(listOf(Band.TOP, Band.UPPER, Band.MID), plan(calibration = Calibration(Band.TOP, Profile.NORMAL, 0), ambient = Ambient(emptyMap(), listOf(Band.TOP, Band.HIGH), false)).route)
    }

    @Test fun robustLadderAfterSixNormalRepetitionsOnlyWhereRobustExists() {
        val l = RobustLadder(true, false)
        val seen = listOf(Band.TOP, Band.TOP, Band.TOP, Band.HIGH, Band.HIGH, Band.HIGH, Band.XHIGH, Band.UPPER).map { l.next(it) }
        assertEquals(List(7) { Profile.NORMAL } + Profile.ROBUST, seen)
        assertEquals("repetitions", l.reason)
        assertEquals(Profile.NORMAL, RobustLadder(false, false).next(Band.TOP))
    }

    @Test fun lappedMeansBackToAChannelAfterTheWholeRoute() {
        assertFalse(lapped(listOf(Band.TOP, Band.TOP, Band.HIGH, Band.UPPER, Band.MID), 4))
        assertTrue(lapped(listOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID, Band.TOP), 4))
    }

    @Test fun storesKeepAndValidateCalibrations() {
        val m = MemoryCalibrationStore()
        m.save(Calibration(Band.MID, Profile.NORMAL, 1))
        assertEquals(Calibration(Band.MID, Profile.NORMAL, 1), m.load())
        val c = Calibration(Band.UPPER, Profile.ROBUST, 2)
        assertEquals(c, parseCalibration(c.encode()))
        assertNull(parseCalibration("nowhere:normal:1"))
        assertNull(parseCalibration("mid:normal:-1"))
        assertNull(parseCalibration("audible:normal:1"))
        assertNull(parseCalibration(null))
    }

    @Test fun ackRunsFollowTheSpecNumbers() {
        // docs/SESSION.md section 10: for "pay 12.50 to shop 7", L (top, robust, with gap) is 3.36 s, the ack run 7.86 s, receipts 10.86 s.
        val l = longestRepetitionSeconds(TEXT)
        assertEquals(3.36, l, 0.005)
        assertEquals(7.86, ackRunSeconds(l), 0.005)
        assertEquals(10.86, receiptSeconds(l), 0.005)
    }

    @Test fun aTextTooLongForTopRobustIsPlayedNormalAndStillTimesItsAcks() {
        val text = "a".repeat(Modem.maxPayloadBytes(Band.TOP, Profile.NORMAL)) // fits top/normal, not top/robust
        val l = longestRepetitionSeconds(text)
        assertTrue(l > Modem.MAX_FRAME_SECONDS)
        assertEquals(ACK_RUN_MAX_SECONDS, ackRunSeconds(l), 0.0)
        // The robust ladder would kick in after six repetitions: the text does not fit robust there, so it stays normal.
        val played = ArrayList<Int>()
        class Due(val at: Double, val fn: () -> Unit) { var cancelled = false }
        val due = ArrayList<Due>()
        var clock = 0.0
        val platform = object : Platform {
            override val sampleRate = 48000
            override fun now() = clock
            override fun after(seconds: Double, fn: () -> Unit): () -> Unit { val d = Due(clock + seconds, fn); due.add(d); return { d.cancelled = true } }
            override fun play(audio: FloatArray, onEnd: () -> Unit): () -> Unit { played.add(audio.size); return after(audio.size / 48000.0, onEnd) }
            override fun random() = 0.0
        }
        val reps = ArrayList<Repetition>()
        SenderSession(text, platform, SenderOptions(band = Band.TOP, securityLevel = SecurityLevel.BASIC, ambient = false, memory = RiskMemory(),
            timeoutSeconds = 1000.0, onRepetition = { reps.add(it) })).start()
        while (reps.size < 9) {
            val d = due.filter { !it.cancelled }.minByOrNull { it.at } ?: break
            due.remove(d)
            clock = d.at
            d.fn()
        }
        assertEquals(9, reps.size)
        assertTrue(reps.all { it.profile == Profile.NORMAL })
        assertTrue(played.all { it == played[0] && it > 0 })
        // Checked up front, on the caller's thread: too long for the band (or for a fixed robust profile), or empty.
        assertThrows(TextTooLongException::class.java) { SenderSession(text + "a", platform, SenderOptions(band = Band.TOP)) }
        assertThrows(TextTooLongException::class.java) { SenderSession(text, platform, SenderOptions(band = Band.TOP, profile = Profile.ROBUST)) }
        assertThrows(TextTooLongException::class.java) { SenderSession(text + "a", platform, SenderOptions(channels = listOf(Band.MID, Band.TOP))) }
        assertThrows(IllegalArgumentException::class.java) { SenderSession("", platform) }
    }
}

class AutomaticAdaptationTest {
    private class Run(val s: SenderSession, val reps: List<Repetition>)
    private fun Run.repetitions() = reps

    private fun send(d: Device, o: SenderOptions): Run {
        val reps = ArrayList<Repetition>()
        val s = SenderSession(TEXT, d, o.copy(memory = RiskMemory(), onRepetition = { reps.add(it) }))
        d.listen { s.hear(it) }
        s.start()
        return Run(s, reps)
    }

    private fun receive(d: Device): ReceiverSession {
        val r = ReceiverSession(d, ReceiverOptions(memory = RiskMemory(), securityLevel = SecurityLevel.STANDARD))
        d.listen(robust = true) { r.hear(it) }
        r.start()
        return r
    }

    @Test fun aBrokenTweeterClimbsDownToMidRemembersItAndTheNextSendIsReadAtOnce() {
        val air = Air()
        val phone = air.device("phone"); val rx = air.device("receiver")
        phone.speaker = brokenTweeter(17_300.0) // only mid gets through
        val store = MemoryCalibrationStore()
        val first = send(phone, SenderOptions(securityLevel = SecurityLevel.STANDARD, calibration = store))
        var r = receive(rx)
        air.run(70.0) { first.s.delivered.isDone }
        val d = first.s.delivered.value()!!
        assertEquals(Band.MID, d.band)
        assertTrue(d.stated)
        val reps = first.repetitions()
        assertEquals(listOf(Band.TOP, Band.TOP, Band.TOP), reps.take(3).map { it.band })
        assertEquals(setOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID), reps.map { it.band }.toSet())
        assertTrue("nobody read the first 6: the ladder went robust", reps.any { it.profile == Profile.ROBUST })
        assertEquals("robust by the ladder is not the speaker's", Calibration(Band.MID, Profile.NORMAL, 0), store.load())
        r.confirmed()
        first.s.stop()
        // No receipt reaches the receiver (receipts are on high), so it acks for ACK_TOTAL_SECONDS; the next pre-listen would measure
        // those acks as noise on mid. Wait until it is quiet.
        air.run(40.0) { r.finished.isDone }

        val second = send(phone, SenderOptions(securityLevel = SecurityLevel.STANDARD, calibration = store))
        r = receive(rx)
        air.run(30.0) { second.s.delivered.isDone }
        assertEquals(Repetition(0, Band.MID, Profile.NORMAL), second.repetitions()[0])
        val d2 = second.s.delivered.value()!!
        assertEquals(Band.MID, d2.band)
        assertEquals(1, d2.repetitions)
        assertEquals(Calibration(Band.MID, Profile.NORMAL, 0), store.load())
        second.s.stop()
    }

    @Test fun skipsChannelsTheAmbientCheckFindsNoisy() {
        val air = Air()
        val fan = air.device("fan"); val phone = air.device("phone")
        fan.play(hiss(18_900.0, 19_500.0, 0.02, 12.0)) { } // tones only top and high cover
        val routes = ArrayList<List<Band>>()
        val a = send(phone, SenderOptions(securityLevel = SecurityLevel.STANDARD, calibration = null, onAmbient = { _, route -> routes.add(route) }, timeoutSeconds = 3.0))
        air.run(8.0) { a.s.delivered.isDone }
        assertEquals(listOf(Band.UPPER, Band.MID), routes[0])
        assertEquals(Band.UPPER, a.repetitions()[0].band)
    }

    @Test fun aNoisyPlaceStartsRobustAndAnExplicitProfileWins() {
        for ((profile, expected) in listOf(null to Profile.ROBUST, Profile.NORMAL to Profile.NORMAL)) {
            val air = Air(48000, 0.05) // white noise about -40 dB per tone: over ROBUST_NOISE_DB on every channel
            val a = send(air.device("phone"), SenderOptions(securityLevel = SecurityLevel.BASIC, calibration = null, timeoutSeconds = 2.0, profile = profile))
            air.run(5.0) { a.s.delivered.isDone }
            assertEquals(expected, a.repetitions()[0].profile)
        }
    }

    @Test fun aCalibrationThatKeepsFailingExpiresAfterCalibrationUsesSends() {
        val store = MemoryCalibrationStore(Calibration(Band.UPPER, Profile.NORMAL, CALIBRATION_USES - 1))
        val air = Air()
        val a = send(air.device("phone"), SenderOptions(securityLevel = SecurityLevel.BASIC, calibration = store, timeoutSeconds = 2.0))
        air.run(5.0) { a.s.delivered.isDone }
        assertEquals(Band.UPPER, a.repetitions()[0].band)
        assertEquals(CALIBRATION_USES, store.load()?.uses)
        val b = send(air.device("phone2"), SenderOptions(securityLevel = SecurityLevel.BASIC, calibration = store, timeoutSeconds = 2.0))
        air.run(5.0) { b.s.delivered.isDone }
        assertEquals(Band.TOP, b.repetitions()[0].band)
    }
}
