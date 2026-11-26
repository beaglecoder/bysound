package io.github.beaglecoder.bysound.audio

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import io.github.beaglecoder.bysound.modem.Level
import io.github.beaglecoder.bysound.session.AirEvent
import io.github.beaglecoder.bysound.session.Calibration
import io.github.beaglecoder.bysound.session.CalibrationStore
import io.github.beaglecoder.bysound.session.Confirmation
import io.github.beaglecoder.bysound.session.Delivered
import io.github.beaglecoder.bysound.session.Ears
import io.github.beaglecoder.bysound.session.Platform
import io.github.beaglecoder.bysound.session.ReceiverOptions
import io.github.beaglecoder.bysound.session.ReceiverSession
import io.github.beaglecoder.bysound.session.ReceiverState
import io.github.beaglecoder.bysound.session.Received
import io.github.beaglecoder.bysound.session.SenderOptions
import io.github.beaglecoder.bysound.session.SenderSession
import io.github.beaglecoder.bysound.session.SenderState
import io.github.beaglecoder.bysound.session.parseCalibration
import io.github.beaglecoder.bysound.session.encode
import io.github.beaglecoder.bysound.session.sharedCalibrationStore
import java.util.Random
import java.util.concurrent.CompletableFuture

/** A [CalibrationStore] in SharedPreferences: the speaker of a device does not change between launches. */
class SharedPreferencesCalibrationStore(context: Context, private val key: String = "calibration") : CalibrationStore {
    private val prefs = context.applicationContext.getSharedPreferences("bysound", Context.MODE_PRIVATE)
    override fun load(): Calibration? = parseCalibration(prefs.getString(key, null))
    override fun save(c: Calibration?) { prefs.edit().apply { if (c == null) remove(key) else putString(key, c.encode()) }.apply() }
}

/** The device as the session layer sees it: the loudspeaker through [SoundSender], the clock and timers on one handler thread. */
internal class AndroidPlatform(private val handler: Handler, private val sender: SoundSender) : Platform {
    private val random = Random()
    override val sampleRate = SoundSender.SAMPLE_RATE
    override fun now() = System.nanoTime() / 1e9
    override fun after(seconds: Double, fn: () -> Unit): () -> Unit {
        val r = Runnable { fn() }
        handler.postDelayed(r, Math.max(0L, Math.round(seconds * 1000)))
        return { handler.removeCallbacks(r) }
    }
    override fun play(audio: FloatArray, onEnd: () -> Unit): () -> Unit {
        val stopTrack = sender.playOnce(audio, sampleRate)
        val cancel = after(audio.size.toDouble() / sampleRate + 0.03) { stopTrack(); onEnd() }
        return { cancel(); stopTrack() }
    }
    override fun random() = random.nextDouble()
}

/** Everything one session owns: a handler thread (all the state machine runs there), the loudspeaker and the microphone. */
internal class SessionRuntime(context: Context, onLevel: ((Level) -> Unit)?, onError: ((String) -> Unit)?) {
    private val thread = HandlerThread("bysound-session").also { it.start() }
    val handler = Handler(thread.looper)
    val speaker = SoundSender(context)
    val platform: Platform = AndroidPlatform(handler, speaker)
    private val mic = SoundListener(context, Ears.adapter { e -> handler.post { hear?.invoke(e) } }, onLevel, onError)
    @Volatile var hear: ((AirEvent) -> Unit)? = null

    fun open() {
        speaker.holdVolume()
        try { mic.start() } catch (e: Exception) { close(); throw e }
    }

    fun close() {
        mic.stop()
        speaker.releaseVolume()
        thread.quitSafely()
    }
}

/** A running send: see [SenderSession]. Completion callbacks run on the session thread. */
class SendSession internal constructor(private val rt: SessionRuntime, private val session: SenderSession) {
    /** Completes with the ack (where the receiver read it); fails with a [io.github.beaglecoder.bysound.session.SessionError]. */
    val delivered: CompletableFuture<Delivered> get() = session.delivered
    /** Completes when nothing plays any more and the microphone is released. */
    val finished: CompletableFuture<Unit> get() = session.finished
    val state: SenderState get() = session.state
    /** Stops at once (also a risk frame being played). */
    fun stop() { rt.handler.post { session.stop() } }
}

/** A running receive: see [ReceiverSession]. */
class ReceiveSession internal constructor(private val rt: SessionRuntime, private val session: ReceiverSession) {
    /** The data, after the final listen. Fails with a [io.github.beaglecoder.bysound.session.SessionError]. */
    val result: CompletableFuture<Received> get() = session.result
    /** true if the sender answered the ack with a receipt. */
    val receipt: CompletableFuture<Boolean> get() = session.receipt
    val finished: CompletableFuture<Unit> get() = session.finished
    val state: ReceiverState get() = session.state
    /** The confirm gate: call it right before acting on the data. Fails if another emitter or a risk frame appeared meanwhile. */
    fun confirmed(): CompletableFuture<Confirmation> {
        val out = CompletableFuture<Confirmation>()
        rt.handler.post { session.confirmed().whenComplete { v, e -> if (e != null) out.completeExceptionally(e) else out.complete(v) } }
        return out
    }
    /** The person left: stop listening and acking. Never throws. */
    fun cancel() { rt.handler.post { session.cancel() } }
}

/**
 * Sends [text] as a session: listens first (other emitters, noise per channel), repeats it with listen windows down a channel ladder
 * until the receiver acknowledges, remembers where it was read (SharedPreferences, unless you pass your own store) and watches for
 * other emitters. Needs RECORD_AUDIO. Throws [IllegalStateException] if the microphone cannot be opened.
 */
fun sendSession(context: Context, text: String, options: SenderOptions = SenderOptions(), onLevel: ((Level) -> Unit)? = null): SendSession {
    val rt = SessionRuntime(context, onLevel, null)
    val o = if (options.calibration === sharedCalibrationStore) options.copy(calibration = SharedPreferencesCalibrationStore(context)) else options
    val session = SenderSession(text, rt.platform, o)
    rt.hear = { session.hear(it) }
    rt.open()
    rt.handler.post { session.start() }
    session.finished.whenComplete { _, _ -> rt.close() }
    return SendSession(rt, session)
}

/** Listens for one session: delivers the data, acknowledges it by sound and keeps the confirm gate. Needs RECORD_AUDIO. */
fun receiveSession(context: Context, options: ReceiverOptions = ReceiverOptions(), onLevel: ((Level) -> Unit)? = null): ReceiveSession {
    val rt = SessionRuntime(context, onLevel, null)
    val session = ReceiverSession(rt.platform, options)
    rt.hear = { session.hear(it) }
    rt.open()
    rt.handler.post { session.start() }
    session.finished.whenComplete { _, _ -> rt.close() }
    return ReceiveSession(rt, session)
}
