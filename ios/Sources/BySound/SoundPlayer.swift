// Plays audio buffers through the loudspeaker, one shot each (iOS only). The session layer plays frames and loops of control frames.
#if os(iOS)
import AVFoundation

/// Plays arbitrary mono buffers, any number at once, each with its own end callback. Create it, `open()` it, `play`, `close()`.
/// Main queue only. The audio session is the one of `SoundSender` / `SoundListener` (`.playAndRecord`, measurement, speaker).
final class SoundPlayer {
    private let engine = AVAudioEngine()
    private var nodes: [Int: AVAudioPlayerNode] = [:]
    private var next = 0
    private var observers: [NSObjectProtocol] = []
    private(set) var isOpen = false
    var onStopped: (() -> Void)?
    private(set) var sampleRate = 48000.0
    static let fadeSeconds = 0.015

    func open() throws {
        try BySoundAudio.configureSession()
        let session = AVAudioSession.sharedInstance()
        try? session.overrideOutputAudioPort(.speaker)
        sampleRate = session.sampleRate > 0 ? session.sampleRate : 48000
        engine.mainMixerNode.outputVolume = 1
        engine.prepare()
        try engine.start()
        isOpen = true
        observers = BySoundAudio.observeStops(engine: engine) { [weak self] in
            guard let self = self, self.isOpen else { return }
            self.close()
            self.onStopped?()
        }
    }

    /// Plays `audio` now. `onEnd` runs (on the main queue) when it finished by itself. Returns stop (with a short fade: an abrupt stop of
    /// a loud high tone clicks).
    func play(_ audio: [Float], onEnd: @escaping () -> Void) -> () -> Void {
        guard isOpen, !audio.isEmpty,
              let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 1),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(audio.count)) else {
            DispatchQueue.main.async(execute: onEnd)
            return {}
        }
        buffer.frameLength = AVAudioFrameCount(audio.count)
        audio.withUnsafeBufferPointer { buffer.floatChannelData![0].update(from: $0.baseAddress!, count: audio.count) }
        let id = next
        next += 1
        let node = AVAudioPlayerNode()
        nodes[id] = node
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        node.volume = 1
        var stopped = false
        node.scheduleBuffer(buffer, completionCallbackType: .dataPlayedBack) { [weak self] _ in
            DispatchQueue.main.async {
                guard let self = self, !stopped, self.nodes[id] != nil else { return }
                self.release(id)
                onEnd()
            }
        }
        node.play()
        return { [weak self] in
            guard !stopped else { return }
            stopped = true
            node.volume = 0
            DispatchQueue.main.asyncAfter(deadline: .now() + SoundPlayer.fadeSeconds) { self?.release(id) }
        }
    }

    private func release(_ id: Int) {
        guard let node = nodes.removeValue(forKey: id) else { return }
        node.stop()
        engine.detach(node)
    }

    func close() {
        guard isOpen else { return }
        isOpen = false
        observers.forEach(NotificationCenter.default.removeObserver)
        observers = []
        for id in Array(nodes.keys) { release(id) }
        engine.stop()
    }
}
#endif
