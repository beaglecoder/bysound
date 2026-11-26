package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Profile
import java.util.concurrent.CompletableFuture

data class SenderOptions(
    /** A fixed band: no ladder, no calibration. */
    val band: Band? = null,
    /** The ladder, from the least audible band down. Default top, high, upper, mid. */
    val channels: List<Band>? = null,
    /** A fixed profile for the data frames. Without it the sender goes robust by itself (section 9.3). Acks and risk frames are always normal. */
    val profile: Profile? = null,
    /** PAYLOAD sends the text; REFERENCE sends only its 12-byte fingerprint (the receiver resolves it locally). */
    val mode: Mode = Mode.PAYLOAD,
    /** Output level 0..1 of everything this side plays. Default 1. */
    val level: Float? = null,
    /** Gives up this long after the first frame if no ack arrived. */
    val timeoutSeconds: Double = 60.0,
    /** Measure the noise of each channel in the pre-listen and skip the noisy ones. */
    val ambient: Boolean = true,
    /** Where the calibration is kept; null: none. Default: in memory, shared by the process. */
    val calibration: CalibrationStore? = sharedCalibrationStore,
    /** Default maximum. */
    val securityLevel: SecurityLevel = DEFAULT_SECURITY_LEVEL,
    /** Explicit values that win over the level. */
    val security: SecurityOverrides = SecurityOverrides(),
    /** Risk incidents this device already emitted. Default: one shared by every session of the process. */
    val memory: RiskMemory = sharedRiskMemory,
    val onAlert: ((Alert) -> Unit)? = null,
    val onQuarantine: ((QuarantineNotice) -> Unit)? = null,
    val onState: ((SenderState) -> Unit)? = null,
    /** Each repetition as it starts. */
    val onRepetition: ((Repetition) -> Unit)? = null,
    /** What the pre-listen measured (null without noise reports) and the route that follows. */
    val onAmbient: ((Ambient?, List<Band>) -> Unit)? = null,
)

class Repetition(val index: Int, val band: Band, val profile: Profile) {
    override fun equals(other: Any?) = other is Repetition && index == other.index && band == other.band && profile == other.profile
    override fun hashCode() = (index * 31 + band.hashCode()) * 31 + profile.hashCode()
    override fun toString() = "Repetition($index, ${band.id}, $profile)"
}

enum class SenderState { PRE_LISTEN, SENDING, WINDOW, RECEIPT, ALERTING, DONE }

class Delivered(
    /** Where the receiver read it, from the ack; without that in the ack, the band of the last repetition. */
    val band: Band,
    val profile: Profile,
    /** The ack said where it was read. */
    val stated: Boolean,
    /** From the first frame to the ack. */
    val seconds: Double,
    val repetitions: Int,
    /** The calibration saved for next time (null: nothing learned). */
    val calibration: Calibration?,
)

/**
 * Session sender (docs/SESSION.md sections 5 and 9): pre-listen (other emitters + ambient), repetitions with listen windows over an
 * adaptive channel ladder, ack -> receipts, other emitters -> risk frame, heard risk frames -> stop and relay. A plain state machine
 * over [Platform] and [AirEvent]s: not thread-safe, call it from one thread.
 */
class SenderSession(private val text: String, private val platform: Platform, private val options: SenderOptions = SenderOptions()) {
    @Volatile var state = SenderState.PRE_LISTEN
        private set
    /** Completes with the ack; fails with a [SessionError] otherwise (section 8). */
    val delivered = CompletableFuture<Delivered>()
    /** Completes when the session is over and plays nothing more (after receipts or the risk frame). */
    val finished = CompletableFuture<Unit>()
    val security: Security = resolveSecurity(options.securityLevel, options.security)

    private val mode = options.mode
    private var level = options.level ?: 1f
    private val memory = options.memory
    private val store = options.calibration
    private val ownShort = shortId(text)
    private val ownAck = ackId(text)
    private val ownFingerprint = fingerprint(text)
    private val audio = HashMap<String, FloatArray>()
    private val s = Scheduler(platform)
    private var pending = true
    private var plan = Plan(listOf(DEFAULT_CHANNELS[0]), Int.MAX_VALUE, null, null)
    private var ladder = RobustLadder(false, false)
    private var ambient: Ambient? = null
    private val noise = ArrayList<BandNoise>()
    private var hop = 0
    private var onChannel = 0
    private var sent = 0
    private val played = ArrayList<Band>()
    private var band: Band = options.band ?: (options.channels ?: DEFAULT_CHANNELS)[0]
    private var profile = Profile.NORMAL
    private var playing = false
    private var lastEnd = Double.NEGATIVE_INFINITY
    private var firstAt = 0.0
    private var stopPayload: (() -> Unit)? = null
    private val seen = HashSet<String>()
    /** Pre-listen heard a frame start or a clash. */
    private var busy = false

    init { delivered.exceptionally { null } /* the app may only look at `finished` */ }

