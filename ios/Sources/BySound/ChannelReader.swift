// One band's state machine: preamble search, header, block decoding and soft combining across laps.
// Port of `ChannelReader` in `receiver.ts`.
import Foundation

private let floorSeconds = 0.5
private let floorWarmSeconds = 0.3
private let floorWarmTau = 0.08
private let floorOutlier = 4.0
private let headerShare = 0.12
private let headerMinAgreement = 5
private let headerSureAgreement = 7
private let combineMax = 6
private let combineSeconds = 30.0
private let overlapFromSeconds = 0.3
private let overlapMinHops = 2
private let overlapMinScore = 0.45
private let selfEchoSeconds = 0.3
private let overlapMarginSeconds = 0.15
private let headerFailNearSeconds = 0.15
private let headerFailWaitSeconds = 0.3
private let blockErasureTries = [0, 2, 4]

/// Blocks of the text being read, gathered across laps.
final class BlockAssembly {
    var total = 0
    var chunks: [Int: [UInt8]] = [:]
    var pending: [Int: [Soft]] = [:]
    var pendingLast: [Soft] = []
    var loops = 0
    var lastAt = 0
    var channel: Band?
    /// Body bytes of the lap that closed the last block (the header does not say).
    var knownBytes = 0
    func begin(total: Int, at: Int, horizon: Int, channel: Band) {
        if total != self.total || lastAt < horizon { reset() }
        self.total = total
        lastAt = at
        self.channel = channel
    }
    func reset() {
        total = 0
        chunks = [:]
        pending = [:]
        pendingLast = []
        loops = 0
        knownBytes = 0
    }
}

/// State shared by the readers of one receiver.
final class Pool {
    let rate: Double
    let events: ReceiverEvents
    var starts: [(band: Band, at: Int, span: Int)] = []
    let blocks = BlockAssembly()
    var headerFails: [(at: Int, noted: Int)] = []
    var failures = 0
    init(rate: Double, events: ReceiverEvents) {
        self.rate = rate
        self.events = events
    }
    func clash() { events.onClash?() }
    func noteStart(band: Band, at: Int, span: Int) {
        let selfEcho = selfEchoSeconds * rate
        let margin_ = overlapMarginSeconds * rate
        let overlapped = starts.contains { o in
            let apart = Double(abs(at - o.at))
            return (apart >= selfEcho || o.band != band) && apart < Double(o.at < at ? o.span : span) - margin_
        }
        starts.append((band, at, span))
        if starts.count > 32 { starts.removeFirst() }
        if overlapped { clash() }
    }
    /// Waits a little: the real channel may read its header in the same step or right after.
    func flushHeaderFails(total: Int) {
        if headerFails.isEmpty { return }
        let due = Double(total) - headerFailWaitSeconds * rate
        let near = headerFailNearSeconds * rate
        let kept = headerFails.filter { Double($0.noted) > due }
        for f in headerFails where Double(f.noted) <= due {
            if !starts.contains(where: { Double(f.at) >= Double($0.at) - near && f.at <= $0.at + $0.span }) { failures += 1 }
        }
        headerFails = kept
    }
}

enum ReaderState { case search, header, body }

private struct HeaderCandidate {
    let magic: UInt8
    let length: Int
    let nibbles: [Int]
}

final class ChannelReader {
    var state = ReaderState.search
    var progress = -1.0
    private unowned let r: Receiver
    private let ch: Channel
    private let bank: ToneBank
    private let pool: Pool
    private let events: ReceiverEvents
    private let tones: Int
    private let coeffs: [Double]
    private var floor: [Double]
    private var scratch: [Double]
    private var windowOut: [Double]
    private var symbolW: [Double]
    private var groupD = [Double](repeating: 0, count: Protocol.tones)
    /// Echo tail (see `softSymbol`); `nil` at the start of a reading.
    private var previous: [Double]?
    private var echo = 0.0
    private var candStarts: [Int] = []
    private var candScores: [Double] = []
    private var frameScore = 0.0
    private var frameStart = 0
    private var searchFrom = 0
    private var magic = Protocol.magicPayload
    /// The header matched in full. With corrected errors the announced length may be false (see `tryBlocks`).
    private var headerSure = true
    private var dataLength = 0
    private var bodySoft: Soft = []
    private var bodyRead = 0
    private var lastTried = 0
    private let preamble: [[Int]]
    private let candidates: [HeaderCandidate]
    private let headerSymbols: Int
    private let endOffset: Double
    private let preambleSpan: Double

