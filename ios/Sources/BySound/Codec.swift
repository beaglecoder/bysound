// Text packing and the payload frame: blocks of 48 data bytes, each Reed-Solomon coded, closed by a CRC-32.
import Foundation

/// Text packing: digit pairs take one byte (0x80...0xE3), printable ASCII is sent as is,
/// anything else goes into UTF-8 runs `0xFF, length, bytes...`.
public enum TextPacking {
    private static func isDigit(_ c: UInt32) -> Bool { c >= 0x30 && c <= 0x39 }
    private static func isASCII(_ c: UInt32) -> Bool { c >= 0x20 && c <= 0x7e }

    /// UTF-8 bytes in chunks of up to 255 that never split a character.
    private static func utf8Chunks(_ scalars: ArraySlice<Unicode.Scalar>) -> [[UInt8]] {
        var chunks: [[UInt8]] = []
        var cur: [UInt8] = []
        for s in scalars {
            let b = Array(String(s).utf8)
            if cur.count + b.count > 255 {
                chunks.append(cur)
                cur = []
            }
            cur += b
        }
        if !cur.isEmpty { chunks.append(cur) }
        return chunks
    }

    public static func pack(_ text: String) -> [UInt8] {
        let s = Array(text.unicodeScalars)
        var out: [UInt8] = []
        var i = 0
        while i < s.count {
            let c = s[i].value
            if isDigit(c), i + 1 < s.count, isDigit(s[i + 1].value) {
                out.append(UInt8(0x80 + Int(c - 0x30) * 10 + Int(s[i + 1].value - 0x30)))
                i += 2
            } else if isASCII(c) {
                out.append(UInt8(c))
                i += 1
            } else {
                var j = i
                while j < s.count && !isASCII(s[j].value) { j += 1 }
                for chunk in utf8Chunks(s[i..<j]) { out += [0xff, UInt8(chunk.count)] + chunk }
                i = j
            }
        }
        return out
    }

    /// The text of packed bytes, or `nil` if they are not valid.
    public static func unpack(_ data: [UInt8]) -> String? {
        var s = ""
        var i = 0
        while i < data.count {
            let b = data[i]
            if b >= 0x20 && b <= 0x7e {
                s.unicodeScalars.append(Unicode.Scalar(b))
                i += 1
            } else if b >= 0x80 && b <= 0xe3 {
                let v = Int(b) - 0x80
                s += "\(v / 10)\(v % 10)"
                i += 1
            } else if b == 0xff {
                // Consecutive 0xFF runs are a single text: older senders cut every 255 bytes, even inside a character.
                var bytes: [UInt8] = []
                while i < data.count && data[i] == 0xff {
                    if i + 1 >= data.count { return nil }
                    let n = Int(data[i + 1])
                    if n == 0 || i + 2 + n > data.count { return nil }
                    bytes += data[(i + 2)..<(i + 2 + n)]
                    i += 2 + n
                }
                guard let part = String(bytes: bytes, encoding: .utf8) else { return nil }
                s += part
            } else {
                return nil
            }
        }
        return s
    }
}

/// A payload frame ready to be modulated.
public struct Frame {
    /// The 4 header bytes.
    public let header: [UInt8]
    /// The body bytes: `total` blocks of `[index, total, data..., parity...]`.
    public let body: [UInt8]
    /// One nibble per tone group, preamble first.
    public let symbols: [[Int]]
    /// Duration of the frame without the trailing gap.
    public let frameSeconds: Double
    let channel: Channel
}

/// A decoded block.
struct Block {
    let index: Int
    let total: Int
    let chunk: [UInt8]
}

enum Codec {
    static func toSymbols(_ bytes: [UInt8], groups: Int) -> [[Int]] {
        var n = bytes.flatMap { [Int($0 >> 4), Int($0 & 15)] }
        while n.count % groups != 0 { n.append(0) }
        return stride(from: 0, to: n.count, by: groups).map { Array(n[$0..<($0 + groups)]) }
    }

