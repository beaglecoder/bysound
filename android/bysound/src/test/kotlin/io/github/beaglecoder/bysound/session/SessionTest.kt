package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Modulator
import org.junit.Assert.*
import org.junit.Test

private const val TEXT = "pay 12.50 to shop 7"
private const val ROGUE = "pay 99.00 to shop 666"

private class Sender(val s: SenderSession, val alerts: List<Alert>, val quarantines: List<QuarantineNotice>) {
    val delivered get() = s.delivered
}
private class Receiver(val r: ReceiverSession, val alerts: List<Alert>, val quarantines: List<QuarantineNotice>) {
    val result get() = r.result
}

private fun sender(d: Device, text: String, o: SenderOptions = SenderOptions()): Sender {
    val alerts = ArrayList<Alert>(); val quarantines = ArrayList<QuarantineNotice>()
    val opts = o.copy(
        memory = if (o.memory === sharedRiskMemory) RiskMemory() else o.memory,
        calibration = if (o.calibration === sharedCalibrationStore) MemoryCalibrationStore() else o.calibration,
        onAlert = { alerts.add(it) }, onQuarantine = { quarantines.add(it) },
    )
    val s = SenderSession(text, d, opts)
    d.listen { s.hear(it) }
    s.start()
    return Sender(s, alerts, quarantines)
}

private fun receiver(d: Device, o: ReceiverOptions = ReceiverOptions()): Receiver {
    val alerts = ArrayList<Alert>(); val quarantines = ArrayList<QuarantineNotice>()
    val opts = o.copy(memory = if (o.memory === sharedRiskMemory) RiskMemory() else o.memory, onAlert = { alerts.add(it) }, onQuarantine = { quarantines.add(it) })
    val r = ReceiverSession(d, opts)
    d.listen { r.hear(it) }
    r.start()
    return Receiver(r, alerts, quarantines)
}

private fun risk(hops: Int) = riskFrame(RiskFrame(RiskReason.OTHER, Role.RECEIVER, hops, shortId("x"), shortId("y")), Band.MID)
private fun riskLoop(hops: Int): FloatArray { val f = risk(hops); return Modulator.render(f.symbols, f.channel, 48000, CONTROL_GAP_SECONDS) }

class AckAndReceiptTest {
    @Test fun theAckStopsTheSenderTheReceiptReachesTheReceiverTheGateIsClean() {
        val air = Air()
        val a = sender(air.device("sender"), TEXT)
        val b = receiver(air.device("receiver"))
        air.run(30.0) { a.delivered.isDone && b.result.isDone }
        assertEquals(Received(TEXT, Band.TOP, Mode.PAYLOAD), b.result.value())
        assertEquals(Band.TOP, a.delivered.value()?.band)
        assertEquals(SenderState.RECEIPT, a.s.state) // the payload stopped; it is answering with receipts
        air.run(25.0) { b.r.receipt.isDone && a.s.state == SenderState.DONE }
        assertEquals(true, b.r.receipt.value())
        assertEquals(SenderState.DONE, a.s.state)
        val c = b.r.confirmed().join()
        assertEquals(Watch.CLEAN, c.watch)
        assertTrue(c.watchedSeconds > 0)
        assertTrue(a.alerts.isEmpty())
        assertTrue(b.alerts.isEmpty())
    }

    @Test fun aSenderWithNobodyAckingTimesOut() {
        val air = Air()
        val a = sender(air.device("sender"), TEXT, SenderOptions(securityLevel = SecurityLevel.BASIC, timeoutSeconds = 6.0))
        air.run(10.0) { a.delivered.isDone }
        assertEquals(SessionErrorCode.TIMEOUT, a.delivered.error()?.code)
    }
}

