package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage

/** Reference mode: returns the text whose fingerprint (24 hex) was heard, from data the app already has; null if unknown. */
fun interface Resolver {
    fun resolve(fingerprint: String): String?
}

/** The same, asynchronous. */
fun interface AsyncResolver {
    fun resolve(fingerprint: String): CompletionStage<String?>
}

/** A resolver over a local catalog: text by its fingerprint. */
fun catalogResolver(texts: Collection<String>): Resolver {
    val map = texts.associateBy { fingerprint(it) }
    return Resolver { map[it] }
}

data class ReceiverOptions(
    /** Fails with TIMEOUT if no frame was heard by then. */
    val timeoutSeconds: Double = 60.0,
    /** Reference mode: bysound checks that SHA-256(text) starts with the fingerprint. */
    val resolve: Resolver? = null,
    /** Reference mode with an asynchronous lookup (wins over [resolve]). */
    val resolveAsync: AsyncResolver? = null,
    /** Acknowledge by sound after delivery. */
    val ack: Boolean = true,
    /** Output level 0..1 of acks and risk frames. */
    val level: Float = 1f,
    val securityLevel: SecurityLevel = DEFAULT_SECURITY_LEVEL,
    val security: SecurityOverrides = SecurityOverrides(),
    val memory: RiskMemory = sharedRiskMemory,
    val onAlert: ((Alert) -> Unit)? = null,
    val onQuarantine: ((QuarantineNotice) -> Unit)? = null,
    /** The first frame was heard (delivery follows after the final listen). */
    val onHeard: ((Band) -> Unit)? = null,
    val onState: ((ReceiverState) -> Unit)? = null,
)

enum class ReceiverState { LISTENING, FINAL_LISTEN, DELIVERED, CLOSED, FAILED }

class Received(val text: String, val band: Band, val via: Mode) {
    override fun equals(other: Any?) = other is Received && text == other.text && band == other.band && via == other.via
    override fun hashCode() = (text.hashCode() * 31 + band.hashCode()) * 31 + via.hashCode()
    override fun toString() = "Received(\"$text\", ${band.id}, $via)"
}

enum class Watch { CLEAN, EXPIRED, OFF }

data class Confirmation(
    /** CLEAN: watched until now. EXPIRED: the watch hit confirmWatchSeconds first. OFF: no watch at this level. */
    val watch: Watch,
    val watchedSeconds: Double,
    /** From the end of the watch to the confirmed() call (EXPIRED only). */
    val uncoveredSeconds: Double,
)

/**
 * Session receiver (docs/SESSION.md section 6): first frame -> final listen -> delivery -> acks until the receipt, and the confirm
 * gate that keeps watching for other emitters until the app calls [confirmed]. A plain state machine over [Platform] and
 * [AirEvent]s: not thread-safe, call it from one thread.
 */
class ReceiverSession(private val platform: Platform, private val options: ReceiverOptions = ReceiverOptions()) {
    @Volatile var state = ReceiverState.LISTENING
        private set
    /** The data, after the final listen. Fails with a [SessionError] (MULTIPLE_TRANSMISSIONS, RISK_SIGNAL, TIMEOUT...). */
    val result = CompletableFuture<Received>()
    /** true when the sender's receipt was heard; false when acking ended without one (or acks are off). */
    val receipt = CompletableFuture<Boolean>()
    /** Completes when the session needs neither the microphone nor the speaker any more. */
    val finished = CompletableFuture<Unit>()
    val security: Security = resolveSecurity(options.securityLevel, options.security)

    private val s = Scheduler(platform)
    private val memory = options.memory
    private var first: HeardFrame? = null
    private var firstAt = 0.0
    private var finalEnd = 0.0
    private var cancelFinal: (() -> Unit)? = null
    private var text: String? = null
    private var resolved: CompletableFuture<Unit>? = null
    private var received: Received? = null
    private var conflict: SessionError? = null
    private var closed: String? = null
    private var confirmation: Confirmation? = null
    private var watching = false
    private var watchStart = 0.0
    private var watchEnd: Double? = null
    private var watchExpired = false
    private var acking = false
    private var alerting = false
    private val seen = HashSet<String>()
    private var stopAcks: (() -> Unit)? = null

    init { result.exceptionally { null }; }

