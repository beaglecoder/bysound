import XCTest
@testable import BySound

private func vectors(_ name: String) throws -> [[String: Any]] {
    let json = try JSONSerialization.jsonObject(with: Data(contentsOf: Repo.url("test-vectors/\(name)"))) as! [String: Any]
    return json["vectors"] as! [[String: Any]]
}
private func hexString(_ a: [UInt8]) -> String { a.map { String(format: "%02x", $0) }.joined() }

final class SessionFramesTests: XCTestCase {
    func testSha256Ids() {
        // SHA-256("abc") = ba7816bf 8f01cfea ...
        XCTAssertEqual(Ids.payloadId("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        XCTAssertEqual(Ids.fingerprint("abc"), "ba7816bf8f01cfea414140de")
        XCTAssertEqual(Ids.ackId("abc"), "ba7816bf")
        XCTAssertEqual(Ids.shortId("abc"), "ba7816")
        XCTAssertEqual(Ids.payloadId("ñandú 🙂").count, 64)
    }

    func testAckVectors() throws {
        var n = 0
        for v in try vectors("v4-acks.json") {
            let band = Band(rawValue: v["band"] as! String)!
            let ack = AckFrame(receipt: v["receipt"] as! Bool, id: Ids.ackId(v["text"] as! String))
            let f = Frames.ack(ack, band: band)
            XCTAssertEqual(hexString(f.header), v["header"] as? String)
            XCTAssertEqual(hexString(f.body), v["body"] as? String)
            XCTAssertEqual(f.symbols, v["symbols"] as! [[Int]])
            XCTAssertEqual(f.frameSeconds, (v["frameSeconds"] as! NSNumber).doubleValue, accuracy: 1e-6)
            XCTAssertEqual(Frames.parseAck(Array(f.body.prefix(7))), ack)
            n += 1
        }
        XCTAssertGreaterThan(n, 0)
    }

    func testSessionAckVectors() throws {
        var n = 0
        for v in try vectors("session-acks.json") {
            let band = Band(rawValue: v["band"] as! String)!
            var heard: ReadAt?
            if let h = v["heard"] as? [String: Any] { heard = ReadAt(band: Band(rawValue: h["band"] as! String)!, profile: Profile(rawValue: h["profile"] as! String)!) }
            let ack = AckFrame(receipt: v["receipt"] as! Bool, id: Ids.ackId(v["text"] as! String), heard: heard)
            let f = Frames.ack(ack, band: band)
            XCTAssertEqual(hexString(f.header), v["header"] as? String)
            XCTAssertEqual(hexString(f.body), v["body"] as? String)
            XCTAssertEqual(f.symbols, v["symbols"] as! [[Int]])
            XCTAssertEqual(Frames.parseAck(Array(f.body.prefix(7))), ack)
            n += 1
        }
        XCTAssertGreaterThan(n, 3)
    }

    func testRejectsRobustFlagWithoutBand() {
        func with(_ b0: UInt8) -> AckFrame? { let d: [UInt8] = [b0, 0, 0, 0, 0]; let c = crc16(d); return Frames.parseAck(d + [UInt8(c >> 8), UInt8(c & 255)]) }
        XCTAssertNil(with(0x82))
        XCTAssertEqual(with(0x72), AckFrame(receipt: false, id: "00000000", heard: ReadAt(band: .wide, profile: .normal)))
    }

    func testRiskVectors() throws {
        var n = 0
        for v in try vectors("v4-risk.json") {
            let band = Band(rawValue: v["band"] as! String)!
            let r = RiskFrame(reason: RiskReason(rawValue: v["reason"] as! Int)!, source: (v["source"] as! Int) == 1 ? .receiver : .sender, hops: v["hops"] as! Int,
                              own: hexString((v["own"] as! [Int]).map { UInt8($0) }), intruder: hexString((v["intruder"] as! [Int]).map { UInt8($0) }))
            let f = Frames.risk(r, band: band)
            XCTAssertEqual(hexString(f.header), v["header"] as? String)
            XCTAssertEqual(hexString(f.body), v["body"] as? String)
            XCTAssertEqual(f.symbols, v["symbols"] as! [[Int]])
            XCTAssertEqual(f.frameSeconds, (v["frameSeconds"] as! NSNumber).doubleValue, accuracy: 1e-6)
            XCTAssertEqual(Frames.parseRisk(Array(f.body.prefix(10))), r)
            n += 1
        }
        XCTAssertGreaterThan(n, 3)
    }

    func testReferenceVectors() throws {
        var n = 0
        for v in try vectors("v4-frames.json") where v["mode"] as? String == "reference" {
            let band = Band(rawValue: v["band"] as! String)!
            let profile = Profile(rawValue: v["profile"] as? String ?? "normal")!
            let f = Reference.frame(fingerprint: Ids.bytes(fromHex: Ids.fingerprint(v["text"] as! String)), band: band, set: v["set"] as? Int ?? 0, profile: profile)
            XCTAssertEqual(hexString(f.header), v["header"] as? String, "\(band)")
            XCTAssertEqual(hexString(f.body), v["body"] as? String, "\(band)")
            XCTAssertEqual(f.symbols, v["symbols"] as! [[Int]], "\(band)")
            n += 1
        }
        XCTAssertGreaterThan(n, 10)
    }

    func testRiskBands() {
        XCTAssertEqual(Frames.riskBands(work: .top), [.mid, .top])
        XCTAssertEqual(Frames.riskBands(work: .mid), [.mid])
        XCTAssertEqual(Frames.riskBands(work: .audible), [.mid])
        XCTAssertEqual(Frames.riskBands(work: .top, avoid: .high), [.mid])
        XCTAssertEqual(Frames.riskBands(work: .top, avoid: .mid), [.top])
        XCTAssertEqual(Frames.riskBands(work: .xhigh, avoid: .wide), [.mid, .xhigh]) // nothing is clean: the default list
        XCTAssertFalse(Frames.overlap(.mid, .top))
        XCTAssertTrue(Frames.overlap(.upper, .mid))
    }

    func testSecurityLevelsMatchTheTable() throws {
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: Repo.url("test-vectors/security-levels.json"))) as! [String: Any]
        XCTAssertEqual(json["default"] as? String, SecurityLevel.default.rawValue)
        XCTAssertEqual(SecurityLevel.default, .maximum)
        let levels = json["levels"] as! [String: [String: Any]]
        for level in SecurityLevel.allCases {
            let t = levels[level.rawValue]!, s = Security.level(level), r = t["riskSignal"] as! [String: Any]
            XCTAssertEqual(s.detectMultipleEmitters, t["detectMultipleEmitters"] as? Bool, level.rawValue)
            XCTAssertEqual(s.listenWhileSending, t["listenWhileSending"] as? Bool, level.rawValue)
            XCTAssertEqual(s.listenBeforeSendingSeconds, (t["listenBeforeSendingSeconds"] as! NSNumber).doubleValue, level.rawValue)
            XCTAssertEqual(s.scanJitterSeconds, (t["scanJitterSeconds"] as! NSNumber).doubleValue, level.rawValue)
            XCTAssertEqual(s.confirmWatch, t["confirmWatch"] as? Bool, level.rawValue)
            XCTAssertEqual(s.confirmWatchSeconds, (t["confirmWatchSeconds"] as! NSNumber).doubleValue, level.rawValue)
            XCTAssertEqual(s.riskSignal.emit.rawValue, r["emit"] as? String, level.rawValue)
            XCTAssertEqual(s.riskSignal.listen, r["listen"] as? Bool, level.rawValue)
            XCTAssertEqual(s.riskSignal.honor, r["honor"] as? Bool, level.rawValue)
            XCTAssertEqual(s.riskSignal.listenUntil.rawValue, r["listenUntil"] as? String, level.rawValue)
            XCTAssertEqual(s.riskSignal.durationMs, r["durationMs"] as? Int, level.rawValue)
            XCTAssertEqual(s.riskSignal.detectorDurationMs, r["detectorDurationMs"] as? Int, level.rawValue)
            XCTAssertEqual(s.riskSignal.quarantineMs, r["quarantineMs"] as? Int, level.rawValue)
        }
        // An explicit value wins over the level, field by field (also inside riskSignal).
        var o = SecurityOverrides()
        o.listenBeforeSendingSeconds = 0.5
        o.riskSignal.honor = false
        let s = Security.resolve(.standard, o)
        XCTAssertEqual(s.listenBeforeSendingSeconds, 0.5)
        XCTAssertFalse(s.riskSignal.honor)
        XCTAssertTrue(s.riskSignal.listen)
        XCTAssertEqual(Security.resolve(), Security.level(.maximum))
    }