    init(_ r: Receiver, _ ch: Channel, _ bank: ToneBank, _ pool: Pool, _ events: ReceiverEvents) {
        self.r = r
        self.ch = ch
        self.bank = bank
        self.pool = pool
        self.events = events
        tones = ch.groups * Protocol.tones
        coeffs = (0..<tones).map { 2 * cos(2 * Double.pi * ch.toneHz(group: $0 / Protocol.tones, tone: $0 % Protocol.tones) / r.sampleRate) }
        floor = [Double](repeating: 0, count: tones)
        scratch = floor
        windowOut = floor
        symbolW = floor
        preamble = (0..<Protocol.preamble.count).map { ch.preambleSymbol($0) }
        headerSymbols = (Protocol.headerBytes * 2 + ch.groups - 1) / ch.groups
        endOffset = ch.windowStartSeconds + r.windowSeconds
        preambleSpan = Double(Protocol.preamble.count - 1) * ch.symbolSeconds + endOffset
        func head(_ magic: UInt8, _ n: Int, _ set: Int = 0) -> HeaderCandidate {
            HeaderCandidate(magic: magic, length: n, nibbles: Protocol.header(magic: magic, length: n, band: ch.band, set: set, profile: ch.profile).flatMap { [Int($0 >> 4), Int($0 & 15)] })
        }
        var c = [head(Protocol.magicReference, Protocol.referenceData, 0), head(Protocol.magicReference, Protocol.referenceData, 1),
                 head(Protocol.magicRisk, Protocol.riskLength), head(Protocol.magicAck, Protocol.ackLength)]
        for t in 1...Protocol.maxBlocks { c.append(head(Protocol.magicPayload, t)) }
        candidates = c
    }

    private var bodyBytes: Int { (dataLength - 1) * Protocol.blockBytes + Protocol.blockHeader + 1 + Protocol.blockParity }
    private var bodyBytesMax: Int { dataLength * Protocol.blockBytes }
    private func symbolsFor(_ bytes: Int) -> Int { (bytes * 2 + ch.groups - 1) / ch.groups }
    private func start(_ k: Int) -> Int { ch.symbolStart(k, rate: r.sampleRate) }
    private func symbolEnd(_ k: Int) -> Int { frameStart + start(k) + sampleCount(endOffset, r.sampleRate) }
    private var firstBody: Int { Protocol.preamble.count + headerSymbols }
    private func hopEnd(_ i: Int) -> Int { r.winSize + i * r.hopSize }
    private var offset: Int { ch.bin - bank.baseBin }

    func onHop(_ index: Int) {
        if state == .search { updateFloor(index) }
        switch state {
        case .search: search(index)
        case .header: tryHeader()
        case .body:
            if !searchInBody(index) { tryBlocks() }
        }
        progress = currentProgress()
    }

    private func updateFloor(_ hop: Int) {
        let e = bank.at(hop)
        if hop == 0 {
            let sorted = (0..<tones).map { e[offset + $0] }.sorted()
            floor = [Double](repeating: sorted[tones >> 1], count: tones)
            return
        }
        let warm = Double(hop + 1) * r.hopSeconds < floorWarmSeconds
        let a = r.hopSeconds / (warm ? floorWarmTau : floorSeconds)
        for t in 0..<tones {
            let x = e[offset + t], v = floor[t]
            floor[t] = v + a * ((v > 0 ? min(x, floorOutlier * v) : x) - v)
        }
    }

    private func whitenMinimum() -> Double {
        var s = 0.0
        for t in 0..<tones { s += floor[t] }
        return max(s / Double(tones) * 0.05, 1e-20)
    }

    private func preambleScore(_ index: Int) -> (start: Int, score: Double)? {
        let rate = r.sampleRate
        let start = hopEnd(index) - jsRound(preambleSpan * rate)
        if start < 0 { return nil }
        let minimum = whitenMinimum()
        var score = 0.0
        for k in 0..<Protocol.preamble.count {
            let end = start + jsRound((Double(k) * ch.symbolSeconds + endOffset) * rate)
            let hop = jsRound(Double(end - r.winSize) / Double(r.hopSize))
            if hop < 0 || hop > index || index - hop >= ToneBank.history { return nil }
            let e = bank.at(hop), off = offset
            for t in 0..<tones { scratch[t] = e[off + t] / max(floor[t], minimum) }
            for g in 0..<ch.groups {
                let base = g * Protocol.tones
                var sum = 0.0
                for v in 0..<Protocol.tones { sum += scratch[base + v] }
                if sum > 1e-12 { score += scratch[base + preamble[k][g]] / sum }
            }
        }
        return (start, score / Double(Protocol.preamble.count * ch.groups))
    }

