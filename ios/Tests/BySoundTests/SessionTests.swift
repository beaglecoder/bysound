// The session layer on simulated air: ports of web/src/session/session.test.ts and adapt.test.ts.
import XCTest
@testable import BySound

private let TEXT = "pay 12.50 to shop 7"

private final class SenderRig {
    var alerts: [Alert] = []
    var quarantines: [QuarantineNotice] = []
    var reps: [(index: Int, band: Band, profile: Profile)] = []
    var routes: [[Band]] = []
    var session: SenderSession!
    var delivered: Settled<Delivery> { session.delivered }
    var states: [SenderState] = []
}
private final class ReceiverRig {
    var alerts: [Alert] = []
    var quarantines: [QuarantineNotice] = []
    var heardAt: Double?
    var session: ReceiverSession!
    var result: Settled<Received> { session.result }
}

private func sender(_ d: Device, _ text: String = TEXT, robust: Bool = false, _ configure: (inout SenderOptions) -> Void = { _ in }) -> SenderRig {
    let rig = SenderRig()
    var o = SenderOptions()
    o.memory = RiskMemory()
    o.onAlert = { rig.alerts.append($0) }
    o.onQuarantine = { rig.quarantines.append($0) }
    o.onRepetition = { rig.reps.append(($0, $1, $2)) }
    o.onAmbient = { _, route in rig.routes.append(route) }
    o.onState = { rig.states.append($0) }
    configure(&o)
    rig.session = try! SenderSession(text: text, platform: d, options: o)
    d.listen(robust: robust) { [unowned rig] e in rig.session.hear(e) }
    rig.session.start()
    return rig
}

private func receiver(_ d: Device, robust: Bool = false, _ configure: (inout ReceiverOptions) -> Void = { _ in }) -> ReceiverRig {
    let rig = ReceiverRig()
    var o = ReceiverOptions()
    o.memory = RiskMemory()
    o.onAlert = { rig.alerts.append($0) }
    o.onQuarantine = { rig.quarantines.append($0) }
    o.onHeard = { [unowned d] _ in rig.heardAt = d.now() }
    configure(&o)
    rig.session = ReceiverSession(platform: d, options: o)
    d.listen(robust: robust) { [unowned rig] e in rig.session.hear(e) }
    rig.session.start()
    return rig
}

private func loopAudio(_ text: String, _ band: Band) -> [Float] { try! Modem.encode(text: text, band: band, sampleRate: 48000) }
private func riskLoop(hops: Int) -> [Float] {
    let f = Frames.risk(RiskFrame(reason: .other, source: .receiver, hops: hops, own: Ids.shortId("x"), intruder: Ids.shortId("y")), band: .mid)
    return Modem.render(f.symbols, f.channel, rate: 48000, gapSeconds: Frames.controlGapSeconds)
}

final class SessionTests: XCTestCase {
    // MARK: ack and receipt

    func testAckStopsTheSenderReceiptReachesTheReceiverGateIsClean() async throws {
        let air = Air()
        let a = sender(air.device("sender"))
        let b = receiver(air.device("receiver"))
        air.run(30) { a.delivered.isSettled && b.result.isSettled }
        XCTAssertEqual(b.result.success, Received(text: TEXT, band: .top, via: .payload))
        XCTAssertEqual(a.delivered.success?.band, .top)
        XCTAssertEqual(a.session.state, .receipt) // the payload stopped; it is answering with receipts
        air.run(25) { b.session.receipt.isSettled && a.session.state == .done }
        XCTAssertEqual(b.session.receipt.success, true)
        XCTAssertEqual(a.session.state, .done)
        let c = try await b.session.confirmed()
        XCTAssertEqual(c.watch, .clean)
        XCTAssertGreaterThan(c.watchedSeconds, 0)
        XCTAssertTrue(a.alerts.isEmpty)
        XCTAssertTrue(b.alerts.isEmpty)
    }