    // MARK: ControlListener

    private let rate = 48000.0
    private func listenControl(_ x: [Float], rate: Double = 48000) -> [ControlHeard] {
        var heard: [ControlHeard] = []
        let l = ControlListener(rate: rate, onHeard: { heard.append($0) })
        feed(x, chunk: 1024) { l.feed($0) }
        l.feed([Float](repeating: 0, count: Int(rate)))
        return heard
    }
    private func render(_ f: Frame, rate: Double = 48000) -> [Float] { Modem.render(f.symbols, f.channel, rate: rate, gapSeconds: Protocol.gapSeconds) }

    func testControlListenerReadsAckAndRiskOnEveryBand() {
        let risk = RiskFrame(reason: .overlap, source: .receiver, hops: 1, own: Ids.shortId("a"), intruder: Ids.shortId("b"))
        for band in [Band.top, .high, .upper, .mid, .xhigh, .wide] {
            let ack = AckFrame(receipt: false, id: Ids.ackId("hello"))
            let x = air([render(Frames.ack(ack, band: band)), render(Frames.risk(risk, band: band))], rate: rate, noise: 0.05, seed: 3, lead: 0.1)
            XCTAssertEqual(listenControl(x), [.ack(ack, band), .risk(risk, band)], "\(band)")
        }
    }