    fun start() {
        s.after(options.timeoutSeconds) { if (state == ReceiverState.LISTENING) fail(SessionErrorCode.TIMEOUT, "No transmission was heard") }
    }

    /**
     * The confirm gate: call it before acting on the data. The future fails with a [SessionError] if another emitter or a risk frame
     * showed up while watching (MULTIPLE_TRANSMISSIONS, RISK_SIGNAL), if the session was cancelled (CANCELLED) or if the result failed.
     */
    fun confirmed(): CompletableFuture<Confirmation> {
        val out = CompletableFuture<Confirmation>()
        result.whenComplete { _, e ->
            try {
                if (e != null) throw e.sessionError() ?: e
                out.complete(confirmNow())
            } catch (x: SessionError) {
                out.completeExceptionally(x)
            }
        }
        return out
    }

    private fun confirmNow(): Confirmation {
        if (closed == "cancelled") throw SessionError(SessionErrorCode.CANCELLED, "The session was cancelled before confirming")
        conflict?.let { throw it }
        confirmation?.let { return it }
        val now = platform.now()
        val c = when {
            !security.confirmWatch -> Confirmation(Watch.OFF, 0.0, 0.0)
            watchExpired -> Confirmation(Watch.EXPIRED, watchEnd!! - watchStart, now - watchEnd!!)
            else -> Confirmation(Watch.CLEAN, (watchEnd ?: now) - watchStart, 0.0)
        }
        closed = "confirmed"
        confirmation = c
        stopWatch()
        setState(ReceiverState.CLOSED)
        maybeFinished()
        return c
    }

    /** The person left without confirming: stop listening and acking. Never throws. A risk frame already sounding finishes. */
    fun cancel() {
        if (closed != null) return
        closed = "cancelled"
        fail(SessionErrorCode.CANCELLED, "Cancelled by the app")
        stopWatch()
        endAcks(false)
        if (state == ReceiverState.DELIVERED) setState(ReceiverState.CLOSED)
        maybeFinished()
    }

    fun hear(e: AirEvent) {
        when (e) {
            is AirEvent.Frame -> heardFrame(e.heard)
            is AirEvent.Header -> heardHeader(e.band)
            is AirEvent.Clash -> heardClash()
            is AirEvent.Ack -> heardAck(e.ack.receipt, e.ack.id)
            is AirEvent.Risk -> heardRisk(e.risk, e.band)
            is AirEvent.Noise -> {}
        }
    }

    private fun setState(next: ReceiverState) {
        if (state == next) return
        state = next
        options.onState?.invoke(next)
    }

    private fun same(a: HeardFrame, b: HeardFrame) = when (a) {
        is HeardFrame.Payload -> b is HeardFrame.Payload && a.text == b.text
        is HeardFrame.Reference -> b is HeardFrame.Reference && a.fingerprint == b.fingerprint
    }

    private class Risk(val reason: RiskReason, val intruder: String, val avoid: Band?)

    private fun heardFrame(h: HeardFrame) {
        val sec = security
        if (state == ReceiverState.LISTENING) {
            first = h
            firstAt = platform.now()
            options.onHeard?.invoke(h.band)
            when (h) {
                is HeardFrame.Payload -> text = h.text
                is HeardFrame.Reference -> if (options.resolve == null && options.resolveAsync == null) return fail(SessionErrorCode.NO_RESOLVER, "A reference was heard and there is no resolver")
            }
            if (!sec.detectMultipleEmitters) {
                startResolve(h)
                return deliverWhenReady()
            }
            setState(ReceiverState.FINAL_LISTEN)
            finalEnd = firstAt + FINAL_LISTEN_SECONDS
            cancelFinal = s.after(FINAL_LISTEN_SECONDS) { deliverWhenReady() }
            startResolve(h)
            return
        }
        val f = first
        if (f == null || same(f, h)) return // failed before any frame, or the sender repeating
        if (state == ReceiverState.FINAL_LISTEN) return fail(SessionErrorCode.MULTIPLE_TRANSMISSIONS, "Two different transmissions were heard", Risk(RiskReason.OTHER, shortOf(h), h.band))
        if (state == ReceiverState.DELIVERED && watching) conflictWith(SessionErrorCode.MULTIPLE_TRANSMISSIONS, "Another transmission appeared before confirming", RiskReason.OTHER, shortOf(h), h.band)
    }

