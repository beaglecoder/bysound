// A small receiver for the two session frames (ack 0x41, risk 0x58) on the six high bands, and the noise reports of the ambient check.
// Port of `controlListener.ts`. The modem receiver recognises these headers but skips their bodies (docs/SESSION.md section 11), so this
// reads them: per-tone noise floor and whitening for the preamble search, a plateau to place the start, the most likely of the two
// valid headers, Reed-Solomon with erasures and the CRC-16. No echo removal and no lap combining: control frames are looped.
import Foundation

/// What a `ControlListener` decodes.
public enum ControlHeard: Equatable {
    case ack(AckFrame, Band)
    case risk(RiskFrame, Band)
}

/// Mean energy per tone of each band over the last 0.25 s, in dB relative to a full-scale tone of the grid.
public typealias BandNoise = [Band: Double]

private let noiseReportSeconds = 0.25
private let controlBands: [Band] = [.top, .high, .upper, .mid, .xhigh, .wide]
private let lowBin = Band.mid.bin
private let toneCount = 6 * Protocol.tones
private let history = 128
private let ringSeconds = 4.0
private let headerMinAgreement = 6
private let plateauShare = 0.97
private let plateauMaxHops = 40
private let erasureTries = [0, 2, 4]
private let floorSeconds = 0.5
private let floorWarmSeconds = 0.3
private let floorWarmTau = 0.08
private let floorOutlier = 4.0
private let whitenMin = 0.05

private struct Read {
    var start: Int
    var magic: UInt8?
    var dataLength = 0
}

private final class BandState {
    let id: Band
    let channel: Channel
    /// Expected tone (0..95) of each preamble symbol and group.
    let preamble: [[Int]]
    var candidates: [(start: Int, score: Double)] = []
    var reads: [Read] = []
    var quietUntil = 0
    let headers: [(magic: UInt8, length: Int, nibbles: [Int])]
    init(_ id: Band) {
        self.id = id
        channel = id.channel(.normal)
        let ch = channel
        preamble = (0..<Protocol.preamble.count).map { k in
            ch.preambleSymbol(k).enumerated().map { g, v in id.bin - lowBin + g * Protocol.tones + v }
        }
        headers = [(Protocol.magicAck, Frames.ackData), (Protocol.magicRisk, Frames.riskData)].map { magic, length in
            (magic, length, Protocol.header(magic: magic, length: length, band: id).flatMap { [Int($0 >> 4), Int($0 & 15)] })
        }
    }
}

/// Not thread safe: feed it from one queue.
final class ControlListener {
    let rate: Double
    private let onHeard: (ControlHeard) -> Void
    private let onNoise: ((BandNoise) -> Void)?
    private let win: Int
    private let hop: Int
    private let taper: [Double]
    private let fft: FFT?
    private let stride: Int
    private let coeffs: [Double]
    private var ring: [Float]
    private var windowed: [Double]
    private var energy = [Double](repeating: 0, count: toneCount)
    private var hist = [[Double]](repeating: [Double](repeating: 0, count: toneCount), count: history)
    private var floor = [Double](repeating: 0, count: toneCount)
    private var total = 0
    private var hops = 0
    private let bands: [BandState]
    private var noiseSum = [Double](repeating: 0, count: toneCount)
    private var noiseHops = 0
    private let noiseEvery: Int
    /// Energy of a full-scale tone on the grid in this window: (sum window / 2)^2.
    private let fullScale: Double

    init(rate: Double, onHeard: @escaping (ControlHeard) -> Void, onNoise: ((BandNoise) -> Void)? = nil) {
        self.rate = rate
        self.onHeard = onHeard
        self.onNoise = onNoise
        win = sampleCount(Protocol.windowSeconds, rate)
        hop = sampleCount(Protocol.hopSeconds, rate)
        taper = symbolWindow(win)
        fullScale = pow(taper.reduce(0, +) / 2, 2)
        noiseEvery = jsRound(noiseReportSeconds / Protocol.hopSeconds)
        windowed = [Double](repeating: 0, count: win)
        ring = [Float](repeating: 0, count: jsRound(ringSeconds * rate))
        let s = Double(win) * Protocol.spacingHz / rate
        let aligned = win >= 4 && win & (win - 1) == 0 && abs(s - s.rounded()) < 1e-9 && s.rounded() >= 1
        stride = aligned ? Int(s.rounded()) : 0
        fft = aligned && (lowBin + toneCount) * stride < win / 2 ? FFT(size: win) : nil
        coeffs = (0..<toneCount).map { 2 * cos(2 * Double.pi * Double(lowBin + $0) * Protocol.spacingHz / rate) }
        bands = controlBands.map { BandState($0) }
    }