class TwoSendersTest {
    @Test fun theReceiverHearsASecondSenderRejectsTheDataAndSoundsARiskFrameTheRealSenderStopsAndRelaysIt() {
        val air = Air()
        val legit = air.device("legit"); val rogue = air.device("rogue"); val rx = air.device("receiver")
        air.gain(rogue, legit, 0.0) // they cannot hear each other: only the receiver can warn the legit sender
        val a = sender(legit, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD))
        val b = receiver(rx, ReceiverOptions(securityLevel = SecurityLevel.STANDARD))
        air.run(2.5)
        rogue.loop(loopAudio(ROGUE, Band.HIGH), 20.0)
        air.run(30.0) { a.delivered.isDone && b.result.isDone }
        assertEquals(SessionErrorCode.MULTIPLE_TRANSMISSIONS, b.result.error()?.code)
        assertEquals(Alert.Direction.EMITTED, b.alerts[0].direction)
        assertEquals(Role.RECEIVER, b.alerts[0].source)
        assertTrue(b.alerts[0].reason in listOf(RiskReason.OVERLAP, RiskReason.OTHER))
        assertEquals(SessionErrorCode.RISK_SIGNAL, a.delivered.error()?.code)
        val heard = a.delivered.error()!!.alert!!
        assertEquals(Alert.Direction.HEARD, heard.direction); assertEquals(Role.RECEIVER, heard.source); assertEquals(0, heard.hops)
        val relay = a.alerts.first { it.direction == Alert.Direction.EMITTED }
        assertTrue(relay.relay); assertEquals(1, relay.hops); assertEquals(Role.RECEIVER, relay.source)
        assertEquals(30000L, b.quarantines[0].durationMs)
        assertEquals(30000L, a.quarantines[0].durationMs)
    }

    @Test fun aSenderThatHearsAnotherTransmissionInItsListenWindowStopsAndWarnsTheReceiver() {
        val air = Air()
        val legit = air.device("legit"); val rogue = air.device("rogue"); val rx = air.device("receiver")
        air.gain(rogue, rx, 0.0) // the receiver cannot hear the rogue: only the sender can
        val a = sender(legit, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD))
        val b = receiver(rx, ReceiverOptions(securityLevel = SecurityLevel.STANDARD, ack = false))
        air.run(2.2)
        rogue.loop(loopAudio(ROGUE, Band.XHIGH), 30.0)
        air.run(30.0) { a.delivered.isDone && b.result.isDone && b.alerts.isNotEmpty() }
        assertEquals(SessionErrorCode.OTHER_EMITTER, a.delivered.error()?.code)
        val e = a.alerts[0]
        assertEquals(Alert.Direction.EMITTED, e.direction); assertEquals(RiskReason.OTHER, e.reason); assertEquals(Role.SENDER, e.source)
        assertEquals(shortId(TEXT), e.own); assertEquals(shortId(ROGUE), e.intruder)
        val h = b.alerts[0]
        assertEquals(Alert.Direction.HEARD, h.direction); assertEquals(RiskReason.OTHER, h.reason); assertEquals(Role.SENDER, h.source); assertTrue(h.matchesMine)
        // Delivered before the warning arrived: the gate is what catches it.
        if (b.result.value() != null) assertEquals(SessionErrorCode.RISK_SIGNAL, b.r.confirmed().error()?.code)
        else assertEquals(SessionErrorCode.RISK_SIGNAL, b.result.error()?.code)
    }

    @Test fun twoFramesOverlappingOnTheSameBandAreAClashTheReceiverDeliversNothing() {
        val air = Air()
        val legit = air.device("legit"); val rogue = air.device("rogue"); val rx = air.device("receiver")
        air.gain(rogue, legit, 0.0)
        val a = sender(legit, TEXT, SenderOptions(securityLevel = SecurityLevel.BASIC, timeoutSeconds = 10.0))
        val b = receiver(rx, ReceiverOptions(securityLevel = SecurityLevel.STANDARD, ack = false))
        air.run(1.4) // basic pre-listens 1 s: the rogue starts 0.4 s into the legit frame, on the same band
        rogue.loop(loopAudio(ROGUE, Band.TOP), 8.0)
        air.run(12.0) { b.result.isDone }
        assertEquals(SessionErrorCode.MULTIPLE_TRANSMISSIONS, b.result.error()?.code)
        val e = b.alerts[0]
        assertEquals(Alert.Direction.EMITTED, e.direction); assertEquals(RiskReason.OVERLAP, e.reason); assertEquals(Role.RECEIVER, e.source)
        assertEquals("000000", e.intruder)
        assertEquals(listOf(Band.MID, Band.TOP), e.bands) // the clash band is unknown
        a.s.stop()
    }

    @Test fun aCopyOfTheSendersOwnDataOnAnotherBandIsAClone() {
        val air = Air()
        val legit = air.device("legit"); val replay = air.device("replay")
        val a = sender(legit, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD))
        air.run(2.2)
        replay.loop(loopAudio(TEXT, Band.XHIGH), 20.0)
        air.run(20.0) { a.delivered.isDone }
        assertEquals(SessionErrorCode.CLONED_TRANSMISSION, a.delivered.error()?.code)
        val e = a.alerts[0]
        assertEquals(Alert.Direction.EMITTED, e.direction); assertEquals(RiskReason.CLONE, e.reason)
        assertEquals(shortId(TEXT), e.own); assertEquals(shortId(TEXT), e.intruder)
    }

    @Test fun theConfirmGateADifferentTransmissionAfterDeliveryClosesIt() {
        val air = Air()
        val legit = air.device("legit"); val late = air.device("late"); val rx = air.device("receiver")
        val a = sender(legit, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD))
        val b = receiver(rx, ReceiverOptions(securityLevel = SecurityLevel.STANDARD))
        air.run(30.0) { a.delivered.isDone && b.result.isDone }
        assertEquals(TEXT, b.result.value()?.text)
        a.s.stop()
        late.loop(loopAudio(ROGUE, Band.TOP), 10.0)
        air.run(10.0) { b.alerts.isNotEmpty() }
        val e = b.r.confirmed().error()!!
        assertEquals(SessionErrorCode.MULTIPLE_TRANSMISSIONS, e.code)
        assertEquals(Alert.Direction.EMITTED, e.alert?.direction); assertEquals(RiskReason.OTHER, e.alert?.reason); assertEquals(Role.RECEIVER, e.alert?.source)
    }

    @Test fun preListenASenderDoesNotStartWhileAnotherTransmissionSounds() {
        val air = Air()
        val other = air.device("other"); val me = air.device("me")
        other.loop(loopAudio("somebody else", Band.TOP), 10.0)
        val a = sender(me, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD))
        air.run(8.0) { a.delivered.isDone }
        assertEquals(SessionErrorCode.OTHER_TRANSMISSION, a.delivered.error()?.code)
        assertTrue("nothing was sent, nothing to warn about", a.alerts.isEmpty())
    }

    @Test fun preListenItsOwnDataAlreadySoundingIsAClone() {
        val air = Air()
        val replay = air.device("replay"); val me = air.device("me")
        replay.loop(loopAudio(TEXT, Band.TOP), 10.0)
        val a = sender(me, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD))
        air.run(8.0) { a.delivered.isDone }
        assertEquals(SessionErrorCode.CLONED_TRANSMISSION, a.delivered.error()?.code)
    }
}

