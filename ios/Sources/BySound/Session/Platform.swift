// What the session layer hears and what it needs from the device. Both are protocols so the state machines (SenderSession,
// ReceiverSession) run the same on AVFoundation (BySoundSession) and on simulated air (the tests).
import Foundation

/// Cancels a timer or silences a sound. Safe to call more than once.
public typealias Cancel = () -> Void

/// Everything the session layer reacts to.
public enum AirEvent {
    /// A payload or reference frame, fully decoded.
    case frame(Heard)
    /// A payload or reference header read with near certainty: a frame is starting on this band.
    case header(Band)
    /// The modem saw two overlapping frames and delivered neither.
    case clash
    case ack(AckFrame, Band)
    case risk(RiskFrame, Band)
    /// Every 0.25 s: what each band measures (signal included), for the sender's ambient check.
    case noise(BandNoise)
}

/// The device, as the session layer sees it. Times are seconds on one monotonic clock. The state machines are not thread safe:
/// call them, and have the platform call back, from one queue.
public protocol SessionPlatform: AnyObject {
    var sampleRate: Double { get }
    func now() -> Double
    /// Calls `fn` once after `seconds`. Returns a function that cancels it.
    func after(_ seconds: Double, _ fn: @escaping () -> Void) -> Cancel
    /// Plays `audio` from now. `onEnd` runs when it finished on its own (not after stop). Returns stop.
    func play(_ audio: [Float], onEnd: @escaping () -> Void) -> Cancel
    /// Uniform in [0, 1). Injected so tests are reproducible.
    func random() -> Double
}

/// Microphone samples in, `AirEvent`s out: the modem receiver for payload and reference frames, plus `ControlListener` for acks and
/// risk frames. Not thread safe: feed it from one queue.
public final class Ears {
    private let listener: Listener
    private let control: ControlListener?

    /// - Parameters:
    ///   - robust: also listen to the robust profile.
    ///   - control: also read acks, risk frames and noise (needed by sessions).
    public init(sampleRate: Double, robust: Bool = true, control: Bool = true, onEvent: @escaping (AirEvent) -> Void) {
        listener = Listener(sampleRate: sampleRate, robust: robust)
        listener.onFrame = { onEvent(.frame($0)) }
        listener.onHeader = { onEvent(.header($0)) }
        listener.onClash = { onEvent(.clash) }
        self.control = control ? ControlListener(rate: sampleRate, onHeard: { c in
            switch c {
            case .ack(let a, let b): onEvent(.ack(a, b))
            case .risk(let r, let b): onEvent(.risk(r, b))
            }
        }, onNoise: { onEvent(.noise($0)) }) : nil
    }

    /// RMS of the audio, about every 60 ms.
    public var onLevel: ((Float) -> Void)? {
        get { listener.onLevel }
        set { listener.onLevel = newValue }
    }

    public func feed(_ x: [Float]) {
        control?.feed(x)
        listener.feed(x)
    }
}
