import XCTest
@testable import BySound

final class ProtocolTests: XCTestCase {
    func testVectors() throws {
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: Repo.url("test-vectors/v4-frames.json"))) as! [String: Any]
        let vectors = json["vectors"] as! [[String: Any]]
        var checked = 0
        for v in vectors where v["mode"] as? String == "payload" {
            let band = Band(rawValue: v["band"] as! String)!
            let profile = Profile(rawValue: v["profile"] as! String)!
            let frame = try Modem.frame(text: v["text"] as! String, band: band, profile: profile)
            let name = "\(band) \(profile)"
            XCTAssertEqual(frame.header, hex(v["header"] as! String), name)
            XCTAssertEqual(frame.body, hex(v["body"] as! String), name)
            XCTAssertEqual(frame.symbols, v["symbols"] as! [[Int]], name)
            XCTAssertEqual(frame.frameSeconds, (v["frameSeconds"] as! NSNumber).doubleValue, accuracy: 1e-9, name)
            checked += 1
        }
        XCTAssertEqual(checked, 22)
    }

    func testChecksums() {
        let s = Array("123456789".utf8)
        XCTAssertEqual(crc32(s), 0xcbf4_3926)
        XCTAssertEqual(crc16(s), 0x29b1)
    }

    func testReedSolomonRoundTrip() {
        let data = (0..<50).map { UInt8(($0 * 7 + 3) & 255) }
        let code = rsEncode(data, parity: 12)
        XCTAssertEqual(code.count, 62)
        XCTAssertEqual(rsDecode(code, parity: 12)?.data, data)
        // 6 errors
        var bad = code
        for i in [0, 9, 20, 33, 47, 61] { bad[i] ^= 0x5a }
        let r = rsDecode(bad, parity: 12)
        XCTAssertEqual(r?.data, data)
        XCTAssertEqual(r?.corrected, 6)
        // 12 erasures
        var erased = code
        let pos = Array(stride(from: 2, to: 62, by: 5)).prefix(12)
        for p in pos { erased[p] = 0 }
        XCTAssertEqual(rsDecode(erased, parity: 12, erasures: Array(pos))?.data, data)
        // too many errors
        var worse = code
        for i in 0..<10 { worse[i * 6] ^= 0xff }
        XCTAssertNil(rsDecode(worse, parity: 12))
    }

    func testPacking() {
        for t in ["", "hello", "12", "123", "ñandú 🙂 2024", "a\nb", String(repeating: "ñ€", count: 400)] {
            XCTAssertEqual(TextPacking.unpack(TextPacking.pack(t)), t)
        }
        XCTAssertEqual(TextPacking.pack("0123"), [0x81, 0x97] as [UInt8])
    }
}