    func testASenderWithNobodyAckingTimesOut() {
        let air = Air()
        let a = sender(air.device("sender")) { $0.securityLevel = .basic; $0.timeoutSeconds = 6 }
        air.run(10) { a.delivered.isSettled }
        XCTAssertEqual(a.delivered.failure?.code, .timeout)
    }

    // MARK: two senders

    func testReceiverHearsASecondSenderRejectsAndSoundsARiskFrameTheRealSenderStopsAndRelays() {
        let air = Air()
        let legit = air.device("legit"), rogue = air.device("rogue"), rx = air.device("receiver")
        air.gain(rogue, legit, 0) // they cannot hear each other: only the receiver can warn the legit sender
        let a = sender(legit) { $0.securityLevel = .standard }
        let b = receiver(rx) { $0.securityLevel = .standard }
        air.run(2.5)
        rogue.loop(loopAudio("pay 99.00 to shop 666", .high), seconds: 20)
        air.run(30) { a.delivered.isSettled && b.result.isSettled }
        XCTAssertEqual(b.result.failure?.code, .multipleTransmissions)
        XCTAssertEqual(b.alerts.first?.direction, .emitted)
        XCTAssertEqual(b.alerts.first?.source, .receiver)
        XCTAssertTrue([.overlap, .other].contains(b.alerts.first?.reason))
        XCTAssertEqual(a.delivered.failure?.code, .riskSignal)
        XCTAssertEqual(a.delivered.failure?.alert?.direction, .heard)
        XCTAssertEqual(a.delivered.failure?.alert?.source, .receiver)
        XCTAssertEqual(a.delivered.failure?.alert?.hops, 0)
        let relay = a.alerts.first { $0.direction == .emitted }
        XCTAssertEqual(relay?.relay, true)
        XCTAssertEqual(relay?.hops, 1)
        XCTAssertEqual(relay?.source, .receiver)
        XCTAssertEqual(b.quarantines.first?.durationMs, 30000)
        XCTAssertEqual(a.quarantines.first?.durationMs, 30000)
    }

    func testASenderThatHearsAnotherTransmissionInItsWindowStopsAndWarnsTheReceiver() async throws {
        let air = Air()
        let legit = air.device("legit"), rogue = air.device("rogue"), rx = air.device("receiver")
        air.gain(rogue, rx, 0) // the receiver cannot hear the rogue: only the sender can
        let a = sender(legit) { $0.securityLevel = .standard }
        let b = receiver(rx) { $0.securityLevel = .standard; $0.ack = false }
        air.run(2.2)
        rogue.loop(loopAudio("pay 99.00 to shop 666", .xhigh), seconds: 30)
        air.run(30) { a.delivered.isSettled && b.result.isSettled && !b.alerts.isEmpty }
        XCTAssertEqual(a.delivered.failure?.code, .otherEmitter)
        let mine = a.alerts.first
        XCTAssertEqual(mine?.direction, .emitted)
        XCTAssertEqual(mine?.reason, .other)
        XCTAssertEqual(mine?.source, .sender)
        XCTAssertEqual(mine?.own, Ids.shortId(TEXT))
        XCTAssertEqual(mine?.intruder, Ids.shortId("pay 99.00 to shop 666"))
        let theirs = b.alerts.first
        XCTAssertEqual(theirs?.direction, .heard)
        XCTAssertEqual(theirs?.reason, .other)
        XCTAssertEqual(theirs?.source, .sender)
        XCTAssertEqual(theirs?.matchesMine, true)
        // Delivery before the warning arrived: the gate is what catches it.
        if b.result.success != nil {
            do { _ = try await b.session.confirmed(); XCTFail("the gate must be closed") } catch let e as SessionError { XCTAssertEqual(e.code, .riskSignal) }
        } else {
            XCTAssertEqual(b.result.failure?.code, .riskSignal)
        }
    }

