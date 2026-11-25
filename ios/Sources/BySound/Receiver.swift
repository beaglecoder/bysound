// Streaming receiver for v4 payload frames: all seven bands at once. Faithful port of `receiver.ts`
// (payload frames only: reference, ack and risk frames are recognised and skipped).
import Foundation

/// A decoded payload frame.
public struct Heard {
    public let text: String
    public let band: Band
    public let profile: Profile
    /// Bytes that Reed-Solomon had to repair.
    public let correctedBytes: Int
    /// Laps (repetitions of the frame) that were needed to read it; 1 means the first one was enough.
    public let combined: Int
    /// Preamble match of the frame, 0...1.
    public let score: Double
}

/// Callbacks shared between a receiver and its channel readers.
final class ReceiverEvents {
    var onFrame: ((Heard) -> Void)?
    var onHeader: ((Band) -> Void)?
    var onBlocks: ((Int, Int) -> Void)?
    var onClash: (() -> Void)?
}

/// Receives frames from a stream of mono samples. Not thread safe: feed it from one queue.
/// `profile: .robust` only hears robust frames (top, high, upper and mid), with twice the window and search step;
/// use `Listener` to hear both profiles.
public final class Receiver {
    public let sampleRate: Double
    public let profile: Profile
    let events = ReceiverEvents()

    /// Called for every decoded frame.
    public var onFrame: ((Heard) -> Void)? {
        get { events.onFrame }
        set { events.onFrame = newValue }
    }
    /// A valid header was read on this band.
    public var onHeader: ((Band) -> Void)? {
        get { events.onHeader }
        set { events.onHeader = newValue }
    }
    /// Blocks closed so far and total, for the text being read.
    public var onBlocks: ((Int, Int) -> Void)? {
        get { events.onBlocks }
        set { events.onBlocks = newValue }
    }
    /// Two transmissions overlapped.
    public var onClash: (() -> Void)? {
        get { events.onClash }
        set { events.onClash = newValue }
    }

    let winSize: Int
    let hopSize: Int
    let windowSeconds: Double
    let hopSeconds: Double
    /// Samples that can be re-read backwards (12 s).
    let ringSize: Int
    var windowed: [Double]
    /// FFT of the window when tones fall exactly on its bins; `nil` means Goertzel.
    let fft: FFT?
    let binStride: Int
    private let taper: [Double]
    private(set) var total = 0
    private var ring: [Float]
    private var hops = 0
    private var sinceHop = 0
    private let highBank: ToneBank
    private let lowBank: ToneBank?
    private var readers: [ChannelReader] = []
    private let pool: Pool

    static let ringSeconds = 12.0
    static let ringSlack = 4096

    public init(sampleRate: Double, profile: Profile = .normal, onFrame: ((Heard) -> Void)? = nil) {
        self.sampleRate = sampleRate
        self.profile = profile
        let scale = profile == .robust ? 2.0 : 1.0
        windowSeconds = Protocol.windowSeconds * scale
        hopSeconds = Protocol.hopSeconds * scale
        winSize = sampleCount(windowSeconds, sampleRate)
        hopSize = sampleCount(hopSeconds, sampleRate)
        ringSize = Int((Receiver.ringSeconds * sampleRate).rounded())
        ring = [Float](repeating: 0, count: ringSize + Receiver.ringSlack)
        windowed = [Double](repeating: 0, count: winSize)
        taper = symbolWindow(winSize)
        let stride = Double(winSize) * Protocol.spacingHz / sampleRate
        let aligned = winSize >= 2 && winSize & (winSize - 1) == 0 && abs(stride - stride.rounded()) < 1e-9 && stride.rounded() >= 1
        binStride = aligned ? Int(stride.rounded()) : 0
        fft = aligned && (Band.wide.bin + 6 * Protocol.tones) * binStride < winSize / 2 ? FFT(size: winSize) : nil
        highBank = ToneBank(baseBin: Band.mid.bin, tones: 6 * Protocol.tones, rate: sampleRate)
        // Audible has no robust profile: the robust receiver skips its bank.
        lowBank = profile == .robust ? nil : ToneBank(baseBin: Band.audible.bin, tones: 4 * Protocol.tones, rate: sampleRate)
        pool = Pool(rate: sampleRate, events: events)
        events.onFrame = onFrame
        let bands = profile == .robust ? Protocol.robustBands : Band.allCases
        readers = bands.map { ChannelReader(self, $0.channel(profile), $0 == .audible ? lowBank! : highBank, pool, events) }
    }

