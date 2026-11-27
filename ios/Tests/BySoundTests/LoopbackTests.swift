import XCTest
import BySound

private let short = #"{"m":"Almacén","a":1250.5}"#
private let long = "00020101021226470017com.example.pagos01220000003100099988877766520454115303032540812500.505802AR5918ALMACEN LA ESQUINA6007CORDOBA62250108F-0001230509PEDIDO-776304F4AF ñandú 🙂"

final class LoopbackTests: XCTestCase {
    private func check(_ text: String, rate: Double, noise: Double, seed: UInt32, profile: Profile = .normal, bands: [Band] = Band.allCases) throws {
        for band in bands {
            let lap = try Modem.encode(text: text, band: band, profile: profile, sampleRate: rate)
            let out = listen(air([lap, lap], rate: rate, noise: noise, seed: seed), rate: rate, profile: profile)
            XCTAssertEqual(out.heard.first?.text, text, "\(band) \(profile) \(rate)")
            XCTAssertEqual(out.heard.first?.band, band, "\(band) \(profile) \(rate)")
            XCTAssertEqual(out.clashes, 0, "\(band) \(rate)")
            XCTAssertEqual(out.failures, 0, "\(band) \(rate)")
        }
    }

    func testShort48000() throws { try check(short, rate: 48000, noise: 0.3, seed: 1) }
    func testShort44100() throws { try check(short, rate: 44100, noise: 0.3, seed: 1) }
    func testLong48000() throws { try check(long, rate: 48000, noise: 0.15, seed: 3) }
    func testLong44100() throws { try check(long, rate: 44100, noise: 0.15, seed: 3) }

    func testRobust48000() throws { try check(short, rate: 48000, noise: 0.3, seed: 1, profile: .robust, bands: Protocol.robustBands) }
    func testRobust44100() throws { try check(short, rate: 44100, noise: 0.3, seed: 1, profile: .robust, bands: [.top, .mid]) }

    func testWeakMicWithEcho() throws {
        let rate = 48000.0
        for band in [Band.top, .wide] {
            let lap = try Modem.encode(text: short, band: band, sampleRate: rate)
            let out = listen(air([lap, lap, lap], rate: rate, noise: 0.15, gain: 0.2, echo: 0.3, seed: 7), rate: rate)
            XCTAssertEqual(out.heard.first?.text, short, "\(band)")
        }
    }

    func testListenerHearsBothProfiles() throws {
        let rate = 48000.0
        let a = try Modem.encode(text: short, band: .upper, profile: .normal, sampleRate: rate)
        let b = try Modem.encode(text: "other", band: .top, profile: .robust, sampleRate: rate)
        var heard: [Heard] = []
        let l = Listener(sampleRate: rate)
        l.onFrame = { heard.append($0) }
        feed(air([a, a, b, b], rate: rate, noise: 0.1)) { l.feed($0) }
        XCTAssertTrue(heard.contains { $0.text == short && $0.profile == .normal })
        XCTAssertTrue(heard.contains { $0.text == "other" && $0.profile == .robust })
    }

    func testSilenceHearsNothing() {
        let out = listen(air([[Float](repeating: 0, count: 48000 * 4)], rate: 48000, noise: 0.3), rate: 48000)
        XCTAssertTrue(out.heard.isEmpty)
        XCTAssertEqual(out.failures, 0)
    }
}
