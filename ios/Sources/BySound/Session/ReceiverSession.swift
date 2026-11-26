// Session receiver (docs/SESSION.md section 6): first frame -> final listen -> delivery -> acks until the receipt, and the confirm gate
// that keeps watching for other emitters until the app calls `confirmed()`. A plain state machine over `SessionPlatform` and `AirEvent`s.
import Foundation

public struct ReceiverOptions {
    /// Fails with `.timeout` if no frame was heard by then.
    public var timeoutSeconds = 60.0
    /// Reference mode: returns the text whose fingerprint (24 hex) was heard, from data the app already has. It calls `done` once, now
    /// or later (`nil`: unknown). bysound checks that SHA-256(text) starts with the fingerprint.
    public var resolve: ((_ fingerprint: String, _ done: @escaping (String?) -> Void) -> Void)?
    /// Acknowledge by sound after delivery.
    public var ack = true
    /// Output level 0...1 of acks and risk frames.
    public var level: Float = 1
    public var securityLevel = SecurityLevel.default
    public var security = SecurityOverrides()
    public var memory: RiskMemory?
    public var onAlert: ((Alert) -> Void)?
    public var onQuarantine: ((QuarantineNotice) -> Void)?
    /// The first frame was heard (delivery follows after the final listen).
    public var onHeard: ((Band) -> Void)?
    public var onState: ((ReceiverState) -> Void)?
    public init() {}

    /// Reference mode with a synchronous lookup, for example a dictionary: `options.resolve(from: catalog)`.
    public mutating func resolve(from catalog: [String: String]) { resolve = { fp, done in done(catalog[fp]) } }
    /// Reference mode with a synchronous lookup that may fail (a throw counts as unknown).
    public mutating func resolve(_ lookup: @escaping (String) throws -> String?) { resolve = { fp, done in done((try? lookup(fp)) ?? nil) } }
}

public enum ReceiverState: String, Sendable { case listening, finalListen = "final-listen", delivered, closed, failed }

public struct Received: Equatable {
    public var text: String
    public var band: Band
    public var via: ContentMode
}

public struct Confirmation: Equatable {
    public enum Watch: String, Sendable {
        /// Watched until now.
        case clean
        /// The watch hit `confirmWatchSeconds` first.
        case expired
        /// No watch at this level.
        case off
    }
    public var watch: Watch
    public var watchedSeconds: Double
    /// From the end of the watch to the `confirmed()` call (expired only).
    public var uncoveredSeconds: Double
}

/// Not thread safe: call `hear` and let the platform call back from one queue.
public final class ReceiverSession {
    public private(set) var state = ReceiverState.listening
    public let result = Settled<Received>()
    /// `true` when the sender's receipt was heard; `false` when acking ended without one (or acks are off).
    public let receipt = Settled<Bool>()
    /// Settles when the session needs neither the microphone nor the speaker any more.
    public let finished = Settled<Void>()
    public let security: Security

    private let platform: SessionPlatform
    private var options: ReceiverOptions
    private let s: Scheduler
    private let memory: RiskMemory
    private var first: Heard?
    private var firstAt = 0.0
    private var finalEnd = 0.0
    private var finalOver = false
    private var cancelFinal: Cancel?
    private var text: String?
    private enum Resolution { case none, pending, done }
    private var resolution = Resolution.none
    private var received: Received?
    private var conflict: SessionError?
    private enum Closed { case confirmed, cancelled }
    private var closed: Closed?
    private var confirmation: Confirmation?
    private var watching = false
    private var watchStart = 0.0
    private var watchEnd: Double?
    private var watchExpired = false
    private var acking = false
    private var alerting = false
    private var seen = Set<String>()
    private var stopAcks: Cancel?
    private var ackAudio: [Float] = []
    private var ackUntil = 0.0

    public init(platform: SessionPlatform, options: ReceiverOptions = ReceiverOptions()) {
        self.platform = platform
        self.options = options
        security = Security.resolve(options.securityLevel, options.security)
        memory = options.memory ?? RiskMemory.shared
        s = Scheduler(platform)
    }

    public func start() {
        s.after(options.timeoutSeconds) { [weak self] in
            if let self = self, self.state == .listening { self.fail(.timeout, "No transmission was heard") }
        }
    }

    /// The confirm gate: call it before acting on the data. Waits for the result (and fails like it), then fails if another emitter or
    /// a risk frame showed up while watching.
    public func confirmed() async throws -> Confirmation {
        _ = try await result.value
        return try confirmedNow()
    }

    /// `confirmed()` for a result that already arrived (`result.isSettled`); throws its error otherwise.
    public func confirmedNow() throws -> Confirmation {
        if case .failure(let e)? = result.result { throw e }
        if closed == .cancelled { throw SessionError(.cancelled, "The session was cancelled before confirming") }
        if let c = conflict { throw c }
        if let c = confirmation { return c }
        let now = platform.now()
        let c: Confirmation
        if !security.confirmWatch { c = Confirmation(watch: .off, watchedSeconds: 0, uncoveredSeconds: 0) }
        else if watchExpired { c = Confirmation(watch: .expired, watchedSeconds: watchEnd! - watchStart, uncoveredSeconds: now - watchEnd!) }
        else { c = Confirmation(watch: .clean, watchedSeconds: (watchEnd ?? now) - watchStart, uncoveredSeconds: 0) }
        closed = .confirmed
        confirmation = c
        stopWatch()
        setState(.closed)
        maybeFinished()
        return c
    }

