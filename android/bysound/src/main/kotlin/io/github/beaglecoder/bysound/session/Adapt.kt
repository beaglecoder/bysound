package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Codec
import io.github.beaglecoder.bysound.modem.Modem
import io.github.beaglecoder.bysound.modem.Profile

// Automatic adaptation of the sender (docs/SESSION.md section 9): channel ladder, ambient check, robust ladder, calibration memory.

/** The default ladder, from the least audible band down. mid, the most audible, is last. */
val DEFAULT_CHANNELS: List<Band> = listOf(Band.TOP, Band.HIGH, Band.UPPER, Band.MID)
/** Repetitions on a channel before hopping to the next one (doubled when starting from a calibration). */
const val HOP_REPETITIONS = 3
const val CALIBRATED_HOP_FACTOR = 2
/** Repetitions in the normal profile without an ack before the rest of the ladder goes robust. */
const val ROBUST_AFTER_REPETITIONS = 6
/** A channel this much noisier than the quietest one is skipped. */
const val NOISY_CHANNEL_DB = 10.0
/** Quietest channel at or above this: a loud place (dB per tone relative to a full-scale grid tone; not calibrated in the field). */
const val LOUD_PLACE_DB = -63.0
/** Noise of the starting channel at or above this: start robust (same scale; not calibrated in the field). */
const val ROBUST_NOISE_DB = -68.0
/** Sends that may start from a calibration without a read on its channel before it is forgotten. */
const val CALIBRATION_USES = 10

/** From [start] down the chain, then the channels above it, nearest first: the most audible ones come last. */
fun channelOrder(start: Band, chain: List<Band>): List<Band> {
    val at = chain.indexOf(start)
    if (at < 0) return listOf(start)
    return chain.subList(at, chain.size) + chain.subList(0, at).reversed()
}

/** Noise per band: dB relative to a full-scale tone of the grid. */
typealias BandNoise = Map<Band, Double>

class Ambient(
    /** Median of the noise reports heard during the pre-listen, per band. */
    val noise: BandNoise,
    /** Channels skipped: [NOISY_CHANNEL_DB] above the quietest. */
    val noisy: List<Band>,
    /** Even the quietest channel is at [LOUD_PLACE_DB] or more. */
    val loud: Boolean,
)

/** Reads the noise reports of the pre-listen. null without reports (no microphone). */
fun readAmbient(reports: List<BandNoise>, channels: List<Band>): Ambient? {
    if (reports.isEmpty()) return null
    val noise = LinkedHashMap<Band, Double>()
    for (b in channels) {
        val v = reports.mapNotNull { it[b] }.sorted()
        if (v.isNotEmpty()) noise[b] = v[v.size shr 1]
    }
    val levels = channels.filter { noise.containsKey(it) }
    if (levels.isEmpty()) return null
    val quietest = levels.minOf { noise.getValue(it) }
    return Ambient(noise, levels.filter { noise.getValue(it) > quietest + NOISY_CHANNEL_DB }, quietest >= LOUD_PLACE_DB)
}

/** Where (and how) the last receiver read this device, and how many sends have started from it since. */
data class Calibration(val band: Band, val profile: Profile, val uses: Int)

/** Where calibrations live. In memory by default; Android apps use SharedPreferences ([io.github.beaglecoder.bysound.audio.SharedPreferencesCalibrationStore]). */
interface CalibrationStore {
    fun load(): Calibration?
    fun save(c: Calibration?)
}

class MemoryCalibrationStore(initial: Calibration? = null) : CalibrationStore {
    private var c = initial
    override fun load() = c
    override fun save(c: Calibration?) { this.c = c }
}

/** Text form of a calibration for stores that keep strings (`band:profile:uses`); null if it is not valid. */
fun Calibration.encode() = "${band.id}:${profile.name.lowercase()}:$uses"
fun parseCalibration(raw: String?): Calibration? {
    val p = raw?.split(":") ?: return null
    if (p.size != 3) return null
    val band = Band.entries.firstOrNull { it.id == p[0] && it != Band.AUDIBLE } ?: return null
    val uses = p[2].toIntOrNull()?.takeIf { it >= 0 } ?: return null
    return Calibration(band, if (p[1] == "robust") Profile.ROBUST else Profile.NORMAL, uses)
}

val sharedCalibrationStore: CalibrationStore = MemoryCalibrationStore()

/** Robust after [ROBUST_AFTER_REPETITIONS] normal repetitions without an ack (only on bands that have it), or from the start. */
class RobustLadder(private val auto: Boolean, private var robust: Boolean, var reason: String? = null) {
    private var normal = 0
    fun next(band: Band): Profile {
        if (!robust && auto && normal >= ROBUST_AFTER_REPETITIONS) { robust = true; reason = "repetitions" }
        val p = if (robust && band.hasRobust) Profile.ROBUST else Profile.NORMAL
        if (p == Profile.NORMAL) normal++
        return p
    }
}

enum class StartRobust { CALIBRATION, NOISE }

class Plan(
    /** Channels in the order they are played, cycling. */
    val route: List<Band>,
    /** Repetitions per channel. */
    val every: Int,
    /** The calibration it starts from, if any. */
    val calibration: Calibration?,
    val startRobust: StartRobust?,
)

/**
 * Where to play: a fixed [band] is played as is. Otherwise the ladder starts at a valid calibration (fewer than [CALIBRATION_USES]
 * uses, its band in the chain) or at the top of the chain, skips the noisy channels (the calibrated one stays) and, without an
 * explicit [profile], starts robust if the calibration was robust or the starting channel is very noisy.
 */
fun plan(band: Band? = null, channels: List<Band>? = null, profile: Profile? = null, calibration: Calibration?, ambient: Ambient?): Plan {
    if (band != null) return Plan(listOf(band), Int.MAX_VALUE, null, null)
    val chain = if (channels.isNullOrEmpty()) DEFAULT_CHANNELS else channels
    val cal = calibration?.takeIf { it.uses < CALIBRATION_USES && chain.contains(it.band) }
    var route = channelOrder(cal?.band ?: chain[0], chain)
    val a = ambient
    if (a != null) {
        val kept = route.filter { !a.noisy.contains(it) || it == cal?.band }
        route = kept.ifEmpty { listOf(chain.sortedBy { a.noise[it] ?: 0.0 }[0]) }
    }
    val startRobust = when {
        profile != null -> null
        cal?.profile == Profile.ROBUST -> StartRobust.CALIBRATION
        a != null && (a.noise[route[0]] ?: Double.NEGATIVE_INFINITY) >= ROBUST_NOISE_DB -> StartRobust.NOISE
        else -> null
    }
    return Plan(route, HOP_REPETITIONS * (if (cal != null) CALIBRATED_HOP_FACTOR else 1), cal, startRobust)
}

/** The send went through every channel of its route and came back to one: a late read says nothing about the speaker. */
fun lapped(played: List<Band>, routeLength: Int): Boolean {
    if (routeLength < 2) return false
    var changes = 0
    var last: Band? = null
    for (b in played) if (b != last) { changes++; last = b }
    return changes > routeLength
}

/**
 * The longest repetition (frame + gap) this content can have on the default ladder: top in the robust profile. The receiver's ack
 * run and the sender's receipts are timed with it, so they cover a listen window even after a hop or a switch to robust.
 */
fun longestRepetitionSeconds(text: String?): Double {
    val f = if (text != null) Codec.payloadFrame(text, Band.TOP, Profile.ROBUST) else Codec.referenceFrame(IntArray(12), Band.TOP, 0, Profile.ROBUST)
    return f.frameSeconds + Modem.GAP_SECONDS
}
