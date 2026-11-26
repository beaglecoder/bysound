// Session sender (docs/SESSION.md sections 5 and 9): pre-listen (other emitters + ambient), repetitions with listen windows over a
// channel ladder that adapts (noisy channels skipped, robust after a while, calibration remembered), ack -> receipts, other emitters ->
// risk frame, heard risk frames -> stop and relay. A plain state machine over `SessionPlatform` and `AirEvent`s: no AVFoundation here.
import Foundation

/// `.payload` sends the text; `.reference` sends only its 12-byte fingerprint (the receiver resolves it locally).
public enum ContentMode: Sendable { case payload, reference }

/// Where the calibration is kept.
public enum CalibrationSetting {
    /// The default of the platform: in memory (shared by the process) in the core; `UserDefaults` in `BySoundSession`.
    case automatic
    case disabled
    case store(CalibrationStore)
}

public struct SenderOptions {
    /// A fixed band: no ladder, no ambient skip, no calibration.
    public var band: Band?
    /// The ladder, from the least audible band down. Default top, high, upper, mid.
    public var channels: [Band]?
    /// A fixed profile for the data frames. Without it the sender goes robust by itself. Acks and risk frames are always normal.
    public var profile: Profile?
    public var mode: ContentMode = .payload
    /// Output level 0...1 of everything this side plays. Default 1.
    public var level: Float?
    /// Gives up this long after the first frame if no ack arrived.
    public var timeoutSeconds = 60.0
    /// Measure the noise of each channel in the pre-listen and skip the noisy ones.
    public var ambient = true
    public var calibration = CalibrationSetting.automatic
    public var securityLevel = SecurityLevel.default
    /// Explicit values that win over the level.
    public var security = SecurityOverrides()
    /// Risk incidents this device already emitted. Default: one shared by every session.
    public var memory: RiskMemory?
    public var onAlert: ((Alert) -> Void)?
    public var onQuarantine: ((QuarantineNotice) -> Void)?
    public var onState: ((SenderState) -> Void)?
    /// Each repetition as it starts.
    public var onRepetition: ((_ index: Int, _ band: Band, _ profile: Profile) -> Void)?
    /// What the pre-listen measured (`nil` without noise reports) and the route that follows.
    public var onAmbient: ((Ambient?, [Band]) -> Void)?
    public init() {}
}

public enum SenderState: String, Sendable { case preListen = "pre-listen", sending, window, receipt, alerting, done }

public struct Delivery: Equatable {
    /// Where the receiver read it, from the ack; without that in the ack, the band of the last repetition.
    public var band: Band
    public var profile: Profile
    /// The ack said where it was read.
    public var stated: Bool
    /// From the first frame to the ack.
    public var seconds: Double
    public var repetitions: Int
    /// The calibration saved for next time (`nil`: nothing learned).
    public var calibration: Calibration?
}

/// Not thread safe: call `hear` and let the platform call back from one queue.
public final class SenderSession {
    public private(set) var state = SenderState.preListen
    public let delivered = Settled<Delivery>()
    /// Settles when the session is over and plays nothing more (after receipts or the risk frame).
    public let finished = Settled<Void>()
    public let security: Security

    private let text: String
    private let platform: SessionPlatform
    private var options: SenderOptions
    private let mode: ContentMode
    private var level: Float
    private let memory: RiskMemory
    private let store: CalibrationStore?
    private let own: (short: String, ack: String, fingerprint: String)
    private var audio: [String: [Float]] = [:]
    private let s: Scheduler
    private var pending = true
    private var plan = Plan(route: [Adapt.defaultChannels[0]], every: Int.max, calibration: nil, startRobust: nil)
    private var ladder = RobustLadder(auto: false, robust: false)
    private var ambient: Ambient?
    private var noise: [BandNoise] = []
    private var hop = 0
    private var onChannel = 0
    private var sent = 0
    private var played: [Band] = []
    private var band: Band
    private var profile = Profile.normal
    private var playing = false
    private var lastEnd = -Double.infinity
    private var firstAt = 0.0
    private var stopPayload: Cancel?
    private var seen = Set<String>()
    /// Pre-listen heard a frame start or a clash.
    private var busy = false

    /// Throws `ModemError` if the text cannot be sent on the first band of the ladder (too long for one transmission).
    public init(text: String, platform: SessionPlatform, options: SenderOptions = SenderOptions()) throws {
        self.text = text
        self.platform = platform
        self.options = options
        security = Security.resolve(options.securityLevel, options.security)
        mode = options.mode
        level = options.level ?? 1
        memory = options.memory ?? RiskMemory.shared
        switch options.calibration {
        case .automatic: store = MemoryCalibrationStore.shared
        case .disabled: store = nil
        case .store(let c): store = c
        }
        own = (Ids.shortId(text), Ids.ackId(text), Ids.fingerprint(text))
        let chain = options.channels?.isEmpty == false ? options.channels! : Adapt.defaultChannels
        band = options.band ?? chain[0]
        s = Scheduler(platform)
        if options.mode == .payload { _ = try Codec.payloadFrame(text, band: band, profile: options.profile ?? .normal) }
    }