    /// The person left without confirming: stop listening and acking. Never throws. A risk frame already sounding finishes.
    public func cancel() {
        if closed != nil { return }
        closed = .cancelled
        fail(.cancelled, "Cancelled by the app")
        stopWatch()
        endAcks(false)
        if state == .delivered { setState(.closed) }
        maybeFinished()
    }

    public func hear(_ e: AirEvent) {
        switch e {
        case .frame(let h): heardFrame(h)
        case .header(let b): heardHeader(b)
        case .clash: heardClash()
        case .ack(let a, _): heardAck(a)
        case .risk(let r, let b): heardRisk(r, b)
        case .noise: break
        }
    }

    private func setState(_ next: ReceiverState) {
        if state == next { return }
        state = next
        options.onState?(next)
    }

    private func same(_ a: Heard, _ b: Heard) -> Bool {
        a.kind == .payload ? b.kind == .payload && a.text == b.text : b.kind == .reference && a.fingerprint == b.fingerprint
    }

    private func heardFrame(_ h: Heard) {
        let sec = security
        if state == .listening {
            first = h
            firstAt = platform.now()
            options.onHeard?(h.band)
            if h.kind == .payload { text = h.text }
            else if options.resolve == nil { return fail(.noResolver, "A reference was heard and there is no resolver") }
            else { resolution = .pending }
            if sec.detectMultipleEmitters {
                setState(.finalListen)
                finalEnd = firstAt + finalListenSeconds
                cancelFinal = s.after(finalListenSeconds) { [weak self] in self?.finalListenOver() }
            } else {
                finalOver = true
            }
            if h.kind == .reference { startResolve(h.fingerprint) }
            tryDeliver()
            return
        }
        guard let first = first, !same(first, h) else { return } // failed before any frame, or the sender repeating
        if state == .finalListen {
            return fail(.multipleTransmissions, "Two different transmissions were heard", risk: (.other, shortOf(h), h.band))
        }
        if state == .delivered && watching {
            conflictWith(.multipleTransmissions, "Another transmission appeared before confirming", reason: .other, intruder: shortOf(h), avoid: h.band)
        }
    }

    /// A frame starting on another band during the final listen: wait for it, up to `finalListenMaxSeconds`.
    private func heardHeader(_ band: Band) {
        guard state == .finalListen, let first = first, band != first.band else { return }
        let end = min(max(finalEnd, platform.now() + finalExtendSeconds), firstAt + finalListenMaxSeconds)
        if end <= finalEnd { return }
        finalEnd = end
        cancelFinal?()
        cancelFinal = s.after(end - platform.now()) { [weak self] in self?.finalListenOver() }
    }

    private func heardClash() {
        if !security.detectMultipleEmitters || (state != .listening && state != .finalListen) { return }
        fail(.multipleTransmissions, "Two transmissions overlapped", risk: (.overlap, Ids.unknownShort, first?.band))
    }

    private func heardAck(_ a: AckFrame) {
        if a.receipt && acking, let text = text, a.id == Ids.ackId(text) { endAcks(true) }
    }

    private func heardRisk(_ r: RiskFrame, _ band: Band) {
        let cfg = security.riskSignal
        let key = r.key, now = platform.now()
        if !cfg.listen || seen.contains(key) || memory.isOwn(key, now: now, window: Double(cfg.quarantineMs) / 1000) { return }
        let before = state == .listening || state == .finalListen
        let watchingRisk = state == .delivered && watching && cfg.listenUntil == .confirm
        if !before && !watchingRisk { return }
        seen.insert(key)
        let mine = first.map(shortOf)
        let alert = Alert(.heard, r, bands: [band], matchesMine: r.own == mine, relay: false)
        options.onAlert?(alert)
        quarantine(r)
        if !cfg.honor { return }
        // The receiver never relays: the sender that hears it does.
        if before { fail(.riskSignal, "A risk signal was heard", heard: alert) }
        else { conflictWith(.riskSignal, "A risk signal was heard before confirming", reason: nil, intruder: "", avoid: band, heard: alert) }
    }

    private func startResolve(_ fp: String) {
        var answered = false
        let timer = s.after(resolveTimeoutSeconds) { [weak self] in
            if answered { return }
            answered = true
            self?.fail(.timeout, "The resolver did not answer in time")
        }
        options.resolve?(fp) { [weak self] found in
            guard let self = self, !answered else { return }
            answered = true
            timer()
            guard let found = found else { return self.fail(.notFound, "Unknown fingerprint \(fp)") }
            if !Ids.payloadId(found).hasPrefix(fp) { return self.fail(.mismatch, "The resolved text does not match the fingerprint") }
            self.text = found
            self.resolution = .done
            self.tryDeliver()
        }
    }