    func feed(_ x: [Float]) {
        let step = 4096 // the ring keeps 4 s: a larger block is processed in pieces
        var at = 0
        while at < x.count {
            let end = min(at + step, x.count)
            let size = ring.count
            var p = total % size
            for i in at..<end {
                ring[p] = x[i]
                p += 1
                if p == size { p = 0 }
            }
            total += end - at
            while hops * hop + win <= total {
                onHop(hops)
                hops += 1
            }
            for b in bands { readFrames(b) }
            at = end
        }
    }

    /// Energies of the 96 tones in the window that starts at sample `start`.
    private func measure(_ start: Int, _ out: inout [Double]) {
        let size = ring.count
        var p = ((start % size) + size) % size
        for n in 0..<win {
            windowed[n] = Double(ring[p]) * taper[n]
            p += 1
            if p == size { p = 0 }
        }
        if let fft = fft {
            fft.analyze(windowed)
            for t in 0..<toneCount { out[t] = fft.energy(bin: (lowBin + t) * stride) }
        } else {
            goertzel(windowed, coeffs: coeffs, out: &out)
        }
    }

    private func onHop(_ h: Int) {
        measure(h * hop, &energy)
        let busy = bands.contains { !$0.reads.isEmpty || $0.quietUntil > total }
        if h == 0 {
            let median = energy.sorted()[toneCount >> 1]
            floor = [Double](repeating: median, count: toneCount)
        } else if !busy {
            let tau = Double(h) * Protocol.hopSeconds < floorWarmSeconds ? floorWarmTau : floorSeconds
            let a = min(1, Protocol.hopSeconds / tau)
            for t in 0..<toneCount { floor[t] += a * (min(energy[t], floorOutlier * floor[t]) - floor[t]) }
        }
        let mean = floor.reduce(0, +)
        let least = max(whitenMin * mean / Double(toneCount), 1e-12)
        var w = [Double](repeating: 0, count: toneCount)
        for t in 0..<toneCount { w[t] = energy[t] / max(floor[t], least) }
        hist[h % history] = w
        for b in bands { search(b, h) }
        if onNoise != nil { noteNoise() }
    }

    private func noteNoise() {
        for t in 0..<toneCount { noiseSum[t] += energy[t] }
        noiseHops += 1
        if noiseHops < noiseEvery { return }
        var out: BandNoise = [:]
        for id in controlBands {
            let from = id.bin - lowBin, n = id.groups * Protocol.tones
            var sum = 0.0
            for t in from..<(from + n) { sum += noiseSum[t] }
            out[id] = 10 * log10(sum / Double(n) / Double(noiseHops) / fullScale + 1e-30)
        }
        noiseSum = [Double](repeating: 0, count: toneCount)
        noiseHops = 0
        onNoise?(out)
    }

    private func symbolStart(_ ch: Channel, _ k: Int) -> Int { ch.symbolStart(k, rate: rate) }

    /// Tests the hypothesis "a preamble of this band ended in hop h", and closes a plateau of candidates.
    private func search(_ b: BandState, _ h: Int) {
        let ch = b.channel
        let ws = jsRound(ch.windowStartSeconds * rate)
        let start = h * hop - jsRound((Double(Protocol.preamble.count - 1) * ch.symbolSeconds + ch.windowStartSeconds) * rate)
        let first = jsRound(Double(start + ws) / Double(hop))
        var score = -1.0
        if start >= 0 && first > h - history && start >= b.quietUntil {
            var sum = 0.0
            for k in 0..<Protocol.preamble.count {
                let hk = jsRound(Double(start + symbolStart(ch, k) + ws) / Double(hop))
                let w = hist[hk % history]
                for (g, tone) in b.preamble[k].enumerated() {
                    var all = 0.0
                    let base = b.id.bin - lowBin + g * Protocol.tones
                    for v in 0..<Protocol.tones { all += w[base + v] }
                    sum += all > 0 ? w[tone] / all : 0
                }
            }
            score = sum / Double(Protocol.preamble.count * ch.groups)
        }
        if score >= Protocol.preambleThreshold && b.candidates.count < plateauMaxHops {
            b.candidates.append((start, score))
            return
        }
        if b.candidates.isEmpty { return }
        let best = b.candidates.map { $0.score }.max()!
        let plateau = b.candidates.filter { $0.score >= plateauShare * best }
        let at = jsRound(Double(plateau.reduce(0) { $0 + $1.start }) / Double(plateau.count))
        b.candidates = []
        b.reads.append(Read(start: at))
        // The longest control frame on this band: no new search inside it.
        b.quietUntil = at + symbolStart(ch, Protocol.preamble.count + symbolsFor(4, ch.groups) + symbolsFor(Frames.riskData + Frames.controlParity, ch.groups))
    }