class SecurityLevelBehaviourTest {
    @Test fun basicDeliversAtTheFirstFrameAndHasNoConfirmWatch() {
        val air = Air()
        val a = sender(air.device("sender"), TEXT, SenderOptions(securityLevel = SecurityLevel.BASIC))
        var heardAt = 0.0
        val b = receiver(air.device("receiver"), ReceiverOptions(securityLevel = SecurityLevel.BASIC, ack = false, onHeard = { heardAt = air.seconds }))
        air.run(10.0) { b.result.isDone }
        assertTrue(air.seconds - heardAt < 0.1)
        assertEquals(Confirmation(Watch.OFF, 0.0, 0.0), b.r.confirmed().join())
        a.s.stop()
    }

    @Test fun standardAndMaximumWaitForTheFinalListenAndTheGateReportsAnExpiredWatch() {
        for (level in listOf(SecurityLevel.STANDARD, SecurityLevel.MAXIMUM)) {
            val air = Air()
            val a = sender(air.device("sender"), TEXT, SenderOptions(securityLevel = level, timeoutSeconds = 8.0))
            var heardAt = 0.0
            val b = receiver(air.device("receiver"), ReceiverOptions(securityLevel = level, ack = false, security = SecurityOverrides(confirmWatchSeconds = 2.0), onHeard = { heardAt = air.seconds }))
            air.run(15.0) { b.result.isDone }
            assertTrue(air.seconds - heardAt >= 1.5 - 0.03)
            air.run(3.0)
            val c = b.r.confirmed().join()
            assertEquals(Watch.EXPIRED, c.watch)
            assertEquals(2.0, c.watchedSeconds, 0.05)
            assertTrue(c.uncoveredSeconds > 0.5)
            a.s.stop()
        }
    }