    func testTwoFramesOverlappingOnTheSameBandAreAClash() {
        let air = Air()
        let legit = air.device("legit"), rogue = air.device("rogue"), rx = air.device("receiver")
        air.gain(rogue, legit, 0)
        let a = sender(legit) { $0.securityLevel = .basic; $0.timeoutSeconds = 10 }
        let b = receiver(rx) { $0.securityLevel = .standard; $0.ack = false }
        air.run(1.4) // basic pre-listens 1 s: the rogue starts 0.4 s into the legit frame, on the same band
        rogue.loop(loopAudio("pay 99.00 to shop 666", .top), seconds: 8)
        air.run(12) { b.result.isSettled }
        XCTAssertEqual(b.result.failure?.code, .multipleTransmissions)
        XCTAssertEqual(b.alerts.first?.direction, .emitted)
        XCTAssertEqual(b.alerts.first?.reason, .overlap)
        XCTAssertEqual(b.alerts.first?.intruder, "000000")
        XCTAssertEqual(b.alerts.first?.bands, [.mid, .top]) // the clash band is unknown
        a.session.stop()
    }

    func testACopyOfTheSendersOwnDataOnAnotherBandIsAClone() {
        let air = Air()
        let legit = air.device("legit"), replay = air.device("replay")
        let a = sender(legit) { $0.securityLevel = .standard }
        air.run(2.2)
        replay.loop(loopAudio(TEXT, .xhigh), seconds: 20)
        air.run(20) { a.delivered.isSettled }
        XCTAssertEqual(a.delivered.failure?.code, .clonedTransmission)
        XCTAssertEqual(a.alerts.first?.direction, .emitted)
        XCTAssertEqual(a.alerts.first?.reason, .clone)
        XCTAssertEqual(a.alerts.first?.own, Ids.shortId(TEXT))
        XCTAssertEqual(a.alerts.first?.intruder, Ids.shortId(TEXT))
    }

    func testTheConfirmGateADifferentTransmissionAfterDeliveryClosesIt() async throws {
        let air = Air()
        let legit = air.device("legit"), late = air.device("late"), rx = air.device("receiver")
        let a = sender(legit) { $0.securityLevel = .standard }
        let b = receiver(rx) { $0.securityLevel = .standard }
        air.run(30) { a.delivered.isSettled && b.result.isSettled }
        XCTAssertEqual(b.result.success?.text, TEXT)
        a.session.stop()
        late.loop(loopAudio("pay 99.00 to shop 666", .top), seconds: 10)
        air.run(10) { !b.alerts.isEmpty }
        do { _ = try await b.session.confirmed(); XCTFail("the gate must be closed") } catch let e as SessionError {
            XCTAssertEqual(e.code, .multipleTransmissions)
            XCTAssertEqual(e.alert?.direction, .emitted)
            XCTAssertEqual(e.alert?.reason, .other)
            XCTAssertEqual(e.alert?.source, .receiver)
        }
    }

    func testPreListenASenderDoesNotStartWhileAnotherTransmissionSounds() {
        let air = Air()
        let other = air.device("other"), me = air.device("me")
        other.loop(loopAudio("somebody else", .top), seconds: 10)
        let a = sender(me) { $0.securityLevel = .standard }
        air.run(8) { a.delivered.isSettled }
        XCTAssertEqual(a.delivered.failure?.code, .otherTransmission)
        XCTAssertTrue(a.alerts.isEmpty) // nothing was sent, nothing to warn about
    }

    func testPreListenItsOwnDataAlreadySoundingIsAClone() {
        let air = Air()
        let replay = air.device("replay"), me = air.device("me")
        replay.loop(loopAudio(TEXT, .top), seconds: 10)
        let a = sender(me) { $0.securityLevel = .standard }
        air.run(8) { a.delivered.isSettled }
        XCTAssertEqual(a.delivered.failure?.code, .clonedTransmission)
    }

    // MARK: security levels