    /// Centre of the plateau of candidates.
    private func plateau() -> Int {
        let best = candScores.max()!
        frameScore = best
        let centered = candStarts.enumerated().filter { candScores[$0.offset] >= best * 0.97 }.map { $0.element }
        let at = jsRound(Double(centered.reduce(0, +)) / Double(centered.count))
        candStarts = []
        candScores = []
        return at
    }

    private func search(_ index: Int) {
        guard let found = preambleScore(index), found.start >= searchFrom else { return }
        if found.score >= Protocol.preambleThreshold && candStarts.count < 64 {
            candStarts.append(found.start)
            candScores.append(found.score)
            return
        }
        if !candStarts.isEmpty { enterFrame(plateau()) }
    }

    private func enterFrame(_ at: Int) {
        frameStart = at
        state = .header
        measureEcho()
        tryHeader()
    }

    /// Inside a body the preamble is searched too: on top of it is another sender, after the minimum it is the next lap.
    private func searchInBody(_ index: Int) -> Bool {
        guard let found = preambleScore(index) else { return false }
        let rate = r.sampleRate
        let minEnd = frameStart + start(firstBody + symbolsFor(bodyBytes))
        if found.score >= Protocol.preambleThreshold && Double(found.start) >= Double(frameStart) + overlapFromSeconds * rate && candStarts.count < 64 {
            candStarts.append(found.start)
            candScores.append(found.score)
            return false
        }
        if candStarts.isEmpty { return false }
        let hops = candStarts.count
        let score = candScores.max()!
        let at = plateau()
        if at + start(Protocol.preamble.count) < minEnd {
            // A data run that looks like a preamble gives a lone peak; two senders give a high plateau.
            if hops >= overlapMinHops && score >= overlapMinScore { pool.clash() }
            return false
        }
        tryBlocks()
        if state == .body { finishBlocks(nil, bodySoft.count >> 1) }
        enterFrame(at)
        return true
    }

    /// Energies of symbol `k` of the current frame, normalised by the noise floor.
    private func symbolEnergies(_ k: Int) -> [Double] {
        r.window(frameStart + start(k) + sampleCount(ch.windowStartSeconds, r.sampleRate))
        r.toneEnergies(r.windowed, firstBin: ch.bin, coeffs: coeffs, out: &windowOut)
        let minimum = whitenMinimum()
        var out = [Double](repeating: 0, count: tones)
        for t in 0..<tones { out[t] = windowOut[t] / max(floor[t], minimum) }
        return out
    }

    private func measureEcho() {
        var ratios: [Double] = []
        var last = symbolEnergies(0)
        for k in 1..<preamble.count {
            let cur = symbolEnergies(k)
            for g in 0..<ch.groups {
                let t = g * Protocol.tones + preamble[k - 1][g]
                if last[t] > 1e-12 { ratios.append(cur[t] / last[t]) }
            }
            last = cur
        }
        ratios.sort()
        echo = ratios.isEmpty ? 0 : min(max(ratios[ratios.count >> 1], 0), 0.8)
        previous = last
    }

    private func softSymbol(_ k: Int) -> Soft {
        let w = symbolEnergies(k)
        var prev = previous ?? [Double](repeating: 0, count: tones)
        var out: Soft = []
        for g in 0..<ch.groups {
            let base = g * Protocol.tones
            var sum = 0.0
            for v in 0..<Protocol.tones {
                groupD[v] = max(w[base + v] - echo * prev[base + v], 0)
                sum += groupD[v]
            }
            for v in 0..<Protocol.tones { prev[base + v] = w[base + v] + echo * prev[base + v] }
            if sum <= 1e-12 { out.append(uniformNibble) } else { out.append(groupD.map { $0 / sum }) }
        }
        previous = prev
        return out
    }

    private func resetSearch() {
        state = .search
        searchFrom = frameStart + start(1)
        candStarts = []
        candScores = []
    }

    private func bodyReset() {
        bodySoft = []
        bodyRead = 0
        lastTried = Protocol.blockHeader + Protocol.blockParity
    }

