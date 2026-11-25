// Hears both profiles at once, like `host.ts`.
import Foundation

/// Runs a normal receiver (all seven bands) and a robust receiver (top, high, upper, mid) on the same audio.
/// Not thread safe: feed it from one queue.
public final class Listener {
    private let normal: Receiver
    private let robust: Receiver?
    private let levelEvery: Int
    private var count = 0
    private var energy = 0.0

    /// Called for every decoded frame.
    public var onFrame: ((Heard) -> Void)?
    /// RMS of the audio, about every 60 ms.
    public var onLevel: ((Float) -> Void)?
    public var onClash: (() -> Void)?

    public init(sampleRate: Double, robust: Bool = true) {
        normal = Receiver(sampleRate: sampleRate, profile: .normal)
        self.robust = robust ? Receiver(sampleRate: sampleRate, profile: .robust) : nil
        levelEvery = Int((0.06 * sampleRate).rounded())
        for r in [normal, self.robust].compactMap({ $0 }) {
            r.onFrame = { [unowned self] h in self.onFrame?(h) }
            r.onClash = { [unowned self] in self.onClash?() }
        }
    }

    public var failures: Int { normal.failures + (robust?.failures ?? 0) }

    public func feed(_ samples: [Float]) {
        normal.feed(samples)
        robust?.feed(samples)
        for v in samples { energy += Double(v) * Double(v) }
        count += samples.count
        if count >= levelEvery {
            onLevel?(Float((energy / Double(count)).squareRoot()))
            count = 0
            energy = 0
        }
    }
}
