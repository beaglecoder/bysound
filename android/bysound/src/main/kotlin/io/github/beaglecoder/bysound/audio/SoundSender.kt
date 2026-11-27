package io.github.beaglecoder.bysound.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Modem
import io.github.beaglecoder.bysound.modem.Profile

/**
 * Plays a text as sound, looping the frame until [stop].
 *
 * While sending, the track is pinned to the built-in loudspeaker even with headphones connected (they would not reach the
 * other device) and, only if that routing worked, the music volume is raised to the maximum (never when the output is a
 * headset or Bluetooth). The previous volume is restored on [stop] and on every failure path. Thread-safe; the session layer
 * plays through [playOnce] from its own thread.
 */
class SoundSender(context: Context) {
    private val audio = context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var track: AudioTrack? = null
    private var savedVolume = -1

    /** True while a frame is looping. */
    val isSending: Boolean get() = track != null

    private var held = false
    private val once = HashSet<AudioTrack>()

    private fun newTrack(samples: FloatArray, sampleRate: Int): AudioTrack = AudioTrack.Builder()
        .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
        .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setSampleRate(sampleRate).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
        .setTransferMode(AudioTrack.MODE_STATIC)
        .setBufferSizeInBytes(samples.size * 4)
        .build()

    /**
     * Session use: keeps the music volume at the maximum (and the previous one saved) across several [playOnce] calls, so it does not
     * jump between repetitions. Only when the sound really leaves through the built-in speaker. Undo with [releaseVolume].
     */
    @Synchronized
    fun holdVolume() {
        held = true
        if (builtInSpeaker() != null) raiseVolume()
    }

    /** Restores the volume saved by [holdVolume]. Safe to call when idle. */
    @Synchronized
    fun releaseVolume() {
        held = false
        restoreVolume()
    }

    /**
     * Plays [samples] once (no loop) on the built-in loudspeaker, even with headphones. Returns a function that stops it and frees the
     * track; call it also after the sound ended on its own (the caller knows the duration). With [holdVolume] active the volume is
     * already at the maximum.
     */
    @Synchronized
    fun playOnce(samples: FloatArray, sampleRate: Int = SAMPLE_RATE): () -> Unit {
        val t = newTrack(samples, sampleRate)
        try {
            builtInSpeaker()?.let { t.setPreferredDevice(it) }
            check(t.write(samples, 0, samples.size, AudioTrack.WRITE_BLOCKING) == samples.size) { "AudioTrack.write failed" }
            t.play()
        } catch (e: Exception) {
            t.release()
            throw e
        }
        once.add(t)
        return { synchronized(this) { if (once.remove(t)) { runCatching { t.stop() }; t.release() } } }
    }

    /** Starts looping [text] on [band]. Replaces a previous send. */
    @Synchronized
    fun send(text: String, band: Band = Band.TOP, profile: Profile = Profile.NORMAL, sampleRate: Int = SAMPLE_RATE) {
        stop()
        val samples = Modem.encode(text, band, profile, sampleRate)
        val t = newTrack(samples, sampleRate)
        try {
            // Only touch the volume when the sound really leaves through the built-in speaker.
            val speaker = builtInSpeaker()
            val onSpeaker = speaker != null && t.setPreferredDevice(speaker)
            check(t.write(samples, 0, samples.size, AudioTrack.WRITE_BLOCKING) == samples.size) { "AudioTrack.write failed" }
            t.setLoopPoints(0, samples.size, -1)
            if (onSpeaker) raiseVolume()
            t.play()
            track = t
            val routed = t.routedDevice
            if (routed != null && routed.type != AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) restoreVolume()
        } catch (e: Exception) {
            t.release()
            restoreVolume()
            throw e
        }
    }

    /** Stops the loop and restores the volume. Safe to call when idle. */
    @Synchronized
    fun stop() {
        track?.let { runCatching { it.stop() }; it.release() }
        track = null
        if (!held) restoreVolume()
    }

    private fun builtInSpeaker(): AudioDeviceInfo? =
        audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }

    private fun raiseVolume() {
        if (savedVolume < 0) savedVolume = audio.getStreamVolume(AudioManager.STREAM_MUSIC)
        audio.setStreamVolume(AudioManager.STREAM_MUSIC, audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC), 0)
    }

    private fun restoreVolume() {
        if (savedVolume >= 0) runCatching { audio.setStreamVolume(AudioManager.STREAM_MUSIC, savedVolume, 0) }
        savedVolume = -1
    }

    companion object {
        /** Sample rate of the emitted audio. */
        const val SAMPLE_RATE = 48000
    }
}
