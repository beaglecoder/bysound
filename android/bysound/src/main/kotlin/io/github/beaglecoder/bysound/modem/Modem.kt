package io.github.beaglecoder.bysound.modem

/** Entry point for building bysound v4 payload frames. */
object Modem {
    /** Pause after every frame, in seconds. */
    const val GAP_SECONDS = Protocol.GAP_SECONDS

    /** Hard cap on one transmission (preamble + header + body + gap), in seconds. */
    const val MAX_FRAME_SECONDS = Protocol.MAX_FRAME_SECONDS

    /**
     * Largest packed payload (bytes after text packing, before the CRC-32) that fits in [MAX_FRAME_SECONDS] for the band and profile.
     * ASCII letters pack 1:1 (so it is also the letter count); digit pairs pack 2:1.
     */
    fun maxPayloadBytes(band: Band, profile: Profile = Profile.NORMAL): Int = Codec.maxPayloadBytes(band, profile)

    /** A text whose transmission would last more than [MAX_FRAME_SECONDS] throws [TextTooLongException] (an [IllegalArgumentException]). */
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
