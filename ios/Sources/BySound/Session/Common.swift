// Shared by the sender and the receiver: timings, errors, alerts and the per-device memory of risk incidents. See docs/SESSION.md.
import Foundation

/// After every repetition the sender is silent and listens this long (acks, risk frames, other emitters).
public let listenWindowSeconds = 2.0
/// Own content heard this long after the last own repetition ended is someone else playing it (a clone).
public let cloneGuardSeconds = 0.5
/// Receiver: keeps listening this long after the first frame before delivering.
public let finalListenSeconds = 1.5
/// A header on another band during the final listen extends it to at least this long after the header...
public let finalExtendSeconds = 2.5
/// ...but never past this, counted from the first frame.
public let finalListenMaxSeconds = 6.0
/// Ack run: one repetition plus two listen windows plus this margin, clamped to [ackRunMinSeconds, ackRunMaxSeconds].
public let ackMarginSeconds = 0.5
public let ackRunMinSeconds = 3.0
public let ackRunMaxSeconds = 20.0
/// Receiver: after each ack run it listens this long for the receipt.
public let receiptListenSeconds = 3.0
/// Receiver: stops acking this long after delivery even without a receipt.
public let ackTotalSeconds = 30.0
/// Receiver: a reference resolver that has not answered by then fails with TIMEOUT.
public let resolveTimeoutSeconds = 15.0

/// How long the receiver loops acks; both sides compute it from the repetition length (frame + gap).
public func ackRunSeconds(repetition: Double) -> Double {
    min(ackRunMaxSeconds, max(ackRunMinSeconds, repetition + 2 * listenWindowSeconds + ackMarginSeconds))
}
/// How long the sender loops receipts after hearing an ack: covers the end of the ack run and the receiver's receipt listen.
public func receiptSeconds(repetition: Double) -> Double { ackRunSeconds(repetition: repetition) + receiptListenSeconds }

public enum SessionErrorCode: String, Sendable {
    /// Sender, pre-listen: another transmission is already sounding. Nothing was sent.
    case otherTransmission = "OTHER_TRANSMISSION"
    /// Sender: its own data is being played by someone else.
    case clonedTransmission = "CLONED_TRANSMISSION"
    /// Sender: another transmission was heard while sending.
    case otherEmitter = "OTHER_EMITTER"
    /// A risk frame was heard (and honored).
    case riskSignal = "RISK_SIGNAL"
    /// Receiver: two different transmissions, or two overlapping frames.
    case multipleTransmissions = "MULTIPLE_TRANSMISSIONS"
    case timeout = "TIMEOUT"
    /// Sender: `stop()` was called before the ack.
    case stopped = "STOPPED"
    /// Receiver: `cancel()` was called.
    case cancelled = "CANCELLED"
    /// Receiver, reference mode: the resolver did not know the fingerprint.
    case notFound = "NOT_FOUND"
    /// Receiver, reference mode: the resolved text does not match the fingerprint.
    case mismatch = "MISMATCH"
    /// Receiver: a reference frame was heard and there is no resolver.
    case noResolver = "NO_RESOLVER"
}

public struct SessionError: Error, CustomStringConvertible {
    public let code: SessionErrorCode
    public let message: String
    /// The risk frame behind the error, if there was one.
    public let alert: Alert?
    public init(_ code: SessionErrorCode, _ message: String, alert: Alert? = nil) { self.code = code; self.message = message; self.alert = alert }
    public var description: String { "\(code.rawValue): \(message)" }
}

/// A risk frame this device emitted or heard.
public struct Alert: Equatable, Sendable {
    public enum Direction: Sendable { case emitted, heard }
    public var direction: Direction
    public var reason: RiskReason
    /// Who originated the alert.
    public var source: Role
    public var hops: Int
    /// Short ids (6 hex) carried by the frame.
    public var own: String
    public var intruder: String
    /// Heard: the band it was heard on. Emitted: the bands it is looped on.
    public var bands: [Band]
    /// Heard: the frame's `own` is the short id of this side's data. Informative only.
    public var matchesMine: Bool
    /// Emitted as a relay of a heard alert.
    public var relay: Bool

    init(_ direction: Direction, _ r: RiskFrame, bands: [Band], matchesMine: Bool, relay: Bool) {
        self.direction = direction; reason = r.reason; source = r.source; hops = r.hops; own = r.own; intruder = r.intruder
        self.bands = bands; self.matchesMine = matchesMine; self.relay = relay
    }
}

/// Suggested quarantine after a risk frame. bysound never blocks anything because of it: what to do is up to the app.
public struct QuarantineNotice: Equatable, Sendable {
    public var durationMs: Int
    public var reason: RiskReason
    public var source: Role
}

/// Per device: the incidents it emitted or relayed, so it ignores its own frames and relays an incident at most once.
public final class RiskMemory {
    private var incidents: [String: Double] = [:]
    public init() {}
    /// Remembers that this device emitted (or relayed) `key` at `now` (seconds).
    public func note(_ key: String, at now: Double) { incidents[key] = now }
    /// This device emitted `key` within the last `window` seconds.
    public func isOwn(_ key: String, now: Double, window: Double) -> Bool {
        guard let at = incidents[key] else { return false }
        return now - at < window
    }
    /// Shared by every session that does not pass its own.
    public static let shared = RiskMemory()
}