    func testControlListener44100() {
        let ack = AckFrame(receipt: true, id: Ids.ackId("hello"))
        let f = Frames.ack(ack)
        let x = air([render(f, rate: 44100)], rate: 44100, noise: 0.03, lead: 0.1)
        XCTAssertEqual(listenControl(x, rate: 44100), [.ack(ack, .high)])
    }

    func testControlListenerReadsAnAckWhileAPayloadPlays() throws {
        let payload = try Modem.encode(text: "a payload that keeps playing", band: .top, sampleRate: rate)
        let ack = AckFrame(receipt: false, id: Ids.ackId("x"))
        var x = air([payload, payload], rate: rate, noise: 0.02, lead: 0.1)
        let a = render(Frames.ack(ack))
        for i in 0..<a.count where 30000 + i < x.count { x[30000 + i] += a[i] }
        XCTAssertEqual(listenControl(x).filter { if case .ack = $0 { return true } else { return false } }.count, 1)
    }

    func testControlListenerIgnoresPayloadsAndNoise() throws {
        let p = try Modem.encode(text: "not a control frame", band: .mid, sampleRate: rate)
        XCTAssertTrue(listenControl(air([p], rate: rate, noise: 0.05, lead: 0.1)).isEmpty)
        XCTAssertTrue(listenControl(air([[Float](repeating: 0, count: Int(rate) * 3)], rate: rate, noise: 0.2, seed: 9)).isEmpty)
    }

    // MARK: Reference frames in the modem receiver

    func testReferenceFrameLoopback() {
        let text = "pay 12.50 to shop 7"
        let fp = Ids.fingerprint(text)
        for (band, profile) in [(Band.top, Profile.normal), (.mid, .normal), (.high, .robust), (.wide, .normal)] {
            let a = Reference.frame(fingerprint: Ids.bytes(fromHex: fp), band: band, set: 0, profile: profile)
            let b = Reference.frame(fingerprint: Ids.bytes(fromHex: fp), band: band, set: 1, profile: profile)
            let audio = { (f: Frame) in Modem.fitLevel(Modem.render(f.symbols, f.channel, rate: 48000, gapSeconds: Protocol.gapSeconds), level: 1) }
            let out = listen(air([audio(a), audio(b), audio(a)], rate: 48000, noise: 0.15, seed: 2), rate: 48000, profile: profile)
            XCTAssertEqual(out.heard.first?.kind, .reference, "\(band) \(profile)")
            XCTAssertEqual(out.heard.first?.fingerprint, fp, "\(band) \(profile)")
            XCTAssertEqual(out.heard.first?.band, band)
            XCTAssertEqual(out.clashes, 0)
        }
    }