    public func start() {
        let sec = security
        let detect = sec.detectMultipleEmitters
        if sec.listenBeforeSendingSeconds > 0 && (detect || options.ambient) {
            setState(.preListen)
            // The jitter only matters for detection: two senders starting together must not end their pre-listen together.
            let wait = sec.listenBeforeSendingSeconds + (detect ? platform.random() * sec.scanJitterSeconds : 0)
            s.after(wait) { [weak self] in
                guard let self = self else { return }
                if self.busy { self.refuse(.otherTransmission, "Another transmission is already sounding") } else { self.beginSending() }
            }
        } else {
            beginSending()
        }
    }

    /// Stops everything at once, the risk frame included. Before the ack, `delivered` fails with `.stopped`.
    public func stop() {
        fail(.stopped, "Stopped by the app")
        end()
    }

    public func hear(_ e: AirEvent) {
        if state == .done { return }
        switch e {
        case .noise(let n):
            if state == .preListen { noise.append(n) }
            return
        case .risk(let r, let band):
            return heardRisk(r, band)
        case .ack(let a, _):
            if (state == .sending || state == .window) && !a.receipt && a.id == own.ack { acked(a) }
            return
        default: break
        }
        let sec = security
        if !sec.detectMultipleEmitters { return }
        if state == .preListen {
            // A decoded frame refuses at once; a frame start or a clash refuses at the end of the pre-listen, unless the frame is
            // decoded by then (so a replay of our own data is reported as a clone). Acks and risk frames are handled above.
            if case .frame(let h) = e {
                if isOwn(h) { refuse(.clonedTransmission, "Someone else is already playing this data") }
                else { refuse(.otherTransmission, "Another transmission is already sounding") }
            } else {
                busy = true
            }
            return
        }
        guard (state == .sending || state == .window) && sec.listenWhileSending, case .frame(let h) = e else { return }
        if !isOwn(h) { return detected(.other, intruder: shortOf(h), avoid: h.band) }
        // Our own data while we have been silent for a while, or on a band we are not playing: someone else is playing it.
        let silent = !playing && platform.now() - lastEnd >= cloneGuardSeconds
        if silent || (playing && h.band != band) { detected(.clone, intruder: own.short, avoid: h.band) }
    }

    private func isOwn(_ h: Heard) -> Bool {
        h.kind == .payload ? h.text == text : h.fingerprint == own.fingerprint
    }

    private func setState(_ next: SenderState) {
        if state == next { return }
        state = next
        options.onState?(next)
    }

    private func beginSending() {
        let o = options
        let channels = o.channels?.isEmpty == false ? o.channels! : Adapt.defaultChannels
        ambient = o.ambient ? readAmbient(noise, channels: channels) : nil
        plan = BySound.plan(band: o.band, channels: channels, profile: o.profile, calibration: store?.load(), ambient: ambient)
        if let cal = plan.calibration { store?.save(Calibration(band: cal.band, profile: cal.profile, uses: cal.uses + 1)) }
        if ambient?.loud == true && o.level == nil { level = 1 }
        let robustStart = plan.startRobust != nil || o.profile == .robust
        ladder = RobustLadder(auto: o.profile == nil, robust: robustStart, reason: plan.startRobust == .noise ? .noise : nil)
        o.onAmbient?(ambient, plan.route)
        firstAt = platform.now()
        s.after(o.timeoutSeconds) { [weak self] in
            guard let self = self, self.state == .sending || self.state == .window else { return }
            self.fail(.timeout, "No ack arrived")
            self.end()
        }
        playRepetition()
    }

    /// Next repetition of the ladder: `every` repetitions per channel, then the next channel of the route, cycling.
    private func next() -> (band: Band, profile: Profile, set: Int) {
        let route = plan.route
        if onChannel >= plan.every && route.count > 1 {
            onChannel = 0
            hop = (hop + 1) % route.count
        }
        onChannel += 1
        let band = route[hop]
        return (band, ladder.next(band), mode == .reference ? sent % 2 : 0)
    }

    private func render(_ band: Band, _ profile: Profile, _ set: Int) -> [Float] {
        let key = "\(band)/\(profile)/\(set)/\(level)"
        if let a = audio[key] { return a }
        // A text that fits the normal profile may not fit the robust one (twice the airtime): the normal profile is played then.
        let a = (try? renderRepetition(text: text, mode: mode, band: band, rate: platform.sampleRate, level: level, set: set, profile: profile))
            ?? (try? renderRepetition(text: text, mode: mode, band: band, rate: platform.sampleRate, level: level, set: set, profile: .normal)) ?? []
        audio[key] = a
        return a
    }

    private func playRepetition() {
        let r = next()
        band = r.band
        profile = r.profile
        played.append(r.band)
        options.onRepetition?(sent, r.band, r.profile)
        sent += 1
        setState(.sending)
        playing = true
        stopPayload = s.play(render(r.band, r.profile, r.set)) { [weak self] in
            guard let self = self else { return }
            self.playing = false
            self.stopPayload = nil
            self.lastEnd = self.platform.now()
            self.setState(.window)
            self.s.after(listenWindowSeconds) { [weak self] in
                if let self = self, self.state == .window { self.playRepetition() }
            }
        }
    }

