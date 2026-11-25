// Listens to the microphone and decodes frames (iOS only).
#if os(iOS)
import AVFoundation

/// Taps the microphone (measurement mode, no voice processing) and runs a `Listener` on a background queue.
/// Callbacks arrive on the main queue.
public final class SoundListener {
    private let engine = AVAudioEngine()
    private let queue = DispatchQueue(label: "BySound.listener", qos: .userInitiated)
    private var listener: Listener?

    /// A text was decoded.
    public var onFrame: ((Heard) -> Void)?
    /// Microphone RMS level, about every 60 ms (0 ... 1 for full scale).
    public var onLevel: ((Float) -> Void)?

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
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .measurement, options: [.defaultToSpeaker])
        try session.setActive(true)

        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        let rate = format.sampleRate
        let listener = Listener(sampleRate: rate)
        listener.onFrame = { [weak self] h in DispatchQueue.main.async { self?.onFrame?(h) } }
        listener.onLevel = { [weak self] l in DispatchQueue.main.async { self?.onLevel?(l) } }
        self.listener = listener

        input.installTap(onBus: 0, bufferSize: 2048, format: format) { [weak self] buffer, _ in
            guard let self = self, let data = buffer.floatChannelData else { return }
            let samples = Array(UnsafeBufferPointer(start: data[0], count: Int(buffer.frameLength)))
            self.queue.async { listener.feed(samples) }
        }
        engine.prepare()
        try engine.start()
        isListening = true
    }

    public func stop() {
        guard isListening else { return }
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        queue.sync {}
        listener = nil
        isListening = false
    }
}
#endif
