// Shared audio-session setup and stop notifications for SoundSender and SoundListener (iOS only).
#if os(iOS)
import AVFoundation

enum BySoundAudio {
    /// One configuration for sending and listening, so using both at once never changes the session mode.
    static func configureSession() throws {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .measurement, options: [.defaultToSpeaker])
        try session.setActive(true)
    }

    /// Calls `handler` when the engine configuration changes or the session is interrupted (audio is dead then).
    static func observeStops(engine: AVAudioEngine, handler: @escaping () -> Void) -> [NSObjectProtocol] {
        let center = NotificationCenter.default
        let block: (Notification) -> Void = { _ in handler() }
        return [
            center.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main, using: block),
            center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { note in
                let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
                if raw == AVAudioSession.InterruptionType.began.rawValue { handler() }
            },
        ]
    }
}
#endif
