// Simulated air for the session tests: several devices, each with a speaker and a microphone, on one sample clock. What each device
// hears is the sum of what every device plays (scaled by a gain per pair) plus white noise. Port of `testAir.ts`.
import Foundation
@testable import BySound

final class Device: SessionPlatform {
    final class Sound { let from: Device; let audio: [Float]; let start: Int; let onEnd: () -> Void; var stopped = false
        init(from: Device, audio: [Float], start: Int, onEnd: @escaping () -> Void) { self.from = from; self.audio = audio; self.start = start; self.onEnd = onEnd } }
    final class Timer { let due: Int; let fn: () -> Void; var cancelled = false
        init(due: Int, fn: @escaping () -> Void) { self.due = due; self.fn = fn } }

    unowned let air: Air
    let name: String
    var ears: Ears?
    /// What the speaker does to the sound (a broken tweeter, for example).
    var speaker: (([Float]) -> [Float])?
    private(set) var heard: [AirEvent] = []
    private var seed: Int
    init(_ air: Air, _ name: String, seed: Int) { self.air = air; self.name = name; self.seed = seed }

    var sampleRate: Double { air.rate }
    func now() -> Double { Double(air.t) / air.rate }
    func after(_ seconds: Double, _ fn: @escaping () -> Void) -> Cancel {
        let t = Timer(due: air.t + max(0, jsRound(seconds * air.rate)), fn: fn)
        air.timers.append(t)
        return { t.cancelled = true }
    }
    func play(_ audio: [Float], onEnd: @escaping () -> Void) -> Cancel {
        let s = Sound(from: self, audio: speaker?(audio) ?? audio, start: air.t, onEnd: onEnd)
        air.sounds.append(s)
        return { s.stopped = true }
    }
    func random() -> Double {
        seed = (seed &* 1103515245 &+ 12345) % 2147483648
        return Double(seed) / 2147483648
    }
    /// Sends what this device's microphone decodes to `hear`.
    func listen(robust: Bool = false, _ hear: @escaping (AirEvent) -> Void) {
        ears = Ears(sampleRate: air.rate, robust: robust, control: true) { [unowned self] e in
            if case .noise = e {} else { self.heard.append(e) }
            hear(e)
        }
    }
    /// Plays raw audio in a loop for `seconds`: a device that does not follow the session protocol (a rogue sender, a replay).
    @discardableResult
    func loop(_ audio: [Float], seconds: Double) -> Cancel {
        var stopped = false
        let until = now() + seconds
        func again() { if !stopped && now() < until { _ = play(audio, onEnd: again) } }
        again()
        return { stopped = true }
    }
}

final class Air {
    var t = 0
    let rate: Double
    let noiseLevel: Double
    let chunk: Int
    var devices: [Device] = []
    var sounds: [Device.Sound] = []
    var timers: [Device.Timer] = []
    private var gains: [String: Double] = [:]
    private var noise: Gaussian

    init(rate: Double = 48000, noise: Double = 0.003, seed: UInt32 = 1, chunk: Int = 1024) {
        self.rate = rate; noiseLevel = noise; self.chunk = chunk; self.noise = Gaussian(seed: seed)
    }

    func device(_ name: String) -> Device {
        let d = Device(self, name, seed: devices.count * 7919 + 17)
        devices.append(d)
        return d
    }

    /// How loud `from` is in the microphone of `to` (default 1; 0: out of range). Symmetric unless `oneWay`.
    func gain(_ from: Device, _ to: Device, _ g: Double, oneWay: Bool = false) {
        gains["\(from.name)>\(to.name)"] = g
        if !oneWay { gains["\(to.name)>\(from.name)"] = g }
    }

    /// Advances the clock by `seconds`, or until `until()` holds (checked every chunk).
    func run(_ seconds: Double, until: (() -> Bool)? = nil) {
        let end = t + jsRound(seconds * rate)
        while t < end && !(until?() ?? false) {
            let n = min(chunk, end - t)
            let from = t
            t += n
            let white = (0..<n).map { _ in Float(noise.next() * noiseLevel) }
            for d in devices {
                guard let ears = d.ears else { continue }
                var x = white
                for s in sounds where !s.stopped {
                    let g = Float(gains["\(s.from.name)>\(d.name)"] ?? 1)
                    if g == 0 { continue }
                    let k0 = max(0, s.start - from), k1 = min(n, s.start + s.audio.count - from)
                    if k0 >= k1 { continue }
                    for i in k0..<k1 { x[i] += g * s.audio[from + i - s.start] }
                }
                ears.feed(x)
            }
            let current = sounds
            sounds = []
            for s in current where !s.stopped {
                if t >= s.start + s.audio.count { s.onEnd() } else { sounds.append(s) }
            }
            let due = timers.sorted { $0.due < $1.due }
            timers = []
            for tm in due where !tm.cancelled {
                if tm.due <= t { tm.fn() } else { timers.append(tm) }
            }
        }
    }
}

/// A speaker that plays nothing above `cutoffHz`: a windowed-sinc low-pass of 511 taps.
func brokenTweeter(_ cutoffHz: Double, rate: Double = 48000) -> ([Float]) -> [Float] {
    let taps = 511, half = taps / 2
    let fc = cutoffHz / rate
    var h = (0..<taps).map { i -> Double in
        let m = Double(i - half)
        let sinc = m == 0 ? 2 * fc : sin(2 * Double.pi * fc * m) / (Double.pi * m)
        return sinc * (0.54 - 0.46 * cos(2 * Double.pi * Double(i) / Double(taps - 1)))
    }
    let sum = h.reduce(0, +)
    h = h.map { $0 / sum }
    let hf = h.map { Float($0) }
    return { audio in
        var out = [Float](repeating: 0, count: audio.count)
        audio.withUnsafeBufferPointer { a in
            out.withUnsafeMutableBufferPointer { o in
                for n in 0..<a.count {
                    var acc: Float = 0
                    let lo = max(0, n - taps + 1 + half), hi = min(a.count - 1, n + half)
                    if lo > hi { continue }
                    for i in lo...hi { acc += hf[n + half - i] * a[i] }
                    o[n] = acc
                }
            }
        }
        return out
    }
}

/// A steady interferer: equal-power tones on every grid bin from `fromHz` to `toHz` with random phases, for `seconds`.
func hiss(fromHz: Double, toHz: Double, amplitude: Double, seconds: Double, rate: Double = 48000, seed: UInt32 = 5) -> [Float] {
    var g = Gaussian(seed: seed)
    var out = [Double](repeating: 0, count: jsRound(seconds * rate))
    var hz = fromHz
    while hz <= toHz {
        let phase = g.next() * Double.pi, w = 2 * Double.pi * hz / rate
        for i in 0..<out.count { out[i] += amplitude * sin(w * Double(i) + phase) }
        hz += 46.875
    }
    return out.map { Float($0) }
}