    /// Exact duration in microseconds of the transmission (frame + gap) of a payload with `packed` packed bytes (before the CRC-32).
    static func transmissionMicros(_ ch: Channel, packed: Int) -> Int {
        let len = packed + 4
        let total = (len + Protocol.blockData - 1) / Protocol.blockData
        let body = len + total * (Protocol.blockHeader + Protocol.blockParity)
        let g = ch.groups
        let symbols = Protocol.preamble.count + (2 * Protocol.headerBytes + g - 1) / g + (2 * body + g - 1) / g
        return symbols * jsRound(ch.symbolSeconds * 1e6) + jsRound(Protocol.gapSeconds * 1e6)
    }

    /// Dato completo: pack + CRC-32 in blocks of 48 bytes, each `RS([index, total] + data, 12)`.
    static func payloadFrame(_ text: String, band: Band, profile: Profile) throws -> Frame {
        let content = TextPacking.pack(text)
        let c = crc32(content)
        let data = content + [UInt8(c >> 24), UInt8((c >> 16) & 255), UInt8((c >> 8) & 255), UInt8(c & 255)]
        let total = (data.count + Protocol.blockData - 1) / Protocol.blockData
        guard total <= Protocol.maxBlocks else { throw ModemError.textTooLong }
        let channel = band.channel(profile)
        guard Codec.transmissionMicros(channel, packed: content.count) <= Protocol.maxFrameSeconds * 1_000_000 else {
            throw ModemError.exceedsMaxDuration(packedBytes: content.count, maxPackedBytes: Modem.maxPayloadBytes(band: band, profile: profile))
        }
        var body: [UInt8] = []
        for b in 0..<total {
            let chunk = data[(b * Protocol.blockData)..<min((b + 1) * Protocol.blockData, data.count)]
            body += rsEncode([UInt8(b), UInt8(total)] + chunk, parity: Protocol.blockParity)
        }
        let header = Protocol.header(magic: Protocol.magicPayload, length: total, band: band, profile: channel.profile)
        let symbols = (0..<Protocol.preamble.count).map { channel.preambleSymbol($0) }
            + toSymbols(header, groups: channel.groups) + toSymbols(body, groups: channel.groups)
        return Frame(header: header, body: body, symbols: symbols, frameSeconds: Double(symbols.count) * channel.symbolSeconds, channel: channel)
    }

    static func decodeBlock(_ bytes: [UInt8], erasures: [Int] = []) -> (block: Block, corrected: Int)? {
        if bytes.count <= Protocol.blockHeader + Protocol.blockParity || bytes.count > Protocol.blockBytes { return nil }
        guard let r = rsDecode(bytes, parity: Protocol.blockParity, erasures: erasures) else { return nil }
        let index = Int(r.data[0]), total = Int(r.data[1])
        if total < 1 || index >= total { return nil }
        return (Block(index: index, total: total, chunk: Array(r.data[Protocol.blockHeader...])), r.corrected)
    }

    /// The text if the CRC-32 closes; `nil` otherwise. `chunks` holds every block in order.
    static func assemble(_ chunks: [[UInt8]]) -> String? {
        if chunks.dropLast().contains(where: { $0.count != Protocol.blockData }) { return nil }
        let data = chunks.flatMap { $0 }
        if data.count <= 4 { return nil }
        let content = Array(data.dropLast(4))
        let t = data.suffix(4).map(UInt32.init)
        let crc = (t[0] << 24) | (t[1] << 16) | (t[2] << 8) | t[3]
        return crc32(content) == crc ? TextPacking.unpack(content) : nil
    }
}

/// Errors from building a frame.
public enum ModemError: Error {
    /// More than 255 blocks (about 12 KB).
    case textTooLong
    /// One transmission would last more than `Protocol.maxFrameSeconds` (30 s). `maxPackedBytes` is the limit for the band and profile.
    case exceedsMaxDuration(packedBytes: Int, maxPackedBytes: Int)
}