    func testStoresValidateAndRoundTrip() {
        let m = MemoryCalibrationStore()
        m.save(Calibration(band: .mid, profile: .normal, uses: 1))
        XCTAssertEqual(m.load(), Calibration(band: .mid, profile: .normal, uses: 1))
        let suite = UserDefaults(suiteName: "bysound.tests.\(UUID().uuidString)")!
        let u = UserDefaultsCalibrationStore(defaults: suite, key: "k")
        u.save(Calibration(band: .upper, profile: .robust, uses: 2))
        XCTAssertEqual(UserDefaultsCalibrationStore(defaults: suite, key: "k").load(), Calibration(band: .upper, profile: .robust, uses: 2))
        suite.set(Data("{\"band\":\"nowhere\",\"uses\":1}".utf8), forKey: "k")
        XCTAssertNil(UserDefaultsCalibrationStore(defaults: suite, key: "k").load())
        suite.set(Data("{\"band\":\"mid\",\"profile\":\"normal\",\"uses\":-1}".utf8), forKey: "k")
        XCTAssertNil(UserDefaultsCalibrationStore(defaults: suite, key: "k").load())
    }

    func testLadderAmbientAndCalibrationRules() {
        let chain: [Band] = [.top, .high, .upper, .mid]
        XCTAssertEqual(Adapt.channelOrder(.top, chain), [.top, .high, .upper, .mid])
        XCTAssertEqual(Adapt.channelOrder(.upper, chain), [.upper, .mid, .high, .top])
        XCTAssertEqual(Adapt.channelOrder(.mid, chain), [.mid, .upper, .high, .top])

        let a = readAmbient([[.top: -50, .high: -52, .upper: -80, .mid: -78], [.top: -49, .high: -51, .upper: -81, .mid: -77], [.top: -51, .high: -53, .upper: -79, .mid: -79]], channels: chain)!
        XCTAssertEqual(a.noisy, [.top, .high])
        XCTAssertFalse(a.loud)
        let loud = Adapt.loudPlaceDb
        XCTAssertTrue(readAmbient([[.top: loud, .high: loud + 1, .upper: loud, .mid: loud]], channels: chain)!.loud)
        XCTAssertNil(readAmbient([], channels: [.top]))
        let p = plan(calibration: nil, ambient: a)
        XCTAssertEqual(p.route, [.upper, .mid])
        XCTAssertNil(p.startRobust)

        XCTAssertEqual(plan(calibration: Calibration(band: .mid, profile: .normal, uses: 3), ambient: nil),
                       Plan(route: [.mid, .upper, .high, .top], every: 6, calibration: Calibration(band: .mid, profile: .normal, uses: 3), startRobust: nil))
        XCTAssertEqual(plan(calibration: Calibration(band: .mid, profile: .normal, uses: Adapt.calibrationUses), ambient: nil).route[0], .top)
        XCTAssertEqual(plan(band: .xhigh, calibration: Calibration(band: .mid, profile: .normal, uses: 0), ambient: nil).route, [.xhigh])
        XCTAssertEqual(plan(calibration: Calibration(band: .upper, profile: .robust, uses: 0), ambient: nil).startRobust, .calibration)
        XCTAssertNil(plan(profile: .normal, calibration: Calibration(band: .upper, profile: .robust, uses: 0), ambient: nil).startRobust)
        let n = Adapt.robustNoiseDb
        let noisy = Ambient(noise: [.top: n + 1, .high: n, .upper: n, .mid: n], noisy: [], loud: false)
        XCTAssertEqual(plan(calibration: nil, ambient: noisy).startRobust, .noise)
        // The calibrated channel stays in the route even if it measures noisy.
        XCTAssertEqual(plan(calibration: Calibration(band: .top, profile: .normal, uses: 0), ambient: Ambient(noise: [:], noisy: [.top, .high], loud: false)).route, [.top, .upper, .mid])
    }

    func testRobustLadderAndLapped() {
        let l = RobustLadder(auto: true, robust: false)
        let seen = ([.top, .top, .top, .high, .high, .high, .xhigh, .upper] as [Band]).map { l.next($0) }
        XCTAssertEqual(seen, [.normal, .normal, .normal, .normal, .normal, .normal, .normal, .robust])
        XCTAssertEqual(l.reason, .repetitions)
        XCTAssertEqual(RobustLadder(auto: false, robust: false).next(.top), .normal)
        XCTAssertFalse(lapped([.top, .top, .high, .upper, .mid], routeLength: 4))
        XCTAssertTrue(lapped([.top, .high, .upper, .mid, .top], routeLength: 4))
    }
}
