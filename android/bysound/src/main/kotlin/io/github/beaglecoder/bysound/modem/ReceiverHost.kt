package io.github.beaglecoder.bysound.modem

/** Level and state reported about every 60 ms of audio. */
data class Level(val rms: Float, val progress: Double, val failures: Int)

/**
 * Listens to both profiles on the same audio: a normal receiver (all seven bands) and a robust one (TOP, HIGH, UPPER and MID,
 * window and hop twice as long). The level and the progress come from the normal receiver. Use from one thread.
 */
class ReceiverHost(
    sampleRate: Int,
    private val onLevel: ((Level) -> Unit)? = null,
    robust: Boolean = true,
    onFrame: (Heard) -> Unit,
) {
    private val normal = Receiver(sampleRate, Profile.NORMAL, onFrame)
    private val robustReceiver = if (robust) Receiver(sampleRate, Profile.ROBUST, onFrame) else null
    private val every = Math.round(0.06 * sampleRate).toInt()
    private var count = 0
    private var energy = 0.0

    fun feed(x: FloatArray) {
        normal.feed(x)
        robustReceiver?.feed(x)
        for (v in x) energy += v.toDouble() * v
        count += x.size
        if (count < every) return
        onLevel?.invoke(Level(Math.sqrt(energy / count).toFloat(), normal.progress, normal.failures))
        count = 0
        energy = 0.0
    }
}