    private func silence() {
        stopPayload?()
        stopPayload = nil
        playing = false
    }

    private func acked(_ ack: AckFrame) {
        silence()
        pending = false
        let heard = ack.heard ?? ReadAt(band: band, profile: profile)
        let calibration = learn(ack.heard)
        delivered.succeed(Delivery(band: heard.band, profile: heard.profile, stated: ack.heard != nil, seconds: platform.now() - firstAt,
                                    repetitions: sent, calibration: calibration))
        setState(.receipt)
        let receipt = Frames.ack(AckFrame(receipt: true, id: own.ack, heard: ack.heard))
        let longest = longestRepetitionSeconds(text: mode == .payload ? text : nil)
        s.play(loopFrames([receipt], seconds: receiptSeconds(repetition: longest), rate: platform.sampleRate, level: level)) { [weak self] in self?.end() }
    }

    /// Calibration (section 9.4): remember where the receiver read us, unless the band was fixed, the ack did not say, it was audible,
    /// the send went all the way round the route first (lapped), or the ambient moved us off a calibrated band that was noisy here.
    /// A robust profile reached by the robust ladder belongs to this place, not to the speaker: the normal profile is saved.
    private func learn(_ heard: ReadAt?) -> Calibration? {
        guard let store = store, options.band == nil, let heard = heard, heard.band != .audible else { return nil }
        if lapped(played, routeLength: plan.route.count) { return nil }
        if let cal = plan.calibration, heard.band != cal.band, let a = ambient, a.loud || a.noisy.contains(cal.band) { return nil }
        let profile: Profile = heard.profile == .robust && ladder.reason != nil ? .normal : heard.profile
        let next = Calibration(band: heard.band, profile: profile, uses: 0)
        store.save(next)
        return next
    }

    /// Pre-listen found something: nothing was sent, so there is nothing to warn about.
    private func refuse(_ code: SessionErrorCode, _ message: String) {
        fail(code, message)
        end()
    }

    /// Another emitter while sending: stop, and (emit `.auto`) loop a risk frame so the receiver and others stop too.
    private func detected(_ reason: RiskReason, intruder: String, avoid: Band) {
        silence()
        let code: SessionErrorCode = reason == .clone ? .clonedTransmission : .otherEmitter
        let message = reason == .clone ? "Someone else is playing this data" : "Another transmission was heard while sending"
        if security.riskSignal.emit != .auto {
            fail(code, message)
            return end()
        }
        let alert = emitRisk(RiskFrame(reason: reason, source: .sender, hops: 0, own: own.short, intruder: intruder), avoid: avoid, relay: false)
        fail(code, message, alert)
    }

    private func heardRisk(_ r: RiskFrame, _ band: Band) {
        let cfg = security.riskSignal
        let key = r.key, now = platform.now()
        if !cfg.listen || state == .alerting || seen.contains(key) || memory.isOwn(key, now: now, window: Double(cfg.quarantineMs) / 1000) { return }
        seen.insert(key)
        let alert = Alert(.heard, r, bands: [band], matchesMine: r.own == own.short, relay: false)
        options.onAlert?(alert)
        quarantine(r)
        if !cfg.honor { return }
        silence()
        // Any honored risk frame stops this sender, whoever sent it and whatever it reports. It is relayed once, one hop further.
        if r.hops < Frames.riskMaxHops {
            var relayed = r
            relayed.hops += 1
            emitRisk(relayed, avoid: band, relay: true)
        } else {
            end()
        }
        fail(.riskSignal, "A risk signal was heard", alert)
    }

    @discardableResult
    private func emitRisk(_ r: RiskFrame, avoid: Band, relay: Bool) -> Alert {
        let cfg = security.riskSignal
        s.clear() // stops receipts and timers; the payload was already silenced
        memory.note(r.key, at: platform.now())
        let (bands, audio) = riskAudio(r, work: band, avoid: avoid, seconds: Double(cfg.detectorDurationMs) / 1000, rate: platform.sampleRate, level: level)
        let alert = Alert(.emitted, r, bands: bands, matchesMine: true, relay: relay)
        options.onAlert?(alert)
        if !relay { quarantine(r) }
        setState(.alerting)
        s.play(audio) { [weak self] in self?.end() }
        return alert
    }

    private func quarantine(_ r: RiskFrame) {
        let ms = security.riskSignal.quarantineMs
        if ms > 0 { options.onQuarantine?(QuarantineNotice(durationMs: ms, reason: r.reason, source: r.source)) }
    }

    private func fail(_ code: SessionErrorCode, _ message: String, _ alert: Alert? = nil) {
        if !pending { return }
        pending = false
        delivered.fail(SessionError(code, message, alert: alert))
    }

    private func end() {
        s.clear()
        playing = false
        setState(.done)
        finished.succeed(())
    }
}