    private func finalListenOver() {
        cancelFinal = nil
        finalOver = true
        tryDeliver()
    }

    /// Delivers once the final listen is over (or at once without detection) and the text is known.
    private func tryDeliver() {
        if (state != .listening && state != .finalListen) || !finalOver || resolution == .pending { return }
        guard let text = text, let h = first else { return }
        let r = Received(text: text, band: h.band, via: h.kind == .payload ? .payload : .reference)
        received = r
        s.clear() // the listening timeout
        setState(.delivered)
        result.succeed(r)
        let sec = security
        if sec.confirmWatch {
            watching = true
            watchStart = platform.now()
            s.after(sec.confirmWatchSeconds) { [weak self] in
                guard let self = self, self.watching else { return }
                self.watchExpired = true
                self.stopWatch()
                self.maybeFinished()
            }
        }
        if options.ack { startAcks(h, text) } else { receipt.succeed(false) }
        maybeFinished()
    }

    /// Ack runs and receipt listens, alternating, until the receipt or `ackTotalSeconds`.
    private func startAcks(_ h: Heard, _ text: String) {
        acking = true
        let rate = platform.sampleRate
        // Timed with the longest repetition the sender may be playing now (it may have hopped or gone robust since this one).
        let run = ackRunSeconds(repetition: longestRepetitionSeconds(text: h.kind == .payload ? h.text : nil))
        let heard = ReadAt(band: h.band, profile: h.profile)
        let audio = loopFrames([Frames.ack(AckFrame(receipt: false, id: Ids.ackId(text), heard: heard))], seconds: run, rate: rate, level: options.level)
        ackAudio = audio
        ackUntil = platform.now() + ackTotalSeconds
        ackRound()
    }

    private func ackRound() {
        if !acking { return }
        if platform.now() >= ackUntil { return endAcks(false) }
        stopAcks = s.play(ackAudio) { [weak self] in
            self?.stopAcks = nil
            self?.s.after(receiptListenSeconds) { [weak self] in self?.ackRound() }
        }
    }

    private func endAcks(_ got: Bool) {
        if !acking {
            if !got { receipt.succeed(false) }
            return
        }
        acking = false
        stopAcks?()
        stopAcks = nil
        receipt.succeed(got)
        maybeFinished()
    }

    private func stopWatch() {
        if !watching { return }
        watching = false
        watchEnd = platform.now()
    }

    /// Something showed up during the confirm watch: the gate closes for good. A different transmission also gets a risk frame.
    private func conflictWith(_ code: SessionErrorCode, _ message: String, reason: RiskReason?, intruder: String, avoid: Band, heard: Alert? = nil) {
        stopWatch()
        let alert = reason.flatMap { emitRisk($0, intruder, avoid) } ?? heard // before endAcks: `finished` must wait for the risk frame
        endAcks(false)
        conflict = SessionError(code, message, alert: alert)
        maybeFinished()
    }

    /// Before delivery: the result fails. `risk`: also loop a risk frame (emit `.auto`).
    private func fail(_ code: SessionErrorCode, _ message: String, risk: (reason: RiskReason, intruder: String, avoid: Band?)? = nil, heard: Alert? = nil) {
        if state != .listening && state != .finalListen { return }
        s.clear()
        let alert = risk.flatMap { emitRisk($0.reason, $0.intruder, $0.avoid) } ?? heard
        setState(.failed)
        result.fail(SessionError(code, message, alert: alert))
        receipt.succeed(false)
        maybeFinished()
    }

    private func emitRisk(_ reason: RiskReason, _ intruder: String, _ avoid: Band?) -> Alert? {
        let cfg = security.riskSignal
        if cfg.emit != .auto { return nil }
        let r = RiskFrame(reason: reason, source: .receiver, hops: 0, own: first.map(shortOf) ?? Ids.unknownShort, intruder: intruder)
        memory.note(r.key, at: platform.now())
        let work = first?.band ?? avoid ?? .top
        let (bands, audio) = riskAudio(r, work: work, avoid: avoid, seconds: Double(cfg.durationMs) / 1000, rate: platform.sampleRate, level: options.level)
        let alert = Alert(.emitted, r, bands: bands, matchesMine: true, relay: false)
        options.onAlert?(alert)
        quarantine(r)
        alerting = true
        s.play(audio) { [weak self] in
            self?.alerting = false
            self?.maybeFinished()
        }
        return alert
    }

    private func quarantine(_ r: RiskFrame) {
        let ms = security.riskSignal.quarantineMs
        if ms > 0 { options.onQuarantine?(QuarantineNotice(durationMs: ms, reason: r.reason, source: r.source)) }
    }

    private func maybeFinished() {
        let listening = state == .listening || state == .finalListen || watching || acking
        if !listening && !alerting {
            s.clear()
            finished.succeed(())
        }
    }

    /// Short id of what was received, for apps that log alerts.
    public var short: String? { text.map(Ids.shortId) }
}
