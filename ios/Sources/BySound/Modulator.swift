// Audio of a frame: one sine per tone group, a 3 ms raised-cosine ramp at each symbol edge and a gap at the end.
import Foundation

/// Builds the audio of v4 payload frames.
public enum Modem {
    /// The nibble grid of a payload frame: preamble, header and body. Throws `ModemError.exceedsMaxDuration` if one transmission would last more than 30 s.
    public static func symbols(text: String, band: Band, profile: Profile = .normal) throws -> [[Int]] {
        try Codec.payloadFrame(text, band: band, profile: profile).symbols
    }

    /// Largest packed payload (bytes after text packing, before the CRC-32) that fits in `Protocol.maxFrameSeconds`
    /// (preamble + header + body + gap) for the band and profile. ASCII letters pack 1:1; digit pairs pack 2:1.
    public static func maxPayloadBytes(band: Band, profile: Profile = .normal) -> Int {
        let ch = band.channel(profile)
        let cap = Protocol.maxFrameSeconds * 1_000_000
        var lo = 0, hi = Protocol.blockData * Protocol.maxBlocks - 4
        if Codec.transmissionMicros(ch, packed: hi) <= cap { return hi }
        while hi - lo > 1 {
            let mid = (lo + hi) / 2
            if Codec.transmissionMicros(ch, packed: mid) <= cap { lo = mid } else { hi = mid }
        }
        return lo
    }

    /// The payload frame (header, body, symbols) of `text`.
    public static func frame(text: String, band: Band, profile: Profile = .normal) throws -> Frame {
        try Codec.payloadFrame(text, band: band, profile: profile)
    }

    /// One frame followed by 350 ms of silence, as mono samples at `sampleRate`. Throws `ModemError.bandAboveNyquist` if the band does not fit the rate.
    /// `level` scales the output; the frame is only scaled down (never clipped) when its peak exceeds 1.
    public static func encode(text: String, band: Band, profile: Profile = .normal, sampleRate: Double, level: Float = 1) throws -> [Float] {
        let frame = try Codec.payloadFrame(text, band: band, profile: profile)
        let ch = frame.channel
        let top = ch.toneHz(group: ch.groups - 1, tone: Protocol.tones - 1)
        guard top < sampleRate / 2 else { throw ModemError.bandAboveNyquist(highestToneHz: top, sampleRate: sampleRate) }
        return fitLevel(render(frame.symbols, frame.channel, rate: sampleRate, gapSeconds: Protocol.gapSeconds), level: level)
    }

    static func render(_ symbols: [[Int]], _ ch: Channel, rate: Double, gapSeconds: Double) -> [Float] {
        var out = [Float](repeating: 0, count: ch.symbolStart(symbols.count, rate: rate) + sampleCount(gapSeconds, rate))
        let ramp = sampleCount(Protocol.rampSeconds, rate)
        var longest = 0
        for k in 0..<symbols.count { longest = max(longest, ch.symbolStart(k + 1, rate: rate) - ch.symbolStart(k, rate: rate)) }
        // Each tone is computed once for a whole symbol length: a frame repeats the same 16 tones per group.
        var sines: [Int: [Double]] = [:]
        func sine(_ g: Int, _ t: Int) -> [Double] {
            let key = g * Protocol.tones + t
            if let s = sines[key] { return s }
            let omega = 2 * Double.pi * ch.toneHz(group: g, tone: t) / rate
            let s = (0..<longest).map { sin(omega * Double($0)) }
            sines[key] = s
            return s
        }
        for (k, tones) in symbols.enumerated() {
            let start = ch.symbolStart(k, rate: rate)
            let len = ch.symbolStart(k + 1, rate: rate) - start
            let waves = tones.enumerated().map { sine($0.offset, $0.element) }
            for n in 0..<len {
                var env = 1.0
                if n < ramp { env = 0.5 * (1 - cos(Double.pi * Double(n) / Double(ramp))) }
                else if n >= len - ramp { env = 0.5 * (1 - cos(Double.pi * Double(len - 1 - n) / Double(ramp))) }
                var s = 0.0
                for w in waves { s += w[n] }
                out[start + n] = Float(s * env * ch.amplitude)
            }
        }
        return out
    }

    /// Scales the audio by `level`, and down further only if its peak exceeds 1.
    static func fitLevel(_ audio: [Float], level: Float) -> [Float] {
        let peak = audio.reduce(Float(0)) { max($0, abs($1)) }
        let gain = level * (peak > 1 ? 1 / peak : 1)
        return gain == 1 ? audio : audio.map { $0 * gain }
    }
}
