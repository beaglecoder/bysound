// Plays a frame in a loop through the loudspeaker (iOS only).
#if os(iOS)
import AVFoundation

/// Plays one frame over and over until `stop()` is called.
/// The audio session is `.playAndRecord` with `.defaultToSpeaker`, so sound always leaves through the loudspeaker
/// (never a headset), and the level is 1.0. Set the device volume to maximum for the best range.
public final class SoundSender {
    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private var attached = false

    public private(set) var isPlaying = false

    public init() {}

    /// Starts sending `text` repeatedly. Throws if the text is too long or the audio engine cannot start.
    public func start(text: String, band: Band, profile: Profile = .normal) throws {
        stop()
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker])
        try session.setActive(true)
        try? session.overrideOutputAudioPort(.speaker)

        let rate = session.sampleRate > 0 ? session.sampleRate : 48000
        let samples = try Modem.encode(text: text, band: band, profile: profile, sampleRate: rate, level: 1)
        guard let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 1),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(samples.count)) else { return }
        buffer.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { buffer.floatChannelData![0].update(from: $0.baseAddress!, count: samples.count) }

        if !attached {
            engine.attach(player)
            attached = true
        }
        engine.disconnectNodeOutput(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)
        player.volume = 1
        engine.mainMixerNode.outputVolume = 1
        try engine.start()
        player.scheduleBuffer(buffer, at: nil, options: .loops)
        player.play()
        isPlaying = true
    }

    public func stop() {
        guard isPlaying else { return }
        player.stop()
        engine.stop()
        isPlaying = false
    }
}
#endif
