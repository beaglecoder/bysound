import XCTest
import BySound

/// Audio produced by the C implementation and by real recordings must decode here too.
final class CrossImplementationTests: XCTestCase {
    private let example = "00020101021226470017com.example.pagos01220000003100099988877766520454115303032540812500.505802AR5918ALMACEN LA ESQUINA6007CORDOBA62250108F-0001230509PEDIDO-776304F4AF"

    func testGoldenPayloadWide44100() throws {
        let x = int16Samples(try Data(contentsOf: Repo.url("c/test/golden/payload_wide_44100.i16")))
        let out = listen(x, rate: 44100)
        XCTAssertEqual(out.heard.count, 1)
        XCTAssertEqual(out.heard.first?.text, example)
        XCTAssertEqual(out.heard.first?.band, .wide)
    }

    func testGoldenMatchesSwiftModulator() throws {
        let gold = int16Samples(try Data(contentsOf: Repo.url("c/test/golden/payload_wide_44100.i16")))
        let mine = try Modem.encode(text: example, band: .wide, sampleRate: 44100)
        XCTAssertEqual(mine.count, gold.count)
        var worst: Float = 0
        for i in 0..<min(mine.count, gold.count) { worst = max(worst, abs(mine[i] - gold[i])) }
        XCTAssertLessThan(worst, 0.001, "int16 quantisation only")
    }

    func testRecordingFalso() throws {
        let x = int16Samples(try Data(contentsOf: Repo.url("c/test/recordings/falso-v4.wav")), skip: 44)
        let out = listen(x, rate: 48000)
        XCTAssertEqual(out.heard.count, 1)
        XCTAssertTrue(out.heard.first?.text.hasPrefix("000201010212264700") ?? false)
        XCTAssertEqual(out.heard.first?.band, .top)
        XCTAssertEqual(out.clashes, 0)
    }
}
