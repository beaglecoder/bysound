package io.github.beaglecoder.bysound.audio

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import io.github.beaglecoder.bysound.modem.Heard
import io.github.beaglecoder.bysound.modem.Level
import io.github.beaglecoder.bysound.modem.ReceiverEvents
import io.github.beaglecoder.bysound.modem.ReceiverHost

/**
 * Listens to the microphone and decodes bysound v4 frames (normal and robust profiles) on a background thread.
 *
 * The microphone source is UNPROCESSED when the device supports it (no AGC or noise suppression, which would eat the
 * near-ultrasound tones), otherwise VOICE_RECOGNITION, otherwise MIC. With headphones connected the built-in microphone is used.
 * The callbacks run on the listening thread: post to the main thread before touching views. The caller must hold RECORD_AUDIO.
 */
class SoundListener(
    context: Context,
    /** Everything the receiver reports (payload and reference frames, headers, clashes, acks, risk frames, noise). */
    private val events: ReceiverEvents,
    private val onLevel: ((Level) -> Unit)? = null,
    /** Called on the listening thread when reading from the microphone fails and listening stops by itself. */
    private val onError: ((String) -> Unit)? = null,
) {
    /** Payload frames only. */
    constructor(
        context: Context,
        onFrame: (Heard) -> Unit,
        onLevel: ((Level) -> Unit)? = null,
        onError: ((String) -> Unit)? = null,
    ) : this(context, object : ReceiverEvents { override fun onFrame(heard: Heard) = onFrame(heard) }, onLevel, onError)

    private val audio = context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    /** One listening session: its own run flag, so a late-exiting old thread cannot stop a newer session. */
    private class Session(val record: AudioRecord, val rate: Int) {
        @Volatile var running = true
        var thread: Thread? = null
    }

    @Volatile private var session: Session? = null

    /** True while listening. */
    val isListening: Boolean get() = session?.running == true

    /** Capture sample rate of the current (or last) session. */
    @Volatile var sampleRate: Int = SAMPLE_RATES[0]
        private set

    /** Starts listening. Throws [IllegalStateException] if no microphone can be opened. */
    @SuppressLint("MissingPermission")
    @Synchronized
    fun start() {
        if (isListening) return
        val (record, rate) = open() ?: throw IllegalStateException("Could not open the microphone")
        try {
            record.startRecording()
        } catch (e: Exception) {
            record.release()
            throw e
        }
        sampleRate = rate
        val s = Session(record, rate)
        session = s
        s.thread = Thread({ loop(s) }, "bysound-listener").also { it.start() }
    }

    /** Stops listening and waits for the thread. Safe to call when idle. */
    @Synchronized
    fun stop() {
        val s = session ?: return
        s.running = false
        s.thread?.join(1000)
        if (session === s) session = null
    }

    @SuppressLint("MissingPermission")
    private fun open(): Pair<AudioRecord, Int>? {
        val unprocessed = audio.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true"
        val sources = if (unprocessed) intArrayOf(MediaRecorder.AudioSource.UNPROCESSED, MediaRecorder.AudioSource.VOICE_RECOGNITION)
        else intArrayOf(MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC)
        for (rate in SAMPLE_RATES) for (source in sources) {
            val min = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            if (min <= 0) continue
            val r = try {
                AudioRecord(source, rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(min, rate / 5 * 2))
            } catch (e: Exception) { null }
            if (r != null && r.state == AudioRecord.STATE_INITIALIZED && r.sampleRate == rate) {
                audio.getDevices(AudioManager.GET_DEVICES_INPUTS).firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_MIC }
                    ?.let { r.preferredDevice = it }
                return r to rate
            }
            r?.release()
        }
        return null
    }

    private fun loop(s: Session) {
        val host = ReceiverHost(s.rate, events, onLevel)
        val pcm = ShortArray(s.rate / 50)
        val chunk = FloatArray(pcm.size)
        try {
            while (s.running) {
                val n = s.record.read(pcm, 0, pcm.size)
                if (n < 0) {
                    if (s.running) onError?.invoke("Microphone read failed (error $n)")
                    break
                }
                if (n == 0) continue
                val x = if (n == chunk.size) chunk else FloatArray(n)
                for (i in 0 until n) x[i] = pcm[i] / 32768f
                host.feed(x)
            }
        } finally {
            runCatching { s.record.stop() }
            s.record.release()
            s.running = false
        }
    }

    companion object {
        /** Capture sample rates tried in order (48 kHz lets the receiver use the FFT). */
        val SAMPLE_RATES = intArrayOf(48000, 44100)

        /** Preferred capture sample rate. */
        const val SAMPLE_RATE = 48000
    }
}
