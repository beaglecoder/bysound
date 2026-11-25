package io.github.beaglecoder.bysound.modem

/** Entry point for building bysound v4 payload frames. */
object Modem {
    /** Pause after every frame, in seconds. */
    const val GAP_SECONDS = Protocol.GAP_SECONDS

    /** Longest text that fits in one frame is limited to 255 blocks of 48 bytes; this throws [IllegalArgumentException] beyond that. */
    fun symbols(text: String, band: Band, profile: Profile = Profile.NORMAL): Array<IntArray> =
        Codec.payloadFrame(text, band, profile).symbols

    /** Duration in seconds of one lap (frame without the gap). */
    fun frameSeconds(text: String, band: Band, profile: Profile = Profile.NORMAL): Double =
        Codec.payloadFrame(text, band, profile).frameSeconds

    /** The frame as audio samples in -1..1: one frame followed by a 350 ms gap, so it can be played in a loop. */
    fun encode(text: String, band: Band, profile: Profile = Profile.NORMAL, sampleRate: Int, level: Float = 1f): FloatArray {
        val frame = Codec.payloadFrame(text, band, profile)
        return Modulator.fitLevel(Modulator.render(frame.symbols, frame.channel, sampleRate, GAP_SECONDS), level)
    }
}