    private func tryHeader() {
        let first = Protocol.preamble.count
        if r.total < symbolEnd(first + headerSymbols - 1) { return }
        var soft: Soft = []
        for i in 0..<headerSymbols { soft += softSymbol(first + i) }
        let n = Protocol.headerBytes * 2
        var best = -1.0
        var choice = candidates[0]
        for c in candidates {
            var s = 0.0
            for i in 0..<n { s += soft[i][c.nibbles[i]] }
            if s > best { best = s; choice = c }
        }
        var agree = 0
        for i in 0..<n where hard(soft[i]) == choice.nibbles[i] { agree += 1 }
        if foreignHeader(soft) { return resetSearch() }
        if agree < headerMinAgreement { return unreadableHeader() }
        magic = choice.magic
        dataLength = choice.length
        headerSure = agree == n
        bodyReset()
        if magic != Protocol.magicPayload {
            // Not a text: skip over the body (a reference frame still counts for clash detection).
            let bodyLen = magic == Protocol.magicReference ? Protocol.referenceBody : magic == Protocol.magicAck ? Protocol.ackBody : Protocol.riskBody
            if magic == Protocol.magicReference && agree >= headerSureAgreement {
                events.onHeader?(ch.band)
                pool.noteStart(band: ch.band, at: frameStart, span: start(firstBody + symbolsFor(bodyLen)))
            }
            resetSearch()
            searchFrom = frameStart + start(firstBody + symbolsFor(bodyLen))
            return
        }
        if agree >= headerSureAgreement {
            events.onHeader?(ch.band)
            pool.noteStart(band: ch.band, at: frameStart, span: start(firstBody + symbolsFor(bodyBytes)))
        }
        state = .body
        tryBlocks()
    }

    /// The header closes its CRC-8 with another channel's id: another sender, not read as ours.
    private func foreignHeader(_ soft: Soft) -> Bool {
        let b = hardBytes(Array(soft[0..<(Protocol.headerBytes * 2)]))
        let magic = b[0], length = b[1], crc = b[2], version = b[3]
        if Int(version >> 4) != Protocol.version || Int((version >> 1) & 7) == ch.band.channelId { return false }
        return crc == crc8([magic, length, version, ch.profile == .robust ? 1 : 0])
    }

    /// Unreadable header: if this channel was already reading this text, the earlier header holds for this lap.
    private func unreadableHeader() {
        let horizon = frameStart - Int(combineSeconds * r.sampleRate)
        let blocks = pool.blocks
        if blocks.total > 0 && blocks.channel == ch.band && blocks.lastAt >= horizon {
            magic = Protocol.magicPayload
            dataLength = blocks.total
        } else {
            pool.headerFails.append((frameStart, r.total))
            resetSearch()
            return
        }
        headerSure = true
        bodyReset()
        state = .body
        tryBlocks()
    }

    private func tryBlocks() {
        let first = firstBody
        let maxSymbols = symbolsFor(bodyBytesMax)
        while bodyRead < maxSymbols && r.total >= symbolEnd(first + bodyRead) {
            bodySoft += softSymbol(first + bodyRead)
            bodyRead += 1
        }
        // A header with errors may announce a false length, and waiting for that frame would leave the channel deaf. If the
        // first block does not close either, it was not this frame: search again. The sender repeats, so a real frame is not lost.
        if !headerSure && dataLength > 1 && bodySoft.count >> 1 >= Protocol.blockBytes {
            let opening = Codec.decodeBlock(hardBytes(Array(bodySoft[0..<(2 * Protocol.blockBytes)])))
            if opening == nil || opening!.block.index != 0 || opening!.block.total != dataLength {
                pool.failures += 1
                resetSearch()
                return
            }
            headerSure = true
        }
        let lastStart = (dataLength - 1) * Protocol.blockBytes
        let available = bodySoft.count >> 1
        var length = lastTried + 1
        while length <= min(available - lastStart, Protocol.blockBytes) {
            lastTried = length
            let bytes = hardBytes(Array(bodySoft[(2 * lastStart)..<(2 * (lastStart + length))]))
            if let d = Codec.decodeBlock(bytes), d.block.index == dataLength - 1, d.block.total == dataLength {
                return finishBlocks(d, lastStart + length)
            }
            length += 1
        }
        if bodyRead == maxSymbols && lastTried >= Protocol.blockBytes { finishBlocks(nil, bodyBytesMax) }
    }

    private func closeBlock(_ index: Int, _ soft: Soft, _ pending: [Soft]) -> (block: Block, corrected: Int)? {
        var candidates = [soft]
        if !pending.isEmpty { candidates += [summed([soft] + pending), surest([soft] + pending)] }
        for candidate in candidates {
            let bytes = hardBytes(candidate)
            let leastSure = leastSureBytes(candidate, Protocol.blockBytes)
            for f in blockErasureTries {
                if let r = Codec.decodeBlock(bytes, erasures: Array(leastSure.prefix(f))),
                   r.block.index == index, r.block.total == dataLength, r.block.chunk.count == Protocol.blockData { return r }
            }
        }
        return nil
    }