    func testBasicDeliversAtTheFirstFrameAndHasNoConfirmWatch() async throws {
        let air = Air()
        let a = sender(air.device("sender")) { $0.securityLevel = .basic }
        let b = receiver(air.device("receiver")) { $0.securityLevel = .basic; $0.ack = false }
        air.run(10) { b.result.isSettled }
        XCTAssertLessThan(air.devices[1].now() - b.heardAt!, 0.1)
        let c = try await b.session.confirmed()
        XCTAssertEqual(c, Confirmation(watch: .off, watchedSeconds: 0, uncoveredSeconds: 0))
        a.session.stop()
    }

    func testStandardAndMaximumWaitForTheFinalListenAndTheGateReportsAnExpiredWatch() async throws {
        for level in [SecurityLevel.standard, .maximum] {
            let air = Air()
            let a = sender(air.device("sender")) { $0.securityLevel = level; $0.timeoutSeconds = 8 }
            let b = receiver(air.device("receiver")) { $0.securityLevel = level; $0.ack = false; $0.security.confirmWatchSeconds = 2 }
            air.run(15) { b.result.isSettled }
            XCTAssertGreaterThanOrEqual(air.devices[1].now() - b.heardAt!, 1.5 - 0.03, "\(level)")
            air.run(3)
            let c = try await b.session.confirmed()
            XCTAssertEqual(c.watch, .expired)
            XCTAssertEqual(c.watchedSeconds, 2, accuracy: 0.05)
            XCTAssertGreaterThan(c.uncoveredSeconds, 0.5)
            a.session.stop()
        }
    }

    func testTheSenderPreListenLastsListenBeforeSendingSecondsPlusJitter() {
        for (level, lo, hi) in [(SecurityLevel.basic, 1.0, 1.05), (.standard, 2.0, 2.6), (.maximum, 3.0, 4.2)] {
            let air = Air()
            let d = air.device("sender")
            var sendingAt = -1.0
            let a = sender(d) { o in
                o.securityLevel = level
                let prior = o.onState
                o.onState = { s in prior?(s); if s == .sending && sendingAt < 0 { sendingAt = d.now() } }
            }
            air.run(5) { sendingAt >= 0 }
            XCTAssertGreaterThanOrEqual(sendingAt, lo, "\(level)")
            XCTAssertLessThanOrEqual(sendingAt, hi, "\(level)")
            a.session.stop()
        }
    }

    // MARK: risk signal handling

    func testQuarantineIsOnlyACallbackTheNextSessionOnTheSameDeviceRunsNormally() {
        let air = Air()
        let alarm = air.device("alarm"), me = air.device("me"), rx = air.device("receiver")
        air.gain(alarm, rx, 0)
        alarm.loop(riskLoop(hops: 0), seconds: 3)
        let memory = RiskMemory()
        let first = sender(me) { $0.securityLevel = .standard; $0.memory = memory }
        air.run(8) { first.delivered.isSettled }
        XCTAssertEqual(first.delivered.failure?.code, .riskSignal)
        XCTAssertEqual(first.quarantines, [QuarantineNotice(durationMs: 30000, reason: .other, source: .receiver)])
        first.session.stop() // stop relaying
        air.run(1)
        let second = sender(me) { $0.securityLevel = .standard; $0.memory = memory }
        let b = receiver(rx) { $0.securityLevel = .standard }
        air.run(25) { second.delivered.isSettled }
        XCTAssertEqual(second.delivered.success?.band, .top)
        XCTAssertEqual(b.result.success?.text, TEXT)
    }

    func testHonorFalseTheAlertIsReportedButNothingStops() {
        let air = Air()
        let alarm = air.device("alarm"), me = air.device("me")
        alarm.loop(riskLoop(hops: 0), seconds: 3)
        let a = sender(me) { $0.securityLevel = .standard; $0.security.riskSignal.honor = false; $0.timeoutSeconds = 6 }
        air.run(10) { a.delivered.isSettled }
        XCTAssertEqual(a.alerts.map { $0.direction }, [.heard])
        XCTAssertEqual(a.delivered.failure?.code, .timeout)
    }