    private func symbolsFor(_ bytes: Int, _ groups: Int) -> Int { (bytes * 2 + groups - 1) / groups }

    /// Probabilities of the nibbles carried by `count` symbols from symbol `first` of the frame at `start`.
    private func soft(_ b: BandState, _ start: Int, _ first: Int, _ count: Int) -> Soft {
        let ch = b.channel
        var out: Soft = []
        var e = [Double](repeating: 0, count: toneCount)
        for k in first..<(first + count) {
            measure(start + symbolStart(ch, k) + jsRound(ch.windowStartSeconds * rate), &e)
            for g in 0..<ch.groups {
                let base = b.id.bin - lowBin + g * Protocol.tones
                var all = 0.0
                for v in 0..<Protocol.tones { all += e[base + v] }
                out.append((0..<Protocol.tones).map { all > 0 ? e[base + $0] / all : 1.0 / Double(Protocol.tones) })
            }
        }
        return out
    }

    private func readFrames(_ b: BandState) {
        let ch = b.channel
        let headerSymbols = symbolsFor(4, ch.groups)
        func endOf(_ start: Int, _ symbols: Int) -> Int { start + symbolStart(ch, symbols) + jsRound(ch.windowStartSeconds * rate) + win }
        var kept: [Read] = []
        for var r in b.reads {
            if r.magic == nil {
                if total < endOf(r.start, Protocol.preamble.count + headerSymbols) { kept.append(r); continue }
                let s = Array(soft(b, r.start, Protocol.preamble.count, headerSymbols).prefix(8))
                var best = b.headers[0], bestScore = -1.0
                for c in b.headers {
                    var sc = 0.0
                    for (i, n) in c.nibbles.enumerated() { sc += s[i][n] }
                    if sc > bestScore { bestScore = sc; best = c }
                }
                let agree = best.nibbles.enumerated().filter { hard(s[$0.offset]) == $0.element }.count
                if agree < headerMinAgreement { b.quietUntil = 0; continue }
                r.magic = best.magic
                r.dataLength = best.length
            }
            let bodyBytes = r.dataLength + Frames.controlParity
            let bodySymbols = symbolsFor(bodyBytes, ch.groups)
            if total < endOf(r.start, Protocol.preamble.count + headerSymbols + bodySymbols) { kept.append(r); continue }
            let s = soft(b, r.start, Protocol.preamble.count + headerSymbols, bodySymbols)
            var bytes: [UInt8] = [], confidence: [Double] = []
            for i in 0..<bodyBytes {
                let hi = s[2 * i], lo = s[2 * i + 1]
                bytes.append(UInt8((hard(hi) << 4) | hard(lo)))
                confidence.append(min(margin(hi), margin(lo)))
            }
            let doubtful = confidence.enumerated().sorted { $0.element != $1.element ? $0.element < $1.element : $0.offset < $1.offset }.map { $0.offset }
            for n in erasureTries {
                guard let data = Frames.decodeControlBody(bytes, erasures: doubtful.prefix(n).sorted()) else { continue }
                if r.magic == Protocol.magicAck {
                    if let ack = Frames.parseAck(data) { onHeard(.ack(ack, b.id)); break }
                } else if let risk = Frames.parseRisk(data) {
                    onHeard(.risk(risk, b.id)); break
                }
            }
        }
        b.reads = kept
    }
}
