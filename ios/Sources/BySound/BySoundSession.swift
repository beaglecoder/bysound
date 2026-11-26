// The session layer on AVFoundation (iOS only): a speaker (`SoundPlayer`) and the microphone (`SoundListener`) wired to the state
// machines of `SenderSession` and `ReceiverSession`. Everything runs on the main queue. docs/SESSION.md is the specification.
#if os(iOS)
import AVFoundation

public enum BySoundSessionError: Error {
    /// The person denied microphone access (or it is restricted).
    case microphoneDenied
}

/// Speaker and clock of the app, as a `SessionPlatform`. Main queue only.
final class AVPlatform: SessionPlatform {
    let player = SoundPlayer()
    var sampleRate: Double { player.sampleRate }
    func now() -> Double { ProcessInfo.processInfo.systemUptime }
    func after(_ seconds: Double, _ fn: @escaping () -> Void) -> Cancel {
        let item = DispatchWorkItem(block: fn)
        DispatchQueue.main.asyncAfter(deadline: .now() + max(0, seconds), execute: item)
        return { item.cancel() }
    }
    func play(_ audio: [Float], onEnd: @escaping () -> Void) -> Cancel { player.play(audio, onEnd: onEnd) }
    func random() -> Double { Double.random(in: 0..<1) }
}

/// Opens the audio of one session and releases it when the session is over. Keeps itself alive until then.
private final class AudioRig {
    let platform = AVPlatform()
    let listener = SoundListener()
    private var retained: AudioRig?

    /// Asks for the microphone if needed, then opens speaker and microphone. Events of the microphone go to `hear`.
    static func open(hear: @escaping (AirEvent) -> Void, onLevel: ((Float) -> Void)?, onStopped: @escaping () -> Void) async throws -> AudioRig {
        let granted = await withCheckedContinuation { (c: CheckedContinuation<Bool, Never>) in SoundListener.requestPermission { c.resume(returning: $0) } }
        guard granted else { throw BySoundSessionError.microphoneDenied }
        return try await MainActor.run {
            let rig = AudioRig()
            try rig.platform.player.open()
            rig.listener.onAir = hear
            rig.listener.onLevel = onLevel
            rig.listener.onStopped = onStopped
            rig.platform.player.onStopped = onStopped
            try rig.listener.start()
            rig.retained = rig
            return rig
        }
    }

    func close() {
        listener.stop()
        platform.player.close()
        retained = nil
    }
}

private func onMain(_ f: @escaping () -> Void) {
    if Thread.isMainThread { f() } else { DispatchQueue.main.async(execute: f) }
}

/// A running send. Create it with `BySoundSession.startSending(text:options:)`.
public final class SendSession: @unchecked Sendable {
    fileprivate let core: SenderSession
    init(core: SenderSession) { self.core = core }

    /// The receiver's ack; throws a `SessionError` otherwise (see docs/SESSION.md section 8).
    public var delivered: Delivery { get async throws { try await core.delivered.value } }
    /// Completes when nothing plays any more and the microphone is released.
    public var finished: Void { get async { _ = try? await core.finished.value } }
    /// Where the session is. Read it on the main queue.
    public var state: SenderState { core.state }
    /// Stops at once (also a risk frame being played). Before the ack, `delivered` fails with `.stopped`.
    public func stop() { onMain { [core] in core.stop() } }
}

/// A running receive. Create it with `BySoundSession.startReceiving(options:)`.
public final class ReceiveSession: @unchecked Sendable {
    fileprivate let core: ReceiverSession
    init(core: ReceiverSession) { self.core = core }

    /// The data, after the final listen. Throws a `SessionError` (`multipleTransmissions`, `riskSignal`, `timeout`...).
    public var result: Received { get async throws { try await core.result.value } }
    /// The confirm gate: call it before acting on the data. Throws if another emitter or a risk frame appeared meanwhile.
    public func confirmed() async throws -> Confirmation {
        _ = try await core.result.value
        return try await MainActor.run { try core.confirmedNow() }
    }
    /// The person left: stop listening and acking. Never throws.
    public func cancel() { onMain { [core] in core.cancel() } }
    /// `true` if the sender answered the ack with a receipt.
    public var receipt: Bool { get async { (try? await core.receipt.value) ?? false } }
    public var finished: Void { get async { _ = try? await core.finished.value } }
    public var state: ReceiverState { core.state }
}