    /** A frame starting on another band during the final listen: wait for it, up to FINAL_LISTEN_MAX_SECONDS. */
    private fun heardHeader(band: Band) {
        if (state != ReceiverState.FINAL_LISTEN || band == first!!.band) return
        val end = minOf(maxOf(finalEnd, platform.now() + FINAL_EXTEND_SECONDS), firstAt + FINAL_LISTEN_MAX_SECONDS)
        if (end <= finalEnd) return
        finalEnd = end
        cancelFinal?.invoke()
        cancelFinal = s.after(end - platform.now()) { deliverWhenReady() }
    }

    private fun heardClash() {
        if (!security.detectMultipleEmitters || (state != ReceiverState.LISTENING && state != ReceiverState.FINAL_LISTEN)) return
        fail(SessionErrorCode.MULTIPLE_TRANSMISSIONS, "Two transmissions overlapped", Risk(RiskReason.OVERLAP, UNKNOWN_SHORT, first?.band))
    }

    private fun heardAck(isReceipt: Boolean, id: String) {
        val t = text
        if (isReceipt && acking && t != null && id == ackId(t)) endAcks(true)
    }

    private fun heardRisk(r: RiskFrame, band: Band) {
        val cfg = security.riskSignal
        val key = riskKey(r)
        val now = platform.now()
        if (!cfg.listen || seen.contains(key) || memory.isOwn(key, now, cfg.quarantineMs / 1000.0)) return
        val before = state == ReceiverState.LISTENING || state == ReceiverState.FINAL_LISTEN
        val watchingRisk = state == ReceiverState.DELIVERED && watching && cfg.listenUntil == ListenUntil.CONFIRM
        if (!before && !watchingRisk) return
        seen.add(key)
        val mine = first?.let { shortOf(it) }
        val alert = Alert(Alert.Direction.HEARD, r.reason, r.source, r.hops, r.own, r.intruder, listOf(band), r.own == mine, false)
        options.onAlert?.invoke(alert)
        quarantine(r)
        if (!cfg.honor) return
        // The receiver never relays: the sender that hears it does.
        if (before) fail(SessionErrorCode.RISK_SIGNAL, "A risk signal was heard", heard = alert)
        else conflictWith(SessionErrorCode.RISK_SIGNAL, "A risk signal was heard before confirming", null, "", band, alert)
    }

    /** Reference mode: asks the resolver now, in parallel with the final listen; the answer is checked against the fingerprint. */
    private fun startResolve(h: HeardFrame) {
        if (h !is HeardFrame.Reference) return
        val fp = h.fingerprint
        val done = CompletableFuture<Unit>()
        resolved = done
        var cancelTimer: (() -> Unit)? = null
        fun settle(answer: String?, error: Throwable?, timeout: Boolean) {
            if (done.isDone) return
            cancelTimer?.invoke()
            when {
                timeout -> fail(SessionErrorCode.TIMEOUT, "The resolver did not answer in time")
                error != null -> fail(SessionErrorCode.NOT_FOUND, "The resolver failed", cause = error)
                answer == null -> fail(SessionErrorCode.NOT_FOUND, "Unknown fingerprint $fp")
                !payloadId(answer).startsWith(fp) -> fail(SessionErrorCode.MISMATCH, "The resolved text does not match the fingerprint")
                else -> text = answer
            }
            done.complete(Unit)
        }
        cancelTimer = s.after(RESOLVE_TIMEOUT_SECONDS) { settle(null, null, true) }
        try {
            val async = options.resolveAsync
            if (async != null) async.resolve(fp).whenComplete { v, e -> settle(v, e, false) }
            else settle(options.resolve!!.resolve(fp), null, false)
        } catch (e: Throwable) {
            settle(null, e, false)
        }
    }

    private fun deliverWhenReady() {
        cancelFinal = null
        if (state != ReceiverState.LISTENING && state != ReceiverState.FINAL_LISTEN) return
        val r = resolved
        if (r != null && !r.isDone) r.whenComplete { _, _ -> deliverNow() } else deliverNow()
    }

