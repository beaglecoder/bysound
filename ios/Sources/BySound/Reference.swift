// Reference frames (0x52): only the 12-byte fingerprint travels, protected by a RS(46, 14) mother code split into two parity sets
// (A and B) that alternate between repetitions. Port of `referenceFrame` / `decodeMother` in `codec.ts`.
import Foundation

enum Reference {
    static let bytes = 12
    static let data = 14
    static let parity = 16
    static let motherParity = 32
    static let motherBytes = data + motherParity

    static func parityStart(_ set: Int) -> Int { data + (set & 1) * parity }
    static func parityRange(_ set: Int) -> [Int] { (0..<parity).map { parityStart(set) + $0 } }

    /// Header + body of a reference frame: the fingerprint + CRC-16 + the 16 parity bytes of `set`.
    static func frame(fingerprint: [UInt8], band: Band, set: Int = 0, profile: Profile = .normal) -> Frame {
        precondition(fingerprint.count == bytes, "The fingerprint must be 12 bytes")
        let c = crc16(fingerprint)
        let content = fingerprint + [UInt8(c >> 8), UInt8(c & 255)]
        let mother = rsEncode(content, parity: motherParity)
        let start = parityStart(set)
        let channel = band.channel(profile)
        let body = content + Array(mother[start..<(start + parity)])
        let header = Protocol.header(magic: Protocol.magicReference, length: data, band: band, set: set & 1, profile: channel.profile)
        let symbols = (0..<Protocol.preamble.count).map { channel.preambleSymbol($0) }
            + Codec.toSymbols(header, groups: channel.groups) + Codec.toSymbols(body, groups: channel.groups)
        return Frame(header: header, body: body, symbols: symbols, frameSeconds: Double(symbols.count) * channel.symbolSeconds, channel: channel)
    }

    /// The mother code with the parity that is available: what is missing and `erasures` count as erasures. Checks the CRC-16.
    static func decodeMother(_ data: [UInt8], parity sets: [[UInt8]?], erasures: [Int] = []) -> (fingerprint: [UInt8], corrected: Int)? {
        if data.count != Reference.data { return nil }
        var word = data + [UInt8](repeating: 0, count: motherParity)
        var erased = Set(erasures)
        for set in 0..<2 {
            if let p = sets[set], p.count == parity {
                for (i, v) in p.enumerated() { word[parityStart(set) + i] = v }
            } else {
                for i in parityRange(set) { erased.insert(i) }
            }
        }
        if erased.count > motherParity { return nil }
        guard let r = rsDecode(word, parity: motherParity, erasures: erased.sorted()) else { return nil }
        let content = Array(r.data[0..<bytes])
        if crc16(content) != (UInt16(r.data[bytes]) << 8 | UInt16(r.data[bytes + 1])) { return nil }
        return (content, r.corrected)
    }
}