    func testRelaysOneHopFurtherAndNeverPastTwoHops() {
        for (hops, relayed) in [(0, 1), (1, 2), (2, nil)] as [(Int, Int?)] {
            let air = Air()
            let alarm = air.device("alarm"), me = air.device("me")
            alarm.loop(riskLoop(hops: hops), seconds: 2.5)
            let a = sender(me) { $0.securityLevel = .standard }
            air.run(8) { a.delivered.isSettled }
            XCTAssertEqual(a.delivered.failure?.code, .riskSignal)
            XCTAssertEqual(a.alerts.filter { $0.direction == .emitted }.map { $0.hops }, relayed.map { [$0] } ?? [], "hops \(hops)")
            a.session.stop()
        }
    }

    func testBasicDoesNotListenForRiskFrames() {
        let air = Air()
        let alarm = air.device("alarm"), me = air.device("me")
        alarm.loop(riskLoop(hops: 0), seconds: 3)
        let a = sender(me) { $0.securityLevel = .basic; $0.timeoutSeconds = 4 }
        air.run(6) { a.delivered.isSettled }
        XCTAssertTrue(a.alerts.isEmpty)
        XCTAssertEqual(a.delivered.failure?.code, .timeout)
    }

    // MARK: reference mode

    func testTheReceiverResolvesTheFingerprintWithALocalMapAndChecksIt() {
        let catalog = [Ids.fingerprint(TEXT): TEXT]
        let air = Air()
        let a = sender(air.device("sender")) { $0.mode = .reference; $0.securityLevel = .standard }
        let b = receiver(air.device("receiver")) { $0.securityLevel = .standard; $0.resolve(from: catalog) }
        air.run(25) { a.delivered.isSettled && b.result.isSettled }
        XCTAssertEqual(b.result.success, Received(text: TEXT, band: .top, via: .reference))
        XCTAssertEqual(a.delivered.success?.band, .top)
    }

    func testReferenceUnknownFingerprintWrongTextNoResolver() {
        let cases: [(((String, @escaping (String?) -> Void) -> Void)?, SessionErrorCode)] = [
            ({ _, done in done(nil) }, .notFound), ({ _, done in done("something else") }, .mismatch), (nil, .noResolver),
        ]
        for (resolve, code) in cases {
            let air = Air()
            let a = sender(air.device("sender")) { $0.mode = .reference; $0.securityLevel = .basic }
            let b = receiver(air.device("receiver")) { $0.securityLevel = .basic; $0.resolve = resolve }
            air.run(10) { b.result.isSettled }
            XCTAssertEqual(b.result.failure?.code, code)
            a.session.stop()
        }
    }

    func testAnAsynchronousResolverDeliversWhenItAnswersAndTimesOutOtherwise() {
        let air = Air()
        let rx = air.device("receiver")
        let a = sender(air.device("sender")) { $0.mode = .reference; $0.securityLevel = .basic }
        let b = receiver(rx) { o in
            o.securityLevel = .basic
            o.resolve = { fp, done in _ = rx.after(3) { done(Ids.fingerprint(TEXT) == fp ? TEXT : nil) } }
        }
        air.run(25) { b.result.isSettled }
        XCTAssertEqual(b.result.success?.text, TEXT)
        a.session.stop()

        let air2 = Air()
        let a2 = sender(air2.device("sender")) { $0.mode = .reference; $0.securityLevel = .basic }
        let b2 = receiver(air2.device("receiver")) { $0.securityLevel = .basic; $0.resolve = { _, _ in } } // never answers
        air2.run(25) { b2.result.isSettled }
        XCTAssertEqual(b2.result.failure?.code, .timeout)
        a2.session.stop()
    }

    // MARK: cancel

