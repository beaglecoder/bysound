import Foundation
import BySound

enum Repo {
    /// Repository root, found relative to this file.
    static let root = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    static func url(_ path: String) -> URL { root.appendingPathComponent(path) }
}

/// Seeded Gaussian noise (mulberry32 + Box-Muller), the same generator as the TypeScript tests.
struct Gaussian {
    private var s: UInt32
    init(seed: UInt32) { s = seed }
    private mutating func uniform() -> Double {
        s = s &+ 0x6d2b_79f5
        var t = (s ^ (s >> 15)) &* (1 | s)
        t = (t &+ ((t ^ (t >> 7)) &* (61 | t))) ^ t
        return Double(t ^ (t >> 14)) / 4294967296
    }
    mutating func next() -> Double {
        (-2 * log(1 - uniform())).squareRoot() * cos(2 * Double.pi * uniform())
    }
}

/// Joins laps with leading silence and adds gain, echo and white noise.
func air(_ laps: [[Float]], rate: Double, noise: Double, gain: Double = 1, echo: Double = 0, seed: UInt32 = 1, lead: Double = 0.5) -> [Float] {
    let leadN = Int((lead * rate).rounded())
    var out = [Float](repeating: 0, count: leadN + laps.reduce(0) { $0 + $1.count } + Int((0.3 * rate).rounded()))
    var at = leadN
    for l in laps { out.replaceSubrange(at..<(at + l.count), with: l); at += l.count }
    let delay = Int((0.005 * rate).rounded())
    if echo != 0 { var i = out.count - 1; while i >= delay { out[i] += Float(echo) * out[i - delay]; i -= 1 } }
    var g = Gaussian(seed: seed)
    for i in 0..<out.count { out[i] = Float(Double(out[i]) * gain + g.next() * noise) }
    return out
}

/// Feeds in chunks of 1024, like an audio callback.
func feed(_ x: [Float], chunk: Int = 1024, _ f: ([Float]) -> Void) {
    var i = 0
    while i < x.count { f(Array(x[i..<min(i + chunk, x.count)])); i += chunk }
}

func listen(_ x: [Float], rate: Double, profile: Profile = .normal) -> (heard: [Heard], clashes: Int, failures: Int) {
    var heard: [Heard] = []
    var clashes = 0
    let r = Receiver(sampleRate: rate, profile: profile) { heard.append($0) }
    r.onClash = { clashes += 1 }
    feed(x) { r.feed($0) }
    return (heard, clashes, r.failures)
}

func hex(_ s: String) -> [UInt8] {
    var out: [UInt8] = []
    var i = s.startIndex
    while i < s.endIndex {
        let j = s.index(i, offsetBy: 2)
        out.append(UInt8(s[i..<j], radix: 16)!)
        i = j
    }
    return out
}

func int16Samples(_ data: Data, skip: Int = 0) -> [Float] {
    let n = (data.count - skip) / 2
    return (0..<n).map { i in
        let lo = UInt16(data[skip + 2 * i]), hi = UInt16(data[skip + 2 * i + 1])
        return Float(Int16(bitPattern: lo | hi << 8)) / 32768
    }
}
