package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Codec
import io.github.beaglecoder.bysound.modem.Frame
import io.github.beaglecoder.bysound.modem.Modem
import io.github.beaglecoder.bysound.modem.Modulator
import io.github.beaglecoder.bysound.modem.Profile
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

// Shared by the sender and the receiver: timings, errors, alerts and the per-device memory of risk incidents. See docs/SESSION.md.

/** After every repetition the sender is silent and listens this long (acks, risk frames, other emitters). */
const val LISTEN_WINDOW_SECONDS = 2.0
/** Own content heard this long after the last own repetition ended is someone else playing it (a clone). */
const val CLONE_GUARD_SECONDS = 0.5
/** Receiver: keeps listening this long after the first frame before delivering. */
const val FINAL_LISTEN_SECONDS = 1.5
/** A header on another band during the final listen extends it to at least this long after the header... */
const val FINAL_EXTEND_SECONDS = 2.5
/** ...but never past this, counted from the first frame. */
const val FINAL_LISTEN_MAX_SECONDS = 6.0
const val ACK_MARGIN_SECONDS = 0.5
const val ACK_RUN_MIN_SECONDS = 3.0
const val ACK_RUN_MAX_SECONDS = 20.0
/** Receiver: after each ack run it listens this long for the receipt. */
const val RECEIPT_LISTEN_SECONDS = 3.0
/** Receiver: stops acking this long after delivery even without a receipt. */
const val ACK_TOTAL_SECONDS = 30.0
/** Receiver: a reference resolver that has not answered by then fails with TIMEOUT. */
const val RESOLVE_TIMEOUT_SECONDS = 15.0

/** How long the receiver loops acks, from the repetition length (frame + gap). */
fun ackRunSeconds(repetitionSeconds: Double) =
    minOf(ACK_RUN_MAX_SECONDS, maxOf(ACK_RUN_MIN_SECONDS, repetitionSeconds + 2 * LISTEN_WINDOW_SECONDS + ACK_MARGIN_SECONDS))

/** How long the sender loops receipts after hearing an ack: covers the end of the ack run and the receiver's receipt listen. */
fun receiptSeconds(repetitionSeconds: Double) = ackRunSeconds(repetitionSeconds) + RECEIPT_LISTEN_SECONDS

enum class SessionErrorCode {
    /** Sender, pre-listen: another transmission is already sounding. Nothing was sent. */
    OTHER_TRANSMISSION,
    /** Sender: its own data is being played by someone else. */
    CLONED_TRANSMISSION,
    /** Sender: another transmission was heard while sending. */
    OTHER_EMITTER,
    /** A risk frame was heard (and honored). */
    RISK_SIGNAL,
    /** Receiver: two different transmissions, or two overlapping frames. */
    MULTIPLE_TRANSMISSIONS,
    TIMEOUT,
    /** Sender: stop() was called before the ack. */
    STOPPED,
    /** Receiver: cancel() was called. */
    CANCELLED,
    /** Receiver, reference mode: the resolver did not know the fingerprint (or failed). */
    NOT_FOUND,
    /** Receiver, reference mode: the resolved text does not hash to the fingerprint. */
    MISMATCH,
    /** Receiver: a reference frame was heard and there is no resolver. */
    NO_RESOLVER,
}

class SessionError(val code: SessionErrorCode, message: String, val alert: Alert? = null, cause: Throwable? = null) : RuntimeException(message, cause)

/** The [SessionError] behind a failed future (futures wrap what a dependent stage throws). */
fun Throwable.sessionError(): SessionError? = (if (this is CompletionException) cause else this) as? SessionError

/** A risk frame this device emitted or heard. */
data class Alert(
    val direction: Direction,
    val reason: RiskReason,
    /** Who originated the alert. */
    val source: Role,
    val hops: Int,
    /** Short ids (6 hex) carried by the frame. */
    val own: String,
    val intruder: String,
    /** Heard: the band it was heard on. Emitted: the bands it is looped on. */
    val bands: List<Band>,
    /** Heard: the frame's `own` is the short id of this side's data. Informative only. */
    val matchesMine: Boolean,
    /** Emitted as a relay of a heard alert. */
    val relay: Boolean,
) {
    enum class Direction { EMITTED, HEARD }
}

/** Suggested quarantine after a risk frame. bysound never blocks anything because of it: what to do is up to the app. */
data class QuarantineNotice(val durationMs: Long, val reason: RiskReason, val source: Role)