    func testCancelBeforeDeliveryRejectsTheResultAndNeverThrowsConfirmedThenFailsWithCancelled() async throws {
        let air = Air()
        let b = receiver(air.device("receiver"))
        air.run(0.5)
        b.session.cancel()
        b.session.cancel()
        air.run(0.1)
        XCTAssertEqual(b.result.failure?.code, .cancelled)
        do { _ = try await b.session.confirmed(); XCTFail() } catch let e as SessionError { XCTAssertEqual(e.code, .cancelled) }
    }

    // MARK: automatic adaptation

    func testAPhoneWithABrokenTweeterClimbsDownToMidRemembersItAndTheNextSendIsReadAtOnce() {
        let air = Air()
        let phone = air.device("phone"), rx = air.device("receiver")
        phone.speaker = brokenTweeter(17_300) // only mid gets through
        let store = MemoryCalibrationStore()
        let first = sender(phone) { $0.securityLevel = .standard; $0.calibration = .store(store) }
        var r = receiver(rx, robust: true) { $0.securityLevel = .standard }
        air.run(70) { first.delivered.isSettled }
        XCTAssertEqual(first.delivered.success?.band, .mid)
        XCTAssertEqual(first.delivered.success?.stated, true)
        XCTAssertEqual(first.reps.prefix(3).map { $0.band }, [.top, .top, .top])
        XCTAssertEqual(Set(first.reps.map { $0.band }), [.top, .high, .upper, .mid])
        XCTAssertTrue(first.reps.contains { $0.profile == .robust }) // nobody read the first 6: the ladder went robust
        XCTAssertEqual(store.load(), Calibration(band: .mid, profile: .normal, uses: 0)) // robust by the ladder is not the speaker's
        first.session.stop()
        // No receipt reaches the receiver (receipts are on high), so it acks for ackTotalSeconds; the next pre-listen would measure
        // those acks as noise on mid. Wait until it is quiet.
        r.session.cancel()
        air.run(40) { r.session.finished.isSettled }

        let second = sender(phone) { $0.securityLevel = .standard; $0.calibration = .store(store) }
        r = receiver(rx, robust: true) { $0.securityLevel = .standard }
        air.run(30) { second.delivered.isSettled }
        XCTAssertEqual(second.reps.first?.band, .mid)
        XCTAssertEqual(second.reps.first?.profile, .normal)
        XCTAssertEqual(second.delivered.success?.band, .mid)
        XCTAssertEqual(second.delivered.success?.repetitions, 1)
        XCTAssertEqual(store.load(), Calibration(band: .mid, profile: .normal, uses: 0))
        second.session.stop()
    }

    func testSkipsChannelsTheAmbientCheckFindsNoisy() {
        let air = Air()
        let fan = air.device("fan"), phone = air.device("phone")
        _ = fan.play(hiss(fromHz: 18_900, toHz: 19_500, amplitude: 0.02, seconds: 12)) {} // tones only top and high cover
        let a = sender(phone) { $0.securityLevel = .standard; $0.calibration = .disabled; $0.timeoutSeconds = 3 }
        air.run(8) { a.delivered.isSettled }
        XCTAssertEqual(a.routes.first, [.upper, .mid])
        XCTAssertEqual(a.reps.first?.band, .upper)
    }

    func testANoisyPlaceStartsRobustAnExplicitProfileWins() {
        for (profile, expected) in [(nil, Profile.robust), (Profile.normal, .normal)] as [(Profile?, Profile)] {
            let air = Air(noise: 0.05) // white noise about -40 dB per tone: over robustNoiseDb on every channel
            let a = sender(air.device("phone")) { $0.securityLevel = .basic; $0.calibration = .disabled; $0.timeoutSeconds = 2; $0.profile = profile }
            air.run(5) { a.delivered.isSettled }
            XCTAssertEqual(a.reps.first?.profile, expected)
        }
    }