    @Test fun theSenderPreListenLastsListenBeforeSendingSecondsPlusJitter() {
        for ((level, range) in listOf(Triple(SecurityLevel.BASIC, 1.0, 1.05), Triple(SecurityLevel.STANDARD, 2.0, 2.6), Triple(SecurityLevel.MAXIMUM, 3.0, 4.2)).map { it.first to (it.second to it.third) }) {
            val air = Air()
            val d = air.device("sender")
            var sendingAt = -1.0
            val a = sender(d, TEXT, SenderOptions(securityLevel = level, onState = { if (it == SenderState.SENDING && sendingAt < 0) sendingAt = d.now() }))
            air.run(5.0) { sendingAt >= 0 }
            assertTrue("$level $sendingAt", sendingAt >= range.first && sendingAt <= range.second)
            a.s.stop()
        }
    }

    @Test fun maximumIsTheDefault() {
        val air = Air()
        val a = SenderSession(TEXT, air.device("d"))
        assertEquals(resolveSecurity(SecurityLevel.MAXIMUM), a.security)
        assertEquals(resolveSecurity(SecurityLevel.MAXIMUM), ReceiverSession(air.device("e")).security)
    }
}

class RiskSignalHandlingTest {
    @Test fun quarantineIsOnlyACallbackTheNextSessionOnTheSameDeviceRunsNormally() {
        val air = Air()
        val alarm = air.device("alarm"); val me = air.device("me"); val rx = air.device("receiver")
        air.gain(alarm, rx, 0.0)
        alarm.loop(riskLoop(0), 3.0)
        val memory = RiskMemory()
        val first = sender(me, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD, memory = memory))
        air.run(8.0) { first.delivered.isDone }
        assertEquals(SessionErrorCode.RISK_SIGNAL, first.delivered.error()?.code)
        assertEquals(listOf(QuarantineNotice(30000, RiskReason.OTHER, Role.RECEIVER)), first.quarantines)
        first.s.stop() // stop relaying
        air.run(1.0)
        val second = SenderSession(TEXT, me, SenderOptions(securityLevel = SecurityLevel.STANDARD, memory = memory, calibration = MemoryCalibrationStore()))
        me.listen { second.hear(it) }
        second.start()
        val b = receiver(rx, ReceiverOptions(securityLevel = SecurityLevel.STANDARD))
        air.run(25.0) { second.delivered.isDone }
        assertEquals(Band.TOP, second.delivered.value()?.band)
        assertEquals(TEXT, b.result.value()?.text)
    }

    @Test fun honorFalseTheAlertIsReportedButNothingStops() {
        val air = Air()
        val alarm = air.device("alarm"); val me = air.device("me")
        alarm.loop(riskLoop(0), 3.0)
        val a = sender(me, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD, security = SecurityOverrides(riskSignal = RiskSignalOverrides(honor = false)), timeoutSeconds = 6.0))
        air.run(10.0) { a.delivered.isDone }
        assertEquals(listOf(Alert.Direction.HEARD), a.alerts.map { it.direction })
        assertEquals(SessionErrorCode.TIMEOUT, a.delivered.error()?.code)
    }

    @Test fun relaysOneHopFurtherAndNeverPastTwoHops() {
        for ((hops, relayed) in listOf(0 to 1, 1 to 2, 2 to null)) {
            val air = Air()
            val alarm = air.device("alarm"); val me = air.device("me")
            alarm.loop(riskLoop(hops), 2.5)
            val a = sender(me, TEXT, SenderOptions(securityLevel = SecurityLevel.STANDARD))
            air.run(8.0) { a.delivered.isDone }
            assertEquals(SessionErrorCode.RISK_SIGNAL, a.delivered.error()?.code)
            val emitted = a.alerts.filter { it.direction == Alert.Direction.EMITTED }
            assertEquals(listOfNotNull(relayed), emitted.map { it.hops })
            a.s.stop()
        }
    }

    @Test fun basicDoesNotListenForRiskFrames() {
        val air = Air()
        val alarm = air.device("alarm"); val me = air.device("me")
        alarm.loop(riskLoop(0), 3.0)
        val a = sender(me, TEXT, SenderOptions(securityLevel = SecurityLevel.BASIC, timeoutSeconds = 4.0))
        air.run(6.0) { a.delivered.isDone }
        assertTrue(a.alerts.isEmpty())
        assertEquals(SessionErrorCode.TIMEOUT, a.delivered.error()?.code)
    }

    @Test fun aReceiverBeforeDeliveryFailsWithRiskSignalAndNeverRelays() {
        val air = Air()
        val alarm = air.device("alarm"); val rx = air.device("receiver")
        alarm.loop(riskLoop(0), 3.0)
        val b = receiver(rx, ReceiverOptions(securityLevel = SecurityLevel.STANDARD))
        air.run(8.0) { b.result.isDone }
        assertEquals(SessionErrorCode.RISK_SIGNAL, b.result.error()?.code)
        assertEquals(listOf(Alert.Direction.HEARD), b.alerts.map { it.direction })
    }
}

