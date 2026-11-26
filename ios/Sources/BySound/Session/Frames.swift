// Session frames: acknowledgement / receipt (0x41) and risk signal (0x58). Layout in docs/SESSION.md section 3;
// the bytes match test-vectors/v4-acks.json, session-acks.json and v4-risk.json.
import Foundation

public enum RiskReason: Int, Sendable, CaseIterable {
    /// A different transmission was heard.
    case other = 1
    /// Someone else is playing this device's own data.
    case clone = 2
    /// Two frames overlapped; nothing could be separated.
    case overlap = 3
    /// Reserved for an app-initiated alert; the library never emits it.
    case cancelled = 4
}

/// Who raised an alert.
public enum Role: Int, Sendable { case sender = 0, receiver = 1 }

/// Where a receiver read the data: band and profile of the frame it delivered.
public struct ReadAt: Equatable, Sendable {
    public var band: Band
    public var profile: Profile
    public init(band: Band, profile: Profile) { self.band = band; self.profile = profile }
}

public struct AckFrame: Equatable, Sendable {
    /// `false`: ack (the receiver got the data). `true`: receipt (the sender heard the ack).
    public var receipt: Bool
    /// First 4 bytes of SHA-256(text), 8 hex characters.
    public var id: String
    /// Where the receiver read it (a receipt echoes it). `nil`: not stated.
    public var heard: ReadAt?
    public init(receipt: Bool, id: String, heard: ReadAt? = nil) { self.receipt = receipt; self.id = id; self.heard = heard }
}

public struct RiskFrame: Equatable, Sendable {
    public var reason: RiskReason
    public var source: Role
    /// 0 for the original, +1 per relay.
    public var hops: Int
    /// Short id (6 hex) of the data the originator was sending or had received; `000000` if unknown.
    public var own: String
    /// Short id of the other transmission, `000000` if it could not be read.
    public var intruder: String
    public init(reason: RiskReason, source: Role, hops: Int, own: String, intruder: String) {
        self.reason = reason; self.source = source; self.hops = hops; self.own = own; self.intruder = intruder
    }
    /// The incident, without the hops: a relay is the same incident.
    public var key: String { "\(reason.rawValue)/\(source.rawValue)/\(own)/\(intruder)" }
}

public enum Frames {
    public static let controlParity = 8
    public static let ackData = 7
    public static let riskData = 10
    /// A risk frame heard with this many hops is never relayed again.
    public static let riskMaxHops = 2
    /// Acks travel on mid, receipts on high.
    public static let ackBand = Band.mid
    public static let receiptBand = Band.high
    /// Silence after each control frame when they are looped.
    public static let controlGapSeconds = 0.15

    private static func withCrc(_ data: [UInt8]) -> [UInt8] {
        let c = crc16(data)
        return data + [UInt8(c >> 8), UInt8(c & 255)]
    }
    /// High nibble of the ack's first byte: 0 (not stated) or `robust << 3 | channelId + 1`.
    private static func heardNibble(_ h: ReadAt?) -> UInt8 {
        guard let h = h else { return 0 }
        return UInt8((h.profile == .robust ? 8 : 0) | (h.band.channelId + 1))
    }

    public static func ackBytes(_ a: AckFrame) -> [UInt8] {
        withCrc([(heardNibble(a.heard) << 4) | (a.receipt ? 3 : 2)] + Ids.bytes(fromHex: a.id))
    }
    public static func riskBytes(_ r: RiskFrame) -> [UInt8] {
        withCrc([UInt8(r.reason.rawValue), UInt8(((r.hops & 15) << 4) | r.source.rawValue)] + Ids.bytes(fromHex: r.own) + Ids.bytes(fromHex: r.intruder))
    }

    private static func control(magic: UInt8, data: [UInt8], band: Band) -> Frame {
        let channel = band.channel(.normal)
        let header = Protocol.header(magic: magic, length: data.count, band: band)
        let body = rsEncode(data, parity: controlParity)
        let symbols = (0..<Protocol.preamble.count).map { channel.preambleSymbol($0) }
            + Codec.toSymbols(header, groups: channel.groups) + Codec.toSymbols(body, groups: channel.groups)
        return Frame(header: header, body: body, symbols: symbols, frameSeconds: Double(symbols.count) * channel.symbolSeconds, channel: channel)
    }

    /// Always the normal profile. Default band: mid for an ack, high for a receipt.
    public static func ack(_ a: AckFrame, band: Band? = nil) -> Frame {
        control(magic: Protocol.magicAck, data: ackBytes(a), band: band ?? (a.receipt ? receiptBand : ackBand))
    }
    public static func risk(_ r: RiskFrame, band: Band) -> Frame {
        control(magic: Protocol.magicRisk, data: riskBytes(r), band: band)
    }

    private static func crcOk(_ d: [UInt8]) -> Bool {
        d.count > 2 && crc16(Array(d.dropLast(2))) == (UInt16(d[d.count - 2]) << 8 | UInt16(d[d.count - 1]))
    }

    /// The 7 data bytes of an ack, or `nil` if the CRC fails or the heard nibble is invalid.
    public static func parseAck(_ data: [UInt8]) -> AckFrame? {
        guard data.count == ackData, crcOk(data) else { return nil }
        let kind = data[0] & 15, h = Int(data[0] >> 4)
        guard kind == 2 || kind == 3 else { return nil }
        let id = Ids.hex(Array(data[1..<5]))
        if h == 0 { return AckFrame(receipt: kind == 3, id: id) }
        guard let band = Band.allCases.first(where: { $0.channelId == (h & 7) - 1 }) else { return nil }
        return AckFrame(receipt: kind == 3, id: id, heard: ReadAt(band: band, profile: h & 8 != 0 ? .robust : .normal))
    }

    public static func parseRisk(_ data: [UInt8]) -> RiskFrame? {
        guard data.count == riskData, crcOk(data),
              let reason = RiskReason(rawValue: Int(data[0])), let source = Role(rawValue: Int(data[1] & 15)) else { return nil }
        return RiskFrame(reason: reason, source: source, hops: Int(data[1] >> 4), own: Ids.hex(Array(data[2..<5])), intruder: Ids.hex(Array(data[5..<8])))
    }

    /// Body bytes (data + parity) to data, correcting with Reed-Solomon. `erasures`: positions of doubtful bytes.
    static func decodeControlBody(_ body: [UInt8], erasures: [Int] = []) -> [UInt8]? {
        rsDecode(body, parity: controlParity, erasures: erasures)?.data
    }

    /// Two bands share at least one tone.
    public static func overlap(_ a: Band, _ b: Band) -> Bool {
        a.bin < b.bin + b.groups * Protocol.tones && b.bin < a.bin + a.groups * Protocol.tones
    }

    /// Bands a risk frame is looped on, in order (docs/SESSION.md section 3.2). Never audible.
    public static func riskBands(work: Band, avoid: Band? = nil) -> [Band] {
        let base: [Band] = work == .audible || work == .mid ? [.mid] : [.mid, work]
        guard let avoid = avoid, avoid != .audible else { return base }
        let clean = base.filter { !overlap($0, avoid) }
        if !clean.isEmpty { return clean }
        let spare = [Band.mid, .top].filter { !overlap($0, avoid) }
        return spare.isEmpty ? base : spare
    }
}