    func testACalibrationThatKeepsFailingExpiresAfterCalibrationUsesSends() {
        let store = MemoryCalibrationStore(Calibration(band: .upper, profile: .normal, uses: Adapt.calibrationUses - 1))
        let air = Air()
        let a = sender(air.device("phone")) { $0.securityLevel = .basic; $0.calibration = .store(store); $0.timeoutSeconds = 2 }
        air.run(5) { a.delivered.isSettled }
        XCTAssertEqual(a.reps.first?.band, .upper)
        XCTAssertEqual(store.load()?.uses, Adapt.calibrationUses)
        let b = sender(air.device("phone2")) { $0.securityLevel = .basic; $0.calibration = .store(store); $0.timeoutSeconds = 2 }
        air.run(5) { b.delivered.isSettled }
        XCTAssertEqual(b.reps.first?.band, .top)
    }

    // MARK: length limits

    /// A clock and a speaker without air: timers and sound ends run in time order when `drain` is called.
    private final class Clock: SessionPlatform {
        private var due: [(at: Double, fn: () -> Void, id: Int)] = []
        private var cancelled = Set<Int>()
        private var next = 0
        var t = 0.0
        var played: [Int] = []
        let sampleRate = 48000.0
        func now() -> Double { t }
        func after(_ seconds: Double, _ fn: @escaping () -> Void) -> Cancel {
            let id = next
            next += 1
            due.append((t + seconds, fn, id))
            return { [weak self] in self?.cancelled.insert(id) }
        }
        func play(_ audio: [Float], onEnd: @escaping () -> Void) -> Cancel {
            played.append(audio.count)
            return after(Double(audio.count) / sampleRate, onEnd)
        }
        func random() -> Double { 0 }
        func drain(until: () -> Bool) {
            while !until() {
                due.removeAll { cancelled.contains($0.id) }
                guard let i = due.indices.min(by: { due[$0].at < due[$1].at }) else { return }
                let d = due.remove(at: i)
                t = d.at
                d.fn()
            }
        }
    }

    func testATextTooLongForTopRobustIsPlayedNormalAndStillTimesItsAcks() throws {
        let text = String(repeating: "a", count: Modem.maxPayloadBytes(band: .top)) // fits top/normal, not top/robust
        let l = longestRepetitionSeconds(text: text)
        XCTAssertGreaterThan(l, Double(Protocol.maxFrameSeconds))
        XCTAssertEqual(ackRunSeconds(repetition: l), ackRunMaxSeconds)
        // The robust ladder would kick in after six repetitions: the text does not fit robust there, so it stays normal.
        let clock = Clock()
        var reps: [Profile] = []
        var o = SenderOptions()
        o.band = .top; o.securityLevel = .basic; o.ambient = false; o.memory = RiskMemory(); o.timeoutSeconds = 1000
        o.onRepetition = { _, _, p in reps.append(p) }
        let s = try SenderSession(text: text, platform: clock, options: o)
        s.start()
        clock.drain { reps.count >= 9 }
        XCTAssertEqual(reps, Array(repeating: .normal, count: 9))
        XCTAssertTrue(clock.played.allSatisfy { $0 == clock.played[0] && $0 > 0 })
        // Checked up front: too long for the band (or for a fixed robust profile, or for one channel of the ladder), or empty.
        var robust = SenderOptions(); robust.band = .top; robust.profile = .robust
        var ladder = SenderOptions(); ladder.channels = [.mid, .top]
        var fixed = SenderOptions(); fixed.band = .top
        for (t, opts) in [(text + "a", fixed), (text, robust), (text + "a", ladder)] {
            XCTAssertThrowsError(try SenderSession(text: t, platform: clock, options: opts)) { e in
                guard case ModemError.exceedsMaxDuration = e else { return XCTFail("wrong error \(e)") }
            }
        }
        XCTAssertThrowsError(try SenderSession(text: "", platform: clock)) { e in
            guard case ModemError.emptyText = e else { return XCTFail("wrong error \(e)") }
        }
    }
}
