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
 * While sending, the music volume is raised to the maximum and the track is pinned to the built-in loudspeaker even with
 * headphones connected (they would not reach the other device). The previous volume is restored on [stop]. Not thread-safe:
 * call from one thread (the main thread is fine).
 */
class SoundSender(context: Context) {
    private val audio = context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var track: AudioTrack? = null
    private var savedVolume = -1

    /** True while a frame is looping. */
    val isSending: Boolean get() = track != null

    /** Starts looping [text] on [band]. Replaces a previous send. */
    fun send(text: String, band: Band = Band.TOP, profile: Profile = Profile.NORMAL, sampleRate: Int = SAMPLE_RATE) {
        stop()
        val samples = Modem.encode(text, band, profile, sampleRate)
        val t = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setSampleRate(sampleRate).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setTransferMode(AudioTrack.MODE_STATIC)
            .setBufferSizeInBytes(samples.size * 4)
            .build()
        try {
            builtInSpeaker()?.let { t.preferredDevice = it }
            check(t.write(samples, 0, samples.size, AudioTrack.WRITE_BLOCKING) == samples.size) { "AudioTrack.write failed" }
            t.setLoopPoints(0, samples.size, -1)
            raiseVolume()
            t.play()
            track = t
        } catch (e: Exception) {
            t.release()
            restoreVolume()
            throw e
        }
    }

    /** Stops the loop and restores the volume. Safe to call when idle. */
    fun stop() {
        track?.let { runCatching { it.stop() }; it.release() }
        track = null
        restoreVolume()
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