    /// The last block does not say its length: every length is tried with the readings of several laps summed.
    private func closeLast(_ readings: [Soft]) -> (block: Block, corrected: Int)? {
        for candidate in [summed(readings), surest(readings)] {
            let bytes = hardBytes(candidate)
            for length in (Protocol.blockHeader + Protocol.blockParity + 1)...Protocol.blockBytes {
                if let r = Codec.decodeBlock(Array(bytes[0..<min(length, bytes.count)])),
                   r.block.index == dataLength - 1, r.block.total == dataLength { return r }
            }
        }
        return nil
    }

    private func finishBlocks(_ last: (block: Block, corrected: Int)?, _ usedBytes: Int) {
        let a = pool.blocks
        let n = dataLength
        let end = frameStart + start(firstBody + symbolsFor(usedBytes))
        a.begin(total: n, at: frameStart, horizon: frameStart - Int(combineSeconds * r.sampleRate), channel: ch.band)
        var gained = 0, corrected = 0
        let lastStart = (n - 1) * Protocol.blockBytes
        if let last = last {
            if a.chunks[last.block.index] == nil { gained += 1 }
            a.chunks[last.block.index] = last.block.chunk
            a.knownBytes = usedBytes
            corrected += last.corrected
        } else if a.chunks[n - 1] == nil && bodySoft.count > 2 * lastStart {
            var soft = Array(bodySoft[(2 * lastStart)..<min(2 * (lastStart + Protocol.blockBytes), bodySoft.count)])
            while soft.count < Protocol.blockBytes * 2 { soft.append(uniformNibble) }
            a.pendingLast = Array((a.pendingLast + [soft]).suffix(combineMax))
            if a.pendingLast.count > 1, let r = closeLast(a.pendingLast) {
                a.chunks[n - 1] = r.block.chunk
                a.pendingLast = []
                gained += 1
                corrected += r.corrected
            }
        }
        for i in 0..<max(n - 1, 0) {
            if a.chunks[i] != nil { continue }
            if 2 * (i + 1) * Protocol.blockBytes > bodySoft.count { break }
            let soft = Array(bodySoft[(2 * i * Protocol.blockBytes)..<(2 * (i + 1) * Protocol.blockBytes)])
            if let r = closeBlock(i, soft, a.pending[i] ?? []) {
                a.chunks[i] = r.block.chunk
                a.pending[i] = nil
                gained += 1
                corrected += r.corrected
            } else {
                a.pending[i] = Array(((a.pending[i] ?? []) + [soft]).suffix(combineMax - 1))
            }
        }
        a.loops += 1
        resetSearch()
        // Without the last block the end is unknown: the next lap may have started before the maximum.
        searchFrom = last != nil ? end : min(end, frameStart + start(firstBody + symbolsFor(bodyBytes)))
        if gained > 0 { events.onBlocks?(a.chunks.count, n) }
        if a.chunks.count == n {
            let text = Codec.assemble((0..<n).map { a.chunks[$0]! })
            let loops = a.loops
            a.reset()
            if let text = text { return deliver(text, corrected: corrected, combined: loops, end: end) }
            pool.failures += 1
            return
        }
        if gained == 0 { pool.failures += 1 }
    }

    /// Not delivered if a frame from another channel started while this one was playing.
    private func deliver(_ text: String, corrected: Int, combined: Int, end: Int) {
        let preambleLen = start(Protocol.preamble.count)
        let begin = frameStart
        if pool.starts.contains(where: { $0.band != ch.band && $0.at > begin && $0.at + preambleLen < end }) {
            pool.clash()
        } else {
            events.onFrame?(Heard(text: text, band: ch.band, profile: ch.profile, correctedBytes: corrected, combined: combined, score: frameScore))
        }
    }

    private func currentProgress() -> Double {
        if state == .search { return -1 }
        let clamp = { (x: Double) in min(max(x, 0), 1) }
        let headerEnd = symbolEnd(firstBody - 1)
        let total = r.total
        if state == .header { return headerShare * clamp(Double(total - frameStart) / Double(headerEnd - frameStart)) }
        let known = magic == Protocol.magicPayload ? pool.blocks.knownBytes : 0
        let end = symbolEnd(firstBody + symbolsFor(known != 0 ? known : bodyBytes) - 1)
        return headerShare + (1 - headerShare) * clamp(Double(total - headerEnd) / Double(end - headerEnd))
    }
}