/// What `BySoundSession.receive` returns once the data is in: the data and the gate.
public final class Reception: @unchecked Sendable {
    public let text: String
    public let band: Band
    public let via: ContentMode
    public let session: ReceiveSession
    init(_ r: Received, _ s: ReceiveSession) { text = r.text; band = r.band; via = r.via; session = s }
    public func confirmed() async throws -> Confirmation { try await session.confirmed() }
    public func cancel() { session.cancel() }
    public var receipt: Bool { get async { await session.receipt } }
    public var finished: Void { get async { await session.finished } }
}

/// Sessions on the device audio: pre-listen, channel ladder, acks and receipts, confirm gate, risk frames.
/// Names parallel to the TypeScript `sendSession` / `receiveSession`.
public enum BySoundSession {
    /// Sends `text` as a session and returns the handle once the audio is running: listens first (other emitters, noise per channel),
    /// repeats it with listen windows down a channel ladder until the receiver acknowledges, remembers where it was read
    /// (`UserDefaults`) and watches for other emitters. Needs the microphone. Call `stop()` to give up.
    /// Throws `ModemError` if the text does not fit one transmission.
    public static func startSending(text: String, options: SenderOptions = SenderOptions()) async throws -> SendSession {
        var o = options
        if case .automatic = o.calibration { o.calibration = .store(UserDefaultsCalibrationStore()) }
        let ears = Box<(AirEvent) -> Void>()
        let stopped = Box<() -> Void>()
        let rig = try await AudioRig.open(hear: { ears.value?($0) }, onLevel: nil, onStopped: { stopped.value?() })
        let options = o
        return try await MainActor.run {
            do {
                let core = try SenderSession(text: text, platform: rig.platform, options: options)
                ears.value = { core.hear($0) }
                stopped.value = { core.stop() }
                core.finished.onSettle { _ in rig.close() }
                let session = SendSession(core: core)
                core.start()
                return session
            } catch {
                rig.close()
                throw error
            }
        }
    }

    /// Sends `text` and returns when the receiver acknowledges it (receipts keep playing in the background). Cancelling the task stops
    /// the session. Throws a `SessionError` otherwise.
    public static func send(text: String, options: SenderOptions = SenderOptions()) async throws -> Delivery {
        let session = try await startSending(text: text, options: options)
        return try await withTaskCancellationHandler { try await session.delivered } onCancel: { session.stop() }
    }

    /// Listens for one session and returns the handle once the audio is running: delivers the data, acknowledges it by sound and keeps
    /// the confirm gate. Needs the microphone.
    public static func startReceiving(options: ReceiverOptions = ReceiverOptions(), onLevel: ((Float) -> Void)? = nil) async throws -> ReceiveSession {
        let ears = Box<(AirEvent) -> Void>()
        let stopped = Box<() -> Void>()
        let rig = try await AudioRig.open(hear: { ears.value?($0) }, onLevel: onLevel, onStopped: { stopped.value?() })
        return await MainActor.run {
            let core = ReceiverSession(platform: rig.platform, options: options)
            ears.value = { core.hear($0) }
            stopped.value = { core.cancel() }
            core.finished.onSettle { _ in rig.close() }
            let session = ReceiveSession(core: core)
            core.start()
            return session
        }
    }

    /// Listens until data arrives and returns it with the confirm gate: `try await reception.confirmed()` before acting on `text`.
    /// Cancelling the task cancels the session. Throws a `SessionError` (`multipleTransmissions`, `riskSignal`, `timeout`...).
    public static func receive(options: ReceiverOptions = ReceiverOptions(), onLevel: ((Float) -> Void)? = nil) async throws -> Reception {
        let session = try await startReceiving(options: options, onLevel: onLevel)
        let r = try await withTaskCancellationHandler { try await session.result } onCancel: { session.cancel() }
        return Reception(r, session)
    }
}

private final class Box<T> { var value: T? }
#endif