    /// Number of frames that were seen but could not be read.
    public var failures: Int { pool.failures }
    /// Progress of the frame being read, 0...1, or -1 when idle.
    public var progress: Double {
        (readers.first { $0.state == .body } ?? readers.first { $0.state == .header })?.progress ?? -1
    }

    /// Adds samples (any chunk size).
    public func feed(_ x: [Float]) {
        // The block is stored at once (wrapping pointer) and then processed hop by hop. The ring has room for the 12 s that
        // are re-read plus one block.
        if ringSize + x.count > ring.count { grow(ringSize + x.count) }
        let size = ring.count
        var p = total % size
        for v in x {
            ring[p] = v
            p += 1
            if p == size { p = 0 }
        }
        var left = x.count
        while left > 0 {
            let n = min(left, max(hopSize - sinceHop, winSize - total, 1))
            total += n
            sinceHop += n
            left -= n
            if sinceHop >= hopSize && total >= winSize {
                sinceHop = 0
                onHop()
            }
        }
    }

    private func grow(_ size: Int) {
        let old = ring
        var new = [Float](repeating: 0, count: size)
        var at = max(0, total - old.count)
        while at < total {
            new[at % size] = old[at % old.count]
            at += 1
        }
        ring = new
    }

    /// Copies `[start, start + winSize)` into `windowed`, edges smoothed (see `symbolWindow`).
    func window(_ start: Int) {
        let size = ring.count
        var p = ((start % size) + size) % size
        for n in 0..<winSize {
            windowed[n] = Double(ring[p]) * taper[n]
            p += 1
            if p == size { p = 0 }
        }
    }

    private func onHop() {
        let index = hops
        hops += 1
        window(index * hopSize)
        // With an FFT a single transform feeds both banks.
        fft?.analyze(windowed)
        highBank.compute(self, hop: index, analyzed: fft != nil)
        lowBank?.compute(self, hop: index, analyzed: fft != nil)
        for r in readers { r.onHop(index) }
        pool.flushHeaderFails(total: total)
    }

    /// Energies of the tones of a grid in `x`: FFT bins when available, Goertzel otherwise.
    func toneEnergies(_ x: [Double], firstBin: Int, coeffs: [Double], out: inout [Double], analyzed: Bool = false) {
        guard let fft = fft else { return goertzel(x, coeffs: coeffs, out: &out) }
        if !analyzed { fft.analyze(x) }
        for t in 0..<out.count { out[t] = fft.energy(bin: (firstBin + t) * binStride) }
    }
}

/// Tone energies of one grid, per search hop.
final class ToneBank {
    static let history = 160
    let baseBin: Int
    private let coeffs: [Double]
    private var energies: [[Double]]
    init(baseBin: Int, tones: Int, rate: Double) {
        self.baseBin = baseBin
        coeffs = (0..<tones).map { 2 * cos(2 * Double.pi * Double(baseBin + $0) * Protocol.spacingHz / rate) }
        energies = Array(repeating: [Double](repeating: 0, count: tones), count: ToneBank.history)
    }
    func compute(_ r: Receiver, hop: Int, analyzed: Bool) {
        let slot = hop % ToneBank.history
        r.toneEnergies(r.windowed, firstBin: baseBin, coeffs: coeffs, out: &energies[slot], analyzed: analyzed)
    }
    func at(_ hop: Int) -> [Double] { energies[hop % ToneBank.history] }
}
