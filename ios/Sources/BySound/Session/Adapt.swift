// Automatic adaptation of the sender (docs/SESSION.md section 9): channel ladder, ambient check, robust ladder and calibration memory.
// Pure functions and small classes: the sender state machine calls them. Port of `adapt.ts`.
import Foundation

public enum Adapt {
    /// The default ladder, from the least audible band down. `mid`, the most audible, is last.
    public static let defaultChannels: [Band] = [.top, .high, .upper, .mid]
    /// Repetitions on a channel before hopping to the next one (doubled when starting from a calibration).
    public static let hopRepetitions = 3
    public static let calibratedHopFactor = 2
    /// Repetitions in the normal profile without an ack before the rest of the ladder goes robust.
    public static let robustAfterRepetitions = 6
    /// A channel this much noisier than the quietest one is skipped.
    public static let noisyChannelDb = 10.0
    /// Quietest channel at or above this: a loud place (dB per tone relative to a full-scale grid tone; not calibrated in the field).
    public static let loudPlaceDb = -63.0
    /// Noise of the starting channel at or above this: start robust (same scale; not calibrated in the field).
    public static let robustNoiseDb = -68.0
    /// Sends that may start from a calibration without a read on its channel before it is forgotten.
    public static let calibrationUses = 10

    /// From `start` down the chain, then the channels above it, nearest first: the most audible ones come last.
    public static func channelOrder(_ start: Band, _ chain: [Band]) -> [Band] {
        guard let at = chain.firstIndex(of: start) else { return [start] }
        return Array(chain[at...]) + Array(chain[..<at].reversed())
    }
}

public struct Ambient: Equatable {
    /// Median of the noise reports heard during the pre-listen, per band.
    public var noise: BandNoise
    /// Channels skipped: `noisyChannelDb` above the quietest.
    public var noisy: [Band]
    /// Even the quietest channel is at `loudPlaceDb` or more.
    public var loud: Bool
    public init(noise: BandNoise, noisy: [Band], loud: Bool) { self.noise = noise; self.noisy = noisy; self.loud = loud }
}

/// Reads the noise reports of the pre-listen. `nil` without reports (no microphone).
public func readAmbient(_ reports: [BandNoise], channels: [Band]) -> Ambient? {
    if reports.isEmpty { return nil }
    var noise: BandNoise = [:]
    for b in channels {
        let v = reports.compactMap { $0[b] }.sorted()
        if !v.isEmpty { noise[b] = v[v.count >> 1] }
    }
    let levels = channels.filter { noise[$0] != nil }
    if levels.isEmpty { return nil }
    let quietest = levels.map { noise[$0]! }.min()!
    return Ambient(noise: noise, noisy: levels.filter { noise[$0]! > quietest + Adapt.noisyChannelDb }, loud: quietest >= Adapt.loudPlaceDb)
}

public struct Calibration: Equatable, Codable, Sendable {
    /// Where (and how) the last receiver read this device.
    public var band: Band
    public var profile: Profile
    /// Sends that started from it since that read.
    public var uses: Int
    public init(band: Band, profile: Profile, uses: Int) { self.band = band; self.profile = profile; self.uses = uses }
}
extension Band: Codable {}
extension Profile: Codable {}

/// Where calibrations live.
public protocol CalibrationStore: AnyObject {
    func load() -> Calibration?
    func save(_ c: Calibration?)
}

/// In memory.
public final class MemoryCalibrationStore: CalibrationStore {
    private var c: Calibration?
    public init(_ initial: Calibration? = nil) { c = initial }
    public func load() -> Calibration? { c }
    public func save(_ next: Calibration?) { c = next }
    /// Shared by the sessions that do not pass their own store.
    public static let shared = MemoryCalibrationStore()
}

