package io.github.beaglecoder.bysound.session

/** docs/SESSION.md section 7. The table is test-vectors/security-levels.json; `maximum` is the default. */
enum class SecurityLevel(val id: String) { BASIC("basic"), STANDARD("standard"), MAXIMUM("maximum") }

val DEFAULT_SECURITY_LEVEL = SecurityLevel.MAXIMUM

enum class Emit { AUTO, OFF }
enum class ListenUntil { DELIVERY, CONFIRM }

data class RiskSignal(
    /** AUTO: whoever detects another emitter sounds a risk frame. OFF: it only stops and reports. */
    val emit: Emit,
    /** Listen for risk frames. */
    val listen: Boolean,
    /** Act on one: the sender stops (and relays), the receiver fails / closes the confirm gate. false: only onAlert. */
    val honor: Boolean,
    /** Receiver only: DELIVERY stops listening for risk frames at delivery, CONFIRM keeps listening until confirmed(). */
    val listenUntil: ListenUntil,
    /** How long a receiver loops its risk frame. */
    val durationMs: Long,
    /** How long a sender loops its risk frame (after detecting another emitter, or when relaying one). */
    val detectorDurationMs: Long,
    /** Suggested quarantine sent to onQuarantine. Never enforced. */
    val quarantineMs: Long,
)

data class Security(
    /** Sender: pre-listen and listen windows act on what they hear. Receiver: final listen and clash rejection. */
    val detectMultipleEmitters: Boolean,
    /** Sender: other emitters heard while sending (in the listen windows) stop the session. */
    val listenWhileSending: Boolean,
    /** Sender: listen this long before the first frame, plus up to [scanJitterSeconds] at random. Skipped without detection. */
    val listenBeforeSendingSeconds: Double,
    val scanJitterSeconds: Double,
    /** Receiver: keep watching after delivery until confirmed() / cancel(), at most [confirmWatchSeconds]. */
    val confirmWatch: Boolean,
    val confirmWatchSeconds: Double,
    val riskSignal: RiskSignal,
)

private fun risk(emit: Emit, on: Boolean, until: ListenUntil, durationMs: Long, detectorMs: Long, quarantineMs: Long) =
    RiskSignal(emit, on, on, until, durationMs, detectorMs, quarantineMs)

val SECURITY_LEVELS: Map<SecurityLevel, Security> = mapOf(
    SecurityLevel.BASIC to Security(false, false, 1.0, 0.6, false, 120.0, risk(Emit.OFF, false, ListenUntil.DELIVERY, 4000, 4000, 30000)),
    SecurityLevel.STANDARD to Security(true, true, 2.0, 0.6, true, 120.0, risk(Emit.AUTO, true, ListenUntil.CONFIRM, 4000, 15000, 30000)),
    SecurityLevel.MAXIMUM to Security(true, true, 3.0, 1.2, true, 120.0, risk(Emit.AUTO, true, ListenUntil.CONFIRM, 6000, 30000, 60000)),
)

/** Explicit values for [RiskSignal]; null keeps the level's. */
data class RiskSignalOverrides(
    val emit: Emit? = null, val listen: Boolean? = null, val honor: Boolean? = null, val listenUntil: ListenUntil? = null,
    val durationMs: Long? = null, val detectorDurationMs: Long? = null, val quarantineMs: Long? = null,
)

/** Explicit options win over the level, field by field (also inside [riskSignal]). */
data class SecurityOverrides(
    val detectMultipleEmitters: Boolean? = null, val listenWhileSending: Boolean? = null,
    val listenBeforeSendingSeconds: Double? = null, val scanJitterSeconds: Double? = null,
    val confirmWatch: Boolean? = null, val confirmWatchSeconds: Double? = null,
    val riskSignal: RiskSignalOverrides = RiskSignalOverrides(),
)

fun resolveSecurity(level: SecurityLevel = DEFAULT_SECURITY_LEVEL, o: SecurityOverrides = SecurityOverrides()): Security {
    val b = SECURITY_LEVELS.getValue(level)
    val r = o.riskSignal
    return Security(
        o.detectMultipleEmitters ?: b.detectMultipleEmitters, o.listenWhileSending ?: b.listenWhileSending,
        o.listenBeforeSendingSeconds ?: b.listenBeforeSendingSeconds, o.scanJitterSeconds ?: b.scanJitterSeconds,
        o.confirmWatch ?: b.confirmWatch, o.confirmWatchSeconds ?: b.confirmWatchSeconds,
        RiskSignal(
            r.emit ?: b.riskSignal.emit, r.listen ?: b.riskSignal.listen, r.honor ?: b.riskSignal.honor,
            r.listenUntil ?: b.riskSignal.listenUntil, r.durationMs ?: b.riskSignal.durationMs,
            r.detectorDurationMs ?: b.riskSignal.detectorDurationMs, r.quarantineMs ?: b.riskSignal.quarantineMs,
        ),
    )
}
