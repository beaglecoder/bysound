package io.github.beaglecoder.bysound.modem

/**
 * A payload frame received and verified (Reed-Solomon and CRC-32 closed).
 *
 * @property correctedBytes bytes repaired by Reed-Solomon in the blocks of the closing lap(s)
 * @property combined number of laps of the repeated frame that were needed (1 = read in a single lap)
 * @property score preamble match of the frame, 0..1
 */
data class Heard(
    val text: String,
    val band: Band,
    val profile: Profile,
    val correctedBytes: Int,
    val combined: Int,
    val score: Double = 0.0,
)

/** What a [Receiver] reports besides frames. All callbacks run on the thread that calls [Receiver.feed]. */
interface ReceiverEvents {
    /** A frame was received and verified. */
    fun onFrame(heard: Heard)

    /** A valid header was read on that band. */
    fun onHeader(band: Band) {}

    /** [closed] of [total] blocks of the text have been recovered so far. */
    fun onBlocks(closed: Int, total: Int) {}

    /** Two transmissions overlap. */
    fun onClash() {}
}
