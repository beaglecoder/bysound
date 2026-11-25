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
    private val onFrame: (Heard) -> Unit,
    private val onLevel: ((Level) -> Unit)? = null,
) {
    private val audio = context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    @Volatile private var running = false
    private var thread: Thread? = null

    /** True while listening. */
    val isListening: Boolean get() = running

    /** Starts listening. Throws [IllegalStateException] if no microphone can be opened. */
    @SuppressLint("MissingPermission")
    @Synchronized
    fun start() {
        if (running) return
        val record = open() ?: throw IllegalStateException("Could not open the microphone")
        record.startRecording()
        running = true
        thread = Thread({ loop(record) }, "bysound-listener").also { it.start() }
    }

    /** Stops listening and waits for the thread. Safe to call when idle. */
    @Synchronized
    fun stop() {
        running = false
        thread?.join(1000)
        thread = null
    }

    @SuppressLint("MissingPermission")
    private fun open(): AudioRecord? {
        val unprocessed = audio.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true"
        val sources = if (unprocessed) intArrayOf(MediaRecorder.AudioSource.UNPROCESSED, MediaRecorder.AudioSource.VOICE_RECOGNITION)
        else intArrayOf(MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC)
        for (source in sources) {
            val min = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            if (min <= 0) continue
            val r = try {
                AudioRecord(source, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(min, SAMPLE_RATE / 5 * 2))
            } catch (e: Exception) { null }
            if (r != null && r.state == AudioRecord.STATE_INITIALIZED) {
                audio.getDevices(AudioManager.GET_DEVICES_INPUTS).firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_MIC }
                    ?.let { r.preferredDevice = it }
                return r
            }
            r?.release()
        }
        return null
    }

    private fun loop(record: AudioRecord) {
        val host = ReceiverHost(SAMPLE_RATE, onLevel) { onFrame(it) }
        val pcm = ShortArray(SAMPLE_RATE / 50)
        val chunk = FloatArray(pcm.size)
        try {
            while (running) {
                val n = record.read(pcm, 0, pcm.size)
                if (n <= 0) { if (n < 0) break else continue }
                val x = if (n == chunk.size) chunk else FloatArray(n)
                for (i in 0 until n) x[i] = pcm[i] / 32768f
                host.feed(x)
            }
        } finally {
            runCatching { record.stop() }
            record.release()
            running = false
        }
    }

    companion object {
        /** Capture sample rate (48 kHz lets the receiver use the FFT). */
        const val SAMPLE_RATE = 48000
    }
}