/// Short id (6 hex) of what a frame carries.
func shortOf(_ h: Heard) -> String { h.kind == .payload ? Ids.shortId(h.text) : String(h.fingerprint.prefix(6)) }

/// One repetition of a frame, with its gap, at the device rate.
func renderRepetition(text: String, mode: ContentMode, band: Band, rate: Double, level: Float, set: Int = 0, profile: Profile = .normal) throws -> [Float] {
    let f: Frame
    switch mode {
    case .payload: f = try Codec.payloadFrame(text, band: band, profile: profile)
    case .reference: f = Reference.frame(fingerprint: Ids.bytes(fromHex: Ids.fingerprint(text)), band: band, set: set, profile: profile)
    }
    let top = f.channel.toneHz(group: f.channel.groups - 1, tone: Protocol.tones - 1)
    guard top < rate / 2 else { throw ModemError.bandAboveNyquist(highestToneHz: top, sampleRate: rate) }
    return Modem.fitLevel(Modem.render(f.symbols, f.channel, rate: rate, gapSeconds: Protocol.gapSeconds), level: level)
}

/// A control frame looped back to back (`controlGapSeconds` after each), alternating `frames`, whole frames only, at least `seconds`.
func loopFrames(_ frames: [Frame], seconds: Double, rate: Double, level: Float) -> [Float] {
    let audio = frames.map { Modem.fitLevel(Modem.render($0.symbols, $0.channel, rate: rate, gapSeconds: Frames.controlGapSeconds), level: level) }
    var out: [Float] = []
    var i = 0
    while Double(out.count) < seconds * rate {
        out += audio[i % audio.count]
        i += 1
    }
    return out
}

/// The risk frame looped on its bands (see `Frames.riskBands`).
func riskAudio(_ r: RiskFrame, work: Band, avoid: Band?, seconds: Double, rate: Double, level: Float) -> (bands: [Band], audio: [Float]) {
    let bands = Frames.riskBands(work: work, avoid: avoid)
    return (bands, loopFrames(bands.map { Frames.risk(r, band: $0) }, seconds: seconds, rate: rate, level: level))
}

/// What both state machines share: timers and sounds that a stop cancels at once.
final class Scheduler {
    let platform: SessionPlatform
    private var timers: [Int: Cancel] = [:]
    private var sounds: [Int: Cancel] = [:]
    private var next = 0
    init(_ platform: SessionPlatform) { self.platform = platform }

    @discardableResult
    func after(_ seconds: Double, _ fn: @escaping () -> Void) -> Cancel {
        let id = next
        next += 1
        let cancel = platform.after(seconds) { [weak self] in
            guard let self = self, self.timers.removeValue(forKey: id) != nil else { return }
            fn()
        }
        timers[id] = cancel
        return { [weak self] in if let c = self?.timers.removeValue(forKey: id) { c() } }
    }

    @discardableResult
    func play(_ audio: [Float], onEnd: @escaping () -> Void) -> Cancel {
        let id = next
        next += 1
        let stop = platform.play(audio) { [weak self] in
            guard let self = self, self.sounds.removeValue(forKey: id) != nil else { return }
            onEnd()
        }
        sounds[id] = stop
        return { [weak self] in if let c = self?.sounds.removeValue(forKey: id) { c() } }
    }

    var playing: Bool { !sounds.isEmpty }

    func clear() {
        let t = timers, s = sounds
        timers = [:]
        sounds = [:]
        for c in t.values { c() }
        for c in s.values { c() }
    }
}

/// A value that arrives once, as a `Result`. Read it synchronously (`result`) or wait for it (`value`). Thread safe.
public final class Settled<T> {
    private let lock = NSLock()
    private var stored: Result<T, SessionError>?
    private var waiters: [(Result<T, SessionError>) -> Void] = []
    public init() {}

    public var result: Result<T, SessionError>? { lock.lock(); defer { lock.unlock() }; return stored }
    public var isSettled: Bool { result != nil }
    public var success: T? { if case .success(let v)? = result { return v } else { return nil } }
    public var failure: SessionError? { if case .failure(let e)? = result { return e } else { return nil } }

    /// The first call wins.
    func settle(_ r: Result<T, SessionError>) {
        lock.lock()
        if stored != nil { lock.unlock(); return }
        stored = r
        let w = waiters
        waiters = []
        lock.unlock()
        for f in w { f(r) }
    }
    func succeed(_ v: T) { settle(.success(v)) }
    func fail(_ e: SessionError) { settle(.failure(e)) }

    /// Calls `f` once, now if already settled.
    public func onSettle(_ f: @escaping (Result<T, SessionError>) -> Void) {
        lock.lock()
        if let r = stored { lock.unlock(); return f(r) }
        waiters.append(f)
        lock.unlock()
    }

    /// The value, waiting for it; throws the `SessionError` otherwise.
    public var value: T {
        get async throws {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<T, Error>) in
                onSettle { c.resume(with: $0.mapError { $0 as Error }) }
            }
        }
    }
}