    private fun deliverNow() {
        if ((state != ReceiverState.LISTENING && state != ReceiverState.FINAL_LISTEN) || text == null) return
        val h = first!!
        val got = Received(text!!, h.band, if (h is HeardFrame.Payload) Mode.PAYLOAD else Mode.REFERENCE)
        received = got
        s.clear() // the listening timeout
        setState(ReceiverState.DELIVERED)
        result.complete(got)
        val sec = security
        if (sec.confirmWatch) {
            watching = true
            watchStart = platform.now()
            s.after(sec.confirmWatchSeconds) {
                if (!watching) return@after
                watchExpired = true
                stopWatch()
                maybeFinished()
            }
        }
        if (options.ack) startAcks(h) else receipt.complete(false)
        maybeFinished()
    }

    /** Ack runs and receipt listens, alternating, until the receipt or ACK_TOTAL_SECONDS. */
    private fun startAcks(h: HeardFrame) {
        acking = true
        val rate = platform.sampleRate
        // Timed with the longest repetition the sender may be playing now (it may have hopped or gone robust since this one).
        val run = ackRunSeconds(longestRepetitionSeconds(if (h is HeardFrame.Payload) h.text else null))
        val heard = AckHeard(h.band, h.profile)
        val audio = loopFrames(listOf(ackFrame(AckFrame(false, ackId(text!!), heard))), run, rate, options.level)
        val until = platform.now() + ACK_TOTAL_SECONDS
        fun round() {
            if (!acking) return
            if (platform.now() >= until) return endAcks(false)
            stopAcks = s.play(audio) { stopAcks = null; s.after(RECEIPT_LISTEN_SECONDS) { round() } }
        }
        round()
    }

    private fun endAcks(gotReceipt: Boolean) {
        if (!acking) { if (!gotReceipt) receipt.complete(false); return }
        acking = false
        stopAcks?.invoke()
        stopAcks = null
        receipt.complete(gotReceipt)
        maybeFinished()
    }

    private fun stopWatch() {
        if (!watching) return
        watching = false
        watchEnd = platform.now()
    }

    /** Something showed up during the confirm watch: the gate closes for good. A different transmission also gets a risk frame. */
    private fun conflictWith(code: SessionErrorCode, message: String, reason: RiskReason?, intruder: String, avoid: Band, heard: Alert? = null) {
        stopWatch()
        val alert = if (reason != null) emitRisk(reason, intruder, avoid) else heard // before endAcks: `finished` must wait for the risk frame
        endAcks(false)
        conflict = SessionError(code, message, alert)
        maybeFinished()
    }

    /** Before delivery: the result fails. [risk]: also loop a risk frame (emit AUTO). */
    private fun fail(code: SessionErrorCode, message: String, risk: Risk? = null, heard: Alert? = null, cause: Throwable? = null) {
        if (state != ReceiverState.LISTENING && state != ReceiverState.FINAL_LISTEN) return
        s.clear()
        val alert = if (risk != null) emitRisk(risk.reason, risk.intruder, risk.avoid) else heard
        setState(ReceiverState.FAILED)
        result.completeExceptionally(SessionError(code, message, alert, cause))
        receipt.complete(false)
        maybeFinished()
    }

    private fun emitRisk(reason: RiskReason, intruder: String, avoid: Band?): Alert? {
        val cfg = security.riskSignal
        if (cfg.emit != Emit.AUTO) return null
        val f = first
        val r = RiskFrame(reason, Role.RECEIVER, 0, if (f != null) shortOf(f) else UNKNOWN_SHORT, intruder)
        memory.note(riskKey(r), platform.now())
        val work = f?.band ?: avoid ?: Band.TOP
        val a = riskAudio(r, work, avoid, cfg.durationMs / 1000.0, platform.sampleRate, options.level)
        val alert = Alert(Alert.Direction.EMITTED, r.reason, r.source, r.hops, r.own, r.intruder, a.bands, true, false)
        options.onAlert?.invoke(alert)
        quarantine(r)
        alerting = true
        s.play(a.audio) { alerting = false; maybeFinished() }
        return alert
    }

    private fun quarantine(r: RiskFrame) {
        val ms = security.riskSignal.quarantineMs
        if (ms > 0) options.onQuarantine?.invoke(QuarantineNotice(ms, r.reason, r.source))
    }

    private fun maybeFinished() {
        val listening = state == ReceiverState.LISTENING || state == ReceiverState.FINAL_LISTEN || watching || acking
        if (!listening && !alerting) {
            s.clear()
            finished.complete(Unit)
        }
    }

    /** Short id of what was received, for apps that log alerts. */
    val short: String? get() = text?.let { shortId(it) }
}
