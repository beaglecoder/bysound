import XCTest
@testable import BySound

// Receiver bugs found by the benchmark in bench/ (see bench/README.md), in a clean channel.

/// First text of the series whose last block ends in a parity byte 0x00 (about 1 in 256).
private func zeroParityText(_ prefix: String) throws -> String {
    var i = 0
    while true {
        let text = "\(prefix) \(i)"
        if try Codec.payloadFrame(text, band: .top, profile: .normal).body.last == 0 { return text }
        i += 1
    }
}

private func heard(_ text: String, profile: Profile, rate: Double, laps: Int = 1) throws -> [String] {
    let lap = try Modem.encode(text: text, band: .top, profile: profile, sampleRate: rate)
    return listen(air(Array(repeating: lap, count: laps), rate: rate, noise: 0), rate: rate, profile: profile).heard.map(\.text)
}

final class RegressionTests: XCTestCase {
    /// RS also accepts the last block one byte short when its last parity byte is 0x00 (c(x) = x·c'(x)): the CRC-32 picks the length.
    func testZeroLastParityByte() throws {
        let short = try zeroParityText("zero parity")
        let long = try zeroParityText("a payload long enough to need two blocks of forty-eight bytes, zero parity")
        XCTAssertLessThan(try Codec.payloadFrame(short, band: .top, profile: .normal).body.count, 64)
        XCTAssertGreaterThan(try Codec.payloadFrame(long, band: .top, profile: .normal).body.count, 64)
        for text in [short, long] {
            for profile in [Profile.normal, .robust] {
                XCTAssertEqual(try heard(text, profile: profile, rate: 48000), [text], "\(text) \(profile)")
            }
        }
    }

    /// On top/robust, body symbols 50..57 of this payload match 6 of the preamble's 16 nibbles (0.375 without noise): that used to be
    /// taken as the next lap and cut off the read before the last block, so the payload failed only at high SNR.
    func testPreambleLookAlikeInBody() throws {
        let text = "q(qs@TgOXJfdn<NXK<f,h?<StQ*yJq)b"
        for rate in [48000.0, 44100.0] {
            XCTAssertEqual(try heard(text, profile: .robust, rate: rate), [text], "\(rate)")
        }
        XCTAssertEqual(try heard(text, profile: .robust, rate: 48000, laps: 2), [text, text])
    }
}