class ReferenceModeTest {
    @Test fun theReceiverResolvesTheFingerprintWithALocalCatalogAndChecksIt() {
        val resolver = catalogResolver(listOf(TEXT, "another text"))
        val air = Air()
        val a = sender(air.device("sender"), TEXT, SenderOptions(mode = Mode.REFERENCE, securityLevel = SecurityLevel.STANDARD))
        val b = receiver(air.device("receiver"), ReceiverOptions(securityLevel = SecurityLevel.STANDARD, resolve = resolver))
        air.run(25.0) { a.delivered.isDone && b.result.isDone }
        assertEquals(Received(TEXT, Band.TOP, Mode.REFERENCE), b.result.value())
        assertEquals(Band.TOP, a.delivered.value()?.band)
        a.s.stop()
    }

    @Test fun unknownFingerprintWrongTextNoResolver() {
        val cases = listOf<Pair<Resolver?, SessionErrorCode>>(
            Resolver { null } to SessionErrorCode.NOT_FOUND,
            Resolver { "something else" } to SessionErrorCode.MISMATCH,
            Resolver { throw IllegalStateException("boom") } to SessionErrorCode.NOT_FOUND,
            null to SessionErrorCode.NO_RESOLVER,
        )
        for ((resolve, code) in cases) {
            val air = Air()
            val a = sender(air.device("sender"), TEXT, SenderOptions(mode = Mode.REFERENCE, securityLevel = SecurityLevel.BASIC))
            val b = receiver(air.device("receiver"), ReceiverOptions(securityLevel = SecurityLevel.BASIC, resolve = resolve))
            air.run(10.0) { b.result.isDone }
            assertEquals(code, b.result.error()?.code)
            assertTrue("these failures emit no risk frame", b.alerts.isEmpty())
            a.s.stop()
        }
    }

    @Test fun anAsyncResolverMayAnswerLaterAndATooSlowOneTimesOut() {
        val air = Air()
        val a = sender(air.device("sender"), TEXT, SenderOptions(mode = Mode.REFERENCE, securityLevel = SecurityLevel.STANDARD))
        val slow = java.util.concurrent.CompletableFuture<String?>()
        val b = receiver(air.device("receiver"), ReceiverOptions(securityLevel = SecurityLevel.STANDARD, resolveAsync = { slow }))
        air.run(8.0)
        assertFalse(b.result.isDone)
        slow.complete(TEXT)
        air.run(5.0) { b.result.isDone }
        assertEquals(TEXT, b.result.value()?.text)
        a.s.stop()

        val air2 = Air()
        val a2 = sender(air2.device("sender"), TEXT, SenderOptions(mode = Mode.REFERENCE, securityLevel = SecurityLevel.BASIC, timeoutSeconds = 40.0))
        val b2 = receiver(air2.device("receiver"), ReceiverOptions(securityLevel = SecurityLevel.BASIC, resolveAsync = { java.util.concurrent.CompletableFuture() }))
        air2.run(30.0) { b2.result.isDone }
        assertEquals(SessionErrorCode.TIMEOUT, b2.result.error()?.code)
        a2.s.stop()
    }
}

class CancelTest {
    @Test fun cancelBeforeDeliveryRejectsTheResultNeverThrowsAndConfirmedThenFailsWithCancelled() {
        val air = Air()
        val b = receiver(air.device("receiver"))
        air.run(0.5)
        b.r.cancel()
        b.r.cancel()
        air.run(0.1)
        assertEquals(SessionErrorCode.CANCELLED, b.result.error()?.code)
        assertEquals(SessionErrorCode.CANCELLED, b.r.confirmed().error()?.code)
    }

    @Test fun stopBeforeTheAckFailsWithStopped() {
        val air = Air()
        val a = sender(air.device("sender"), TEXT)
        air.run(1.0)
        a.s.stop()
        assertEquals(SessionErrorCode.STOPPED, a.delivered.error()?.code)
        assertTrue(a.s.finished.isDone)
    }
}
