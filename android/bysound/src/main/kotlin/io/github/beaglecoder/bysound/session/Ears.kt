package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Heard
import io.github.beaglecoder.bysound.modem.HeardReference
import io.github.beaglecoder.bysound.modem.Profile
import io.github.beaglecoder.bysound.modem.Protocol
import io.github.beaglecoder.bysound.modem.ReceiverEvents
import io.github.beaglecoder.bysound.modem.ReceiverHost

/** A payload or reference frame, fully decoded. */
sealed interface HeardFrame {
    val band: Band
    val profile: Profile
    class Payload(val text: String, override val band: Band, override val profile: Profile) : HeardFrame
    class Reference(val fingerprint: String, override val band: Band, override val profile: Profile) : HeardFrame
}

/** Everything the session layer reacts to (docs/SESSION.md section 4). */
sealed interface AirEvent {
    class Frame(val heard: HeardFrame) : AirEvent
    /** A payload or reference header read with near certainty: a frame is starting on [band]. */
    class Header(val band: Band) : AirEvent
    /** The modem saw two overlapping frames and delivered neither. */
    object Clash : AirEvent
    class Ack(val ack: AckFrame, val band: Band) : AirEvent
    class Risk(val risk: RiskFrame, val band: Band) : AirEvent
    /** Every 0.25 s: what each band measures (signal included), for the sender's ambient check. */
    class Noise(val noise: BandNoise) : AirEvent
}

/** Microphone samples in, [AirEvent]s out: the modem [ReceiverHost] reports frames, headers, clashes, acks, risk frames and noise. */
class Ears(rate: Int, private val onEvent: (AirEvent) -> Unit, robust: Boolean = true) {
    private val host = ReceiverHost(rate, adapter(onEvent), null, robust)

    fun feed(x: FloatArray) = host.feed(x)

    companion object {
        /** The modem events as [AirEvent]s. Use it with a [ReceiverHost] you own (the Android listener does). */
        fun adapter(onEvent: (AirEvent) -> Unit): ReceiverEvents = object : ReceiverEvents {
            override fun onFrame(heard: Heard) = onEvent(AirEvent.Frame(HeardFrame.Payload(heard.text, heard.band, heard.profile)))
            override fun onReference(heard: HeardReference) = onEvent(AirEvent.Frame(HeardFrame.Reference(heard.fingerprint, heard.band, heard.profile)))
            override fun onHeader(band: Band) = onEvent(AirEvent.Header(band))
            override fun onClash() = onEvent(AirEvent.Clash)
            override fun onNoise(noise: Map<Band, Double>) = onEvent(AirEvent.Noise(noise))
            override fun onControl(magic: Int, data: IntArray, band: Band) {
                if (magic == Protocol.MAGIC_ACK) parseAck(data)?.let { onEvent(AirEvent.Ack(it, band)) }
                else if (magic == Protocol.MAGIC_RISK) parseRisk(data)?.let { onEvent(AirEvent.Risk(it, band)) }
            }
        }
    }
}
