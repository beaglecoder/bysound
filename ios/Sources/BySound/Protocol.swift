// bysound v4 protocol constants, bands, profiles and the frame header.
// The numbers here mirror `protocol-v4.json` at the repository root.
import Foundation

/// A transmission band. All bands use 16 tones per group spaced 46.875 Hz apart.
public enum Band: String, CaseIterable, Sendable {
    case top, high, upper, mid, audible, xhigh, wide

    /// Channel id carried in the frame header.
    public var channelId: Int {
        switch self {
        case .top: return 0
        case .high: return 1
        case .upper: return 2
        case .mid: return 3
        case .audible: return 4
        case .xhigh: return 5
        case .wide: return 6
        }
    }

    /// First tone of the band, as a multiple of the 46.875 Hz spacing.
    var bin: Int {
        switch self {
        case .top: return 384
        case .high: return 368
        case .upper, .xhigh: return 352
        case .mid, .wide: return 320
        case .audible: return 40
        }
    }

    /// Tone groups: each group sends one 4-bit nibble per symbol.
    var groups: Int {
        switch self {
        case .top: return 2
        case .high, .upper, .mid: return 3
        case .audible, .xhigh: return 4
        case .wide: return 6
        }
    }

    /// Amplitude of each tone.
    var amplitude: Double {
        switch self {
        case .audible: return 0.2
        case .xhigh: return 0.25
        case .wide: return 0.2
        default: return 0.3
        }
    }

    var symbolSeconds: Double { self == .audible ? 0.04 : 0.032 }
    var windowStartSeconds: Double { self == .audible ? 0.005 : 0.004 }

    /// Whether the band has a robust profile (the audible, xhigh and wide bands do not).
    public var hasRobust: Bool { Protocol.robustBands.contains(self) }

    /// The band timings of a profile.
    func channel(_ profile: Profile) -> Channel {
        let robust = profile == .robust && hasRobust
        return Channel(band: self, profile: robust ? .robust : .normal, bin: bin, groups: groups, amplitude: amplitude,
                       symbolSeconds: robust ? symbolSeconds * 2 : symbolSeconds,
                       windowStartSeconds: robust ? windowStartSeconds * 2 : windowStartSeconds)
    }
}

/// Frame profile. `robust` doubles symbol, window and search step (top, high, upper and mid only):
/// twice the airtime for roughly 3 dB.
public enum Profile: String, CaseIterable, Sendable {
    case normal, robust
}

/// A band together with the timings of one profile.
struct Channel {
    let band: Band
    let profile: Profile
    let bin: Int
    let groups: Int
    let amplitude: Double
    let symbolSeconds: Double
    let windowStartSeconds: Double

    /// Frequency of a tone.
    func toneHz(group: Int, tone: Int) -> Double { Double(bin + group * Protocol.tones + tone) * Protocol.spacingHz }
    /// First sample of symbol `k`.
    func symbolStart(_ k: Int, rate: Double) -> Int { jsRound(Double(k) * symbolSeconds * rate) }

    /// Preamble nibbles of symbol `k` (each channel and profile has its own pattern).
    func preambleSymbol(_ k: Int) -> [Int] {
        let key = (profile == .robust ? Protocol.preambleKeysRobust[band] : nil) ?? Protocol.preambleKeys[band]!
        return (0..<groups).map { (Protocol.preamble[k][($0 + key.shift) % 4] + key.add) & 15 }
    }
}

/// Rounds half up, like `Math.round` in the TypeScript reference.
@inline(__always) func jsRound(_ x: Double) -> Int { Int((x + 0.5).rounded(.down)) }
/// Samples in `seconds` at `rate`.
@inline(__always) func sampleCount(_ seconds: Double, _ rate: Double) -> Int { jsRound(seconds * rate) }

/// Protocol constants.
public enum Protocol {
    public static let version = 4
    public static let spacingHz = 46.875
    public static let tones = 16
    public static let gapSeconds = 0.35
    /// Hard cap: one transmission (preamble + header + body + gap) never lasts longer than this.
    public static let maxFrameSeconds = 30
    public static let rampSeconds = 0.003
    public static let hopSeconds = 0.004
    public static let windowSeconds = 1 / spacingHz
    public static let preambleThreshold = 0.32

    public static let blockData = 48
    public static let blockParity = 12
    public static let blockHeader = 2
    public static let blockBytes = blockHeader + blockData + blockParity
    public static let maxBlocks = 255
    public static let headerBytes = 4

    public static let magicPayload: UInt8 = 0x56
    public static let magicReference: UInt8 = 0x52
    public static let magicRisk: UInt8 = 0x58
    public static let magicAck: UInt8 = 0x41
    // Frame types this package does not decode: they are only recognised so they can be skipped.
    static let referenceData = 14
    static let referenceBody = 30
    static let riskLength = 10
    static let riskBody = 18
    static let ackLength = 7
    static let ackBody = 15

    static let preamble: [[Int]] = [
        [0, 8, 4, 12], [15, 7, 11, 3], [2, 10, 6, 14], [13, 5, 9, 1],
        [4, 12, 0, 8], [11, 3, 15, 7], [6, 14, 2, 10], [9, 1, 13, 5],
    ]
    /// Preamble key per band: (column shift, nibble offset).
    static let preambleKeys: [Band: (shift: Int, add: Int)] = [
        .top: (0, 3), .high: (3, 9), .upper: (2, 14), .mid: (1, 13), .audible: (0, 0), .xhigh: (1, 5), .wide: (3, 14),
    ]
    static let preambleKeysRobust: [Band: (shift: Int, add: Int)] = [
        .top: (1, 13), .high: (3, 11), .upper: (0, 8), .mid: (2, 9),
    ]
    /// Bands that have a robust profile.
    public static let robustBands: [Band] = [.top, .high, .upper, .mid]

    /// Frame header: `[type, length, CRC-8, version | channel << 1 | set]`.
    /// The CRC also covers the profile (0 normal, 1 robust), which is not transmitted.
    static func header(magic: UInt8, length: Int, band: Band, set: Int = 0, profile: Profile = .normal) -> [UInt8] {
        let version = UInt8((Protocol.version << 4) | (band.channelId << 1) | (set & 1))
        let len = UInt8(length & 255)
        return [magic, len, crc8([magic, len, version, profile == .robust ? 1 : 0]), version]
    }
}
