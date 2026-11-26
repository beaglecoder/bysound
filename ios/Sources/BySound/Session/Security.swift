// Security levels (docs/SESSION.md section 7). The table is test-vectors/security-levels.json; the tests check they are equal.
import Foundation

public enum SecurityLevel: String, CaseIterable, Sendable {
    case basic, standard, maximum
    public static let `default` = SecurityLevel.maximum
}

public struct RiskSignal: Equatable, Sendable {
    public enum Emit: String, Sendable { case auto, off }
    public enum ListenUntil: String, Sendable { case delivery, confirm }
    /// `.auto`: whoever detects another emitter sounds a risk frame. `.off`: it only stops and reports.
    public var emit: Emit
    /// Listen for risk frames.
    public var listen: Bool
    /// Act on one: the sender stops (and relays), the receiver fails / closes the confirm gate. `false`: only `onAlert`.
    public var honor: Bool
    /// Receiver only: `.delivery` stops listening for risk frames at delivery, `.confirm` keeps listening until `confirmed()`.
    public var listenUntil: ListenUntil
    /// How long a receiver loops its risk frame.
    public var durationMs: Int
    /// How long a sender loops its risk frame (after detecting another emitter, or when relaying one).
    public var detectorDurationMs: Int
    /// Suggested quarantine sent to `onQuarantine`. Never enforced.
    public var quarantineMs: Int
}

public struct Security: Equatable, Sendable {
    /// Sender: pre-listen and listen windows act on what they hear. Receiver: final listen and clash rejection.
    public var detectMultipleEmitters: Bool
    /// Sender: other emitters heard while sending (in the listen windows) stop the session.
    public var listenWhileSending: Bool
    /// Sender: listen this long before the first frame, plus up to `scanJitterSeconds` at random (the jitter only with detection).
    public var listenBeforeSendingSeconds: Double
    public var scanJitterSeconds: Double
    /// Receiver: keep watching after delivery until `confirmed()` / `cancel()`, at most `confirmWatchSeconds`.
    public var confirmWatch: Bool
    public var confirmWatchSeconds: Double
    public var riskSignal: RiskSignal

    public static func level(_ level: SecurityLevel) -> Security {
        func risk(_ emit: RiskSignal.Emit, _ on: Bool, _ until: RiskSignal.ListenUntil, _ d: Int, _ det: Int, _ q: Int) -> RiskSignal {
            RiskSignal(emit: emit, listen: on, honor: on, listenUntil: until, durationMs: d, detectorDurationMs: det, quarantineMs: q)
        }
        switch level {
        case .basic:
            return Security(detectMultipleEmitters: false, listenWhileSending: false, listenBeforeSendingSeconds: 1, scanJitterSeconds: 0.6,
                            confirmWatch: false, confirmWatchSeconds: 120, riskSignal: risk(.off, false, .delivery, 4000, 4000, 30000))
        case .standard:
            return Security(detectMultipleEmitters: true, listenWhileSending: true, listenBeforeSendingSeconds: 2, scanJitterSeconds: 0.6,
                            confirmWatch: true, confirmWatchSeconds: 120, riskSignal: risk(.auto, true, .confirm, 4000, 15000, 30000))
        case .maximum:
            return Security(detectMultipleEmitters: true, listenWhileSending: true, listenBeforeSendingSeconds: 3, scanJitterSeconds: 1.2,
                            confirmWatch: true, confirmWatchSeconds: 120, riskSignal: risk(.auto, true, .confirm, 6000, 30000, 60000))
        }
    }
}

/// Explicit values that win over the level, field by field. `nil` keeps the level's value.
public struct SecurityOverrides: Sendable {
    public var detectMultipleEmitters: Bool?
    public var listenWhileSending: Bool?
    public var listenBeforeSendingSeconds: Double?
    public var scanJitterSeconds: Double?
    public var confirmWatch: Bool?
    public var confirmWatchSeconds: Double?
    public var riskSignal = RiskSignalOverrides()
    public init() {}
}

public struct RiskSignalOverrides: Sendable {
    public var emit: RiskSignal.Emit?
    public var listen: Bool?
    public var honor: Bool?
    public var listenUntil: RiskSignal.ListenUntil?
    public var durationMs: Int?
    public var detectorDurationMs: Int?
    public var quarantineMs: Int?
    public init() {}
}

extension Security {
    /// The level with the explicit overrides applied.
    public static func resolve(_ level: SecurityLevel = .default, _ o: SecurityOverrides = SecurityOverrides()) -> Security {
        var s = Security.level(level)
        s.detectMultipleEmitters = o.detectMultipleEmitters ?? s.detectMultipleEmitters
        s.listenWhileSending = o.listenWhileSending ?? s.listenWhileSending
        s.listenBeforeSendingSeconds = o.listenBeforeSendingSeconds ?? s.listenBeforeSendingSeconds
        s.scanJitterSeconds = o.scanJitterSeconds ?? s.scanJitterSeconds
        s.confirmWatch = o.confirmWatch ?? s.confirmWatch
        s.confirmWatchSeconds = o.confirmWatchSeconds ?? s.confirmWatchSeconds
        let r = o.riskSignal
        s.riskSignal.emit = r.emit ?? s.riskSignal.emit
        s.riskSignal.listen = r.listen ?? s.riskSignal.listen
        s.riskSignal.honor = r.honor ?? s.riskSignal.honor
        s.riskSignal.listenUntil = r.listenUntil ?? s.riskSignal.listenUntil
        s.riskSignal.durationMs = r.durationMs ?? s.riskSignal.durationMs
        s.riskSignal.detectorDurationMs = r.detectorDurationMs ?? s.riskSignal.detectorDurationMs
        s.riskSignal.quarantineMs = r.quarantineMs ?? s.riskSignal.quarantineMs
        return s
    }
}