/// `UserDefaults` (key `bysound.calibration` by default): the speaker of a device does not change between launches. A stored value
/// that does not validate is ignored.
public final class UserDefaultsCalibrationStore: CalibrationStore {
    private let defaults: UserDefaults
    private let key: String
    private let fallback = MemoryCalibrationStore()
    public init(defaults: UserDefaults = .standard, key: String = "bysound.calibration") { self.defaults = defaults; self.key = key }
    public func load() -> Calibration? {
        if let data = defaults.data(forKey: key), let c = try? JSONDecoder().decode(Calibration.self, from: data), c.uses >= 0 { return c }
        return fallback.load()
    }
    public func save(_ c: Calibration?) {
        fallback.save(c)
        if let c = c, let data = try? JSONEncoder().encode(c) { defaults.set(data, forKey: key) } else { defaults.removeObject(forKey: key) }
    }
}

/// Robust after `robustAfterRepetitions` normal repetitions without an ack (only on bands that have it), or from the start.
final class RobustLadder {
    enum Reason { case noise, repetitions }
    var reason: Reason?
    private var normal = 0
    private let auto: Bool
    private var robust: Bool
    init(auto: Bool, robust: Bool, reason: Reason? = nil) { self.auto = auto; self.robust = robust; self.reason = reason }
    func next(_ band: Band) -> Profile {
        if !robust && auto && normal >= Adapt.robustAfterRepetitions {
            robust = true
            reason = .repetitions
        }
        let p: Profile = robust && band.hasRobust ? .robust : .normal
        if p == .normal { normal += 1 }
        return p
    }
}

public struct Plan: Equatable {
    public enum StartRobust: Equatable { case calibration, noise }
    /// Channels in the order they are played, cycling.
    public var route: [Band]
    /// Repetitions per channel (`Int.max`: never hop).
    public var every: Int
    /// The calibration it starts from, if any.
    public var calibration: Calibration?
    public var startRobust: StartRobust?
}

/// Where to play: a fixed `band` is played as is. Otherwise the ladder starts at a valid calibration (fewer than `calibrationUses`
/// uses, its band in the chain) or at the top of the chain, skips the noisy channels (the calibrated one stays: it is where this
/// speaker works) and, without an explicit profile, starts robust if the calibration was robust or the starting channel is very noisy.
public func plan(band: Band? = nil, channels: [Band]? = nil, profile: Profile? = nil, calibration: Calibration?, ambient a: Ambient?) -> Plan {
    if let band = band { return Plan(route: [band], every: Int.max, calibration: nil, startRobust: nil) }
    let chain = (channels?.isEmpty ?? true) ? Adapt.defaultChannels : channels!
    let cal = calibration.flatMap { $0.uses < Adapt.calibrationUses && chain.contains($0.band) ? $0 : nil }
    var route = Adapt.channelOrder(cal?.band ?? chain[0], chain)
    if let a = a {
        let kept = route.filter { !a.noisy.contains($0) || $0 == cal?.band }
        route = kept.isEmpty ? [chain.sorted { (a.noise[$0] ?? 0) < (a.noise[$1] ?? 0) }[0]] : kept
    }
    var start: Plan.StartRobust?
    if profile == nil {
        if cal?.profile == .robust { start = .calibration }
        else if let a = a, (a.noise[route[0]] ?? -Double.infinity) >= Adapt.robustNoiseDb { start = .noise }
    }
    return Plan(route: route, every: Adapt.hopRepetitions * (cal != nil ? Adapt.calibratedHopFactor : 1), calibration: cal, startRobust: start)
}

/// The send went through every channel of its route and came back to one: a late read says nothing about the speaker.
public func lapped(_ played: [Band], routeLength: Int) -> Bool {
    if routeLength < 2 { return false }
    var changes = 0
    var last: Band?
    for b in played where b != last { changes += 1; last = b }
    return changes > routeLength
}

/// The longest repetition (frame + gap) this content can have on the default ladder: top in the robust profile. The receiver's ack
/// run and the sender's receipts are timed with it, so they cover a listen window even after a hop or a switch to robust.
func longestRepetitionSeconds(text: String?) -> Double {
    let ch = Band.top.channel(.robust)
    if let text = text { return Double(Codec.transmissionMicros(ch, packed: TextPacking.pack(text).count)) / 1e6 }
    let f = Reference.frame(fingerprint: [UInt8](repeating: 0, count: Reference.bytes), band: .top, set: 0, profile: .robust)
    return f.frameSeconds + Protocol.gapSeconds
}