/** Per device: the incidents it emitted or relayed, so it ignores its own frames and relays an incident at most once. */
class RiskMemory {
    private val incidents = HashMap<String, Double>()
    @Synchronized fun note(key: String, now: Double) { incidents[key] = now }
    @Synchronized fun isOwn(key: String, now: Double, windowSeconds: Double): Boolean {
        val at = incidents[key] ?: return false
        return now - at < windowSeconds
    }
}
val sharedRiskMemory = RiskMemory()

/** The device, as the session layer sees it. Times are seconds on one monotonic clock. */
interface Platform {
    val sampleRate: Int
    fun now(): Double
    /** Calls [fn] once after [seconds]. Returns a function that cancels it. */
    fun after(seconds: Double, fn: () -> Unit): () -> Unit
    /** Plays [audio] from now. [onEnd] runs when it finished on its own (not after stop). Returns stop. */
    fun play(audio: FloatArray, onEnd: () -> Unit): () -> Unit
    /** Uniform in [0, 1). Injected so tests are reproducible. */
    fun random(): Double
}

/** One repetition of a frame, with its gap, at the device rate. */
internal fun renderRepetition(text: String, mode: Mode, band: Band, rate: Int, level: Float, set: Int = 0, profile: Profile = Profile.NORMAL): FloatArray {
    val f = if (mode == Mode.PAYLOAD) Codec.payloadFrame(text, band, profile) else Codec.referenceFrame(fromHex(fingerprint(text)), band, set, profile)
    return Modulator.fitLevel(Modulator.render(f.symbols, f.channel, rate, Modem.GAP_SECONDS), level)
}

/** Control frames looped back to back ([CONTROL_GAP_SECONDS] after each), alternating [frames], whole frames only, at least [seconds]. */
internal fun loopFrames(frames: List<Frame>, seconds: Double, rate: Int, level: Float): FloatArray {
    val audio = frames.map { Modulator.fitLevel(Modulator.render(it.symbols, it.channel, rate, CONTROL_GAP_SECONDS), level) }
    val parts = ArrayList<FloatArray>()
    var length = 0
    var i = 0
    while (length < seconds * rate) {
        val a = audio[i++ % audio.size]
        parts.add(a)
        length += a.size
    }
    val out = FloatArray(length)
    var at = 0
    for (p in parts) { p.copyInto(out, at); at += p.size }
    return out
}

internal class RiskAudio(val bands: List<Band>, val audio: FloatArray)

/** The risk frame looped on its bands (see [riskBands]). */
internal fun riskAudio(r: RiskFrame, work: Band, avoid: Band?, seconds: Double, rate: Int, level: Float): RiskAudio {
    val bands = riskBands(work, avoid)
    return RiskAudio(bands, loopFrames(bands.map { riskFrame(r, it) }, seconds, rate, level))
}

/** What both state machines share: timers and sounds that a stop cancels at once. */
internal class Scheduler(val platform: Platform) {
    private val timers = LinkedHashSet<() -> Unit>()
    private val sounds = LinkedHashSet<() -> Unit>()

    fun after(seconds: Double, fn: () -> Unit): () -> Unit {
        lateinit var cancel: () -> Unit
        val c = platform.after(seconds) { timers.remove(cancel); fn() }
        cancel = c
        timers.add(cancel)
        return { if (timers.remove(cancel)) cancel() }
    }

    fun play(audio: FloatArray, onEnd: () -> Unit): () -> Unit {
        lateinit var stop: () -> Unit
        val s = platform.play(audio) { sounds.remove(stop); onEnd() }
        stop = s
        sounds.add(stop)
        return { if (sounds.remove(stop)) stop() }
    }

    fun clear() {
        val t = timers.toList(); val s = sounds.toList()
        timers.clear(); sounds.clear()
        for (c in t) c()
        for (c in s) c()
    }
}

/** What a sender sends: the text itself, or only its 12-byte fingerprint (the receiver resolves it locally). */
enum class Mode { PAYLOAD, REFERENCE }

/** Waits for a future without blocking the thread: `val delivered = send.delivered.await()` inside a coroutine (kotlinx or any other). */
suspend fun <T> CompletableFuture<T>.await(): T = kotlin.coroutines.suspendCoroutine { c ->
    whenComplete { v, e -> if (e == null) c.resume(v) else c.resumeWithException(e.sessionError() ?: e) }
}