    fun start() {
        val sec = security
        val detect = sec.detectMultipleEmitters
        if (sec.listenBeforeSendingSeconds > 0 && (detect || options.ambient)) {
            setState(SenderState.PRE_LISTEN)
            // The jitter only matters for detection: two senders starting together must not end their pre-listen together.
            s.after(sec.listenBeforeSendingSeconds + (if (detect) platform.random() * sec.scanJitterSeconds else 0.0)) {
                if (busy) refuse(SessionErrorCode.OTHER_TRANSMISSION, "Another transmission is already sounding") else beginSending()
            }
        } else beginSending()
    }

    /** Stops everything at once, the risk frame included. Before the ack, [delivered] fails with STOPPED. */
    fun stop() {
        fail(SessionErrorCode.STOPPED, "Stopped by the app")
        end()
    }

    fun hear(e: AirEvent) {
        if (state == SenderState.DONE) return
        when (e) {
            is AirEvent.Noise -> { if (state == SenderState.PRE_LISTEN) noise.add(e.noise); return }
            is AirEvent.Risk -> return heardRisk(e.risk, e.band)
            is AirEvent.Ack -> {
                if ((state == SenderState.SENDING || state == SenderState.WINDOW) && !e.ack.receipt && e.ack.id == ownAck) acked(e.ack)
                return
            }
            else -> {}
        }
        val sec = security
        if (!sec.detectMultipleEmitters) return
        if (state == SenderState.PRE_LISTEN) {
            // A decoded frame refuses at once; a frame start or a clash refuses at the end of the pre-listen, unless the frame is
            // decoded by then (so a replay of our own data is reported as a clone). Acks and risk frames are handled above.
            if (e !is AirEvent.Frame) busy = true
            else if (isOwn(e.heard)) refuse(SessionErrorCode.CLONED_TRANSMISSION, "Someone else is already playing this data")
            else refuse(SessionErrorCode.OTHER_TRANSMISSION, "Another transmission is already sounding")
            return
        }
        if ((state != SenderState.SENDING && state != SenderState.WINDOW) || !sec.listenWhileSending || e !is AirEvent.Frame) return
        val h = e.heard
        if (!isOwn(h)) return detected(RiskReason.OTHER, shortOf(h), h.band)
        // Our own data while we have been silent for a while, or on a band we are not playing: someone else is playing it.
        val silent = !playing && platform.now() - lastEnd >= CLONE_GUARD_SECONDS
        if (silent || (playing && h.band != band)) detected(RiskReason.CLONE, ownShort, h.band)
    }

    private fun isOwn(h: HeardFrame) = when (h) {
        is HeardFrame.Payload -> h.text == text
        is HeardFrame.Reference -> h.fingerprint == ownFingerprint
    }

    private fun setState(next: SenderState) {
        if (state == next) return
        state = next
        options.onState?.invoke(next)
    }

    private fun beginSending() {
        val o = options
        val channels = if (o.channels.isNullOrEmpty()) DEFAULT_CHANNELS else o.channels
        ambient = if (o.ambient) readAmbient(noise, channels) else null
        plan = plan(o.band, channels, o.profile, store?.load(), ambient)
        plan.calibration?.let { store?.save(it.copy(uses = it.uses + 1)) }
        if (ambient?.loud == true && o.level == null) level = 1f
        val robustStart = plan.startRobust != null || o.profile == Profile.ROBUST
        ladder = RobustLadder(o.profile == null, robustStart, if (plan.startRobust == StartRobust.NOISE) "noise" else null)
        o.onAmbient?.invoke(ambient, plan.route)
        firstAt = platform.now()
        s.after(o.timeoutSeconds) {
            if (state != SenderState.SENDING && state != SenderState.WINDOW) return@after
            fail(SessionErrorCode.TIMEOUT, "No ack arrived")
            end()
        }
        playRepetition()
    }

    private class Next(val band: Band, val profile: Profile, val set: Int)

    /** Next repetition of the ladder: `every` repetitions per channel, then the next channel of the route, cycling. */
    private fun next(): Next {
        val route = plan.route
        if (onChannel >= plan.every && route.size > 1) {
            onChannel = 0
            hop = (hop + 1) % route.size
        }
        onChannel++
        val b = route[hop]
        return Next(b, ladder.next(b), if (mode == Mode.REFERENCE) sent % 2 else 0)
    }

    private fun render(band: Band, profile: Profile, set: Int): FloatArray =
        audio.getOrPut("${band.id}/$profile/$set/$level") { renderRepetition(text, mode, band, platform.sampleRate, level, set, profile) }

    private fun playRepetition() {
        val r = next()
        band = r.band
        profile = r.profile
        played.add(r.band)
        options.onRepetition?.invoke(Repetition(sent, r.band, r.profile))
        sent++
        setState(SenderState.SENDING)
        playing = true
        stopPayload = s.play(render(r.band, r.profile, r.set)) {
            playing = false
            stopPayload = null
            lastEnd = platform.now()
            setState(SenderState.WINDOW)
            s.after(LISTEN_WINDOW_SECONDS) { if (state == SenderState.WINDOW) playRepetition() }
        }
    }

