// Listens to the microphone and decodes frames (iOS only).
#if os(iOS)
import AVFoundation

/// Taps the microphone (measurement mode, no voice processing) and runs a `Listener` (or `Ears`, see `onAir`) on a background queue.
/// Call `start()` and `stop()` on the main queue; callbacks arrive there too, and none arrives after `stop()`.
public final class SoundListener {
    private let engine = AVAudioEngine()
    private let queue = DispatchQueue(label: "BySound.listener", qos: .userInitiated)
    private var ears: Ears?
    /// Bumped by every start and stop: events of an older run still queued for the main queue are dropped.
    private var generation = 0

    /// A text (or a reference fingerprint, see `Heard.kind`) was decoded.
    public var onFrame: ((Heard) -> Void)?
    /// Everything the session layer hears (frames, headers, clashes, acks, risk frames, noise). Set it before `start()`: acks, risk frames
    /// and noise reports are only read when it is set.
    public var onAir: ((AirEvent) -> Void)?
    /// Sample rate of the microphone while listening.
    public private(set) var sampleRate = 0.0
    /// Microphone RMS level, about every 60 ms (0 ... 1 for full scale).
    public var onLevel: ((Float) -> Void)?

    /// Listening stopped by itself (audio route/configuration change or interruption). `isListening` is already false.
    public var onStopped: (() -> Void)?

    private var observers: [NSObjectProtocol] = []

    /// True while the microphone is tapped.
    public private(set) var isListening = false

    public init() {}

    /// Asks for microphone permission (shows the system prompt the first time).
    public static func requestPermission(_ completion: @escaping (Bool) -> Void) {
        let done = { (granted: Bool) in DispatchQueue.main.async { completion(granted) } }
        if #available(iOS 17, *) {
            AVAudioApplication.requestRecordPermission(completionHandler: done)
        } else {
            AVAudioSession.sharedInstance().requestRecordPermission(done)
        }
    }

    /// Starts listening. Throws if the audio session or engine cannot start; call after permission was granted.
    public func start() throws {
        stop()
        try BySoundAudio.configureSession()

        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        guard format.sampleRate > 0, format.channelCount > 0 else {
            throw NSError(domain: "BySound", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "Microphone input is not available (invalid input format: \(format.sampleRate) Hz, \(format.channelCount) channels)",
            ])
        }
        let rate = format.sampleRate
        sampleRate = rate
        generation += 1
        let run = generation
        let ears = Ears(sampleRate: rate, robust: true, control: onAir != nil) { [weak self] e in
            DispatchQueue.main.async {
                guard let self = self, self.generation == run else { return }
                if case .frame(let h) = e { self.onFrame?(h) }
                self.onAir?(e)
            }
        }
        ears.onLevel = { [weak self] l in
            DispatchQueue.main.async { if let self = self, self.generation == run { self.onLevel?(l) } }
        }
        self.ears = ears

        input.installTap(onBus: 0, bufferSize: 2048, format: format) { [weak self] buffer, _ in
            guard let self = self, let data = buffer.floatChannelData else { return }
            let samples = Array(UnsafeBufferPointer(start: data[0], count: Int(buffer.frameLength)))
            self.queue.async { ears.feed(samples) }
        }
        engine.prepare()
        do {
            try engine.start()
        } catch {
            input.removeTap(onBus: 0)
            self.ears = nil
            generation += 1
            throw error
        }
        isListening = true
        observers = BySoundAudio.observeStops(engine: engine) { [weak self] in
            guard let self = self, self.isListening else { return }
            self.stop()
            self.onStopped?()
        }
    }

    /// Stops listening and releases the microphone. Safe to call when idle.
    public func stop() {
        guard isListening else { return }
        observers.forEach(NotificationCenter.default.removeObserver)
        observers = []
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        queue.sync {}
        generation += 1
        ears = nil
        isListening = false
    }
}
#endif