    private fun silence() {
        stopPayload?.invoke()
        stopPayload = null
        playing = false
    }

    private fun acked(ack: AckFrame) {
        silence()
        pending = false
        val heard = ack.heard ?: AckHeard(band, profile)
        val calibration = learn(ack.heard)
        delivered.complete(Delivered(heard.band, heard.profile, ack.heard != null, platform.now() - firstAt, sent, calibration))
        setState(SenderState.RECEIPT)
        val receipt = ackFrame(AckFrame(true, ownAck, ack.heard))
        val longest = longestRepetitionSeconds(if (mode == Mode.PAYLOAD) text else null)
        s.play(loopFrames(listOf(receipt), receiptSeconds(longest), platform.sampleRate, level)) { end() }
    }

    /**
     * Calibration (section 9.4): remember where the receiver read us, unless the band was fixed, the ack did not say, it was audible,
     * the send went all the way round the route first (lapped), or the ambient moved us off a calibrated band that was noisy here.
     * A robust profile reached by the robust ladder belongs to this place, not to the speaker: the normal profile is saved.
     */
    private fun learn(heard: AckHeard?): Calibration? {
        val store = store
        if (store == null || options.band != null || heard == null || heard.band == Band.AUDIBLE) return null
        if (lapped(played, plan.route.size)) return null
        val cal = plan.calibration
        val a = ambient
        if (cal != null && heard.band != cal.band && a != null && (a.loud || a.noisy.contains(cal.band))) return null
        val p = if (heard.profile == Profile.ROBUST && ladder.reason != null) Profile.NORMAL else heard.profile
        val next = Calibration(heard.band, p, 0)
        store.save(next)
        return next
    }

    /** Pre-listen found something: nothing was sent, so there is nothing to warn about. */
    private fun refuse(code: SessionErrorCode, message: String) {
        fail(code, message)
        end()
    }

    /** Another emitter while sending: stop, and (emit AUTO) loop a risk frame so the receiver and others stop too. */
    private fun detected(reason: RiskReason, intruder: String, avoid: Band) {
        silence()
        val code = if (reason == RiskReason.CLONE) SessionErrorCode.CLONED_TRANSMISSION else SessionErrorCode.OTHER_EMITTER
        val message = if (reason == RiskReason.CLONE) "Someone else is playing this data" else "Another transmission was heard while sending"
        if (security.riskSignal.emit != Emit.AUTO) {
            fail(code, message)
            return end()
        }
        val alert = emitRisk(RiskFrame(reason, Role.SENDER, 0, ownShort, intruder), avoid, false)
        fail(code, message, alert)
    }

    private fun heardRisk(r: RiskFrame, band: Band) {
        val cfg = security.riskSignal
        val key = riskKey(r)
        val now = platform.now()
        if (!cfg.listen || state == SenderState.ALERTING || seen.contains(key) || memory.isOwn(key, now, cfg.quarantineMs / 1000.0)) return
        seen.add(key)
        val alert = Alert(Alert.Direction.HEARD, r.reason, r.source, r.hops, r.own, r.intruder, listOf(band), r.own == ownShort, false)
        options.onAlert?.invoke(alert)
        quarantine(r)
        if (!cfg.honor) return
        silence()
        // Any honored risk frame stops this sender, whoever sent it and whatever it reports. It is relayed once, one hop further.
        if (r.hops < RISK_MAX_HOPS) emitRisk(r.copy(hops = r.hops + 1), band, true) else end()
        fail(SessionErrorCode.RISK_SIGNAL, "A risk signal was heard", alert)
    }

    private fun emitRisk(r: RiskFrame, avoid: Band, relay: Boolean): Alert {
        val cfg = security.riskSignal
        s.clear() // stops receipts and timers; the payload was already silenced
        memory.note(riskKey(r), platform.now())
        val a = riskAudio(r, band, avoid, cfg.detectorDurationMs / 1000.0, platform.sampleRate, level)
        val alert = Alert(Alert.Direction.EMITTED, r.reason, r.source, r.hops, r.own, r.intruder, a.bands, true, relay)
        options.onAlert?.invoke(alert)
        if (!relay) quarantine(r)
        setState(SenderState.ALERTING)
        s.play(a.audio) { end() }
        return alert
    }

    private fun quarantine(r: RiskFrame) {
        val ms = security.riskSignal.quarantineMs
        if (ms > 0) options.onQuarantine?.invoke(QuarantineNotice(ms, r.reason, r.source))
    }

    private fun fail(code: SessionErrorCode, message: String, alert: Alert? = null) {
        if (!pending) return
        pending = false
        delivered.completeExceptionally(SessionError(code, message, alert))
    }

    private fun end() {
        s.clear()
        playing = false
        setState(SenderState.DONE)
        finished.complete(Unit)
    }
}

/** Short id (6 hex) of what a frame carries. */
internal fun shortOf(h: HeardFrame) = when (h) {
    is HeardFrame.Payload -> shortId(h.text)
    is HeardFrame.Reference -> h.fingerprint.substring(0, 6)
}
