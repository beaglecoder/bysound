package io.github.beaglecoder.bysound.session

import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Checksums
import io.github.beaglecoder.bysound.modem.Channel
import io.github.beaglecoder.bysound.modem.Codec
import io.github.beaglecoder.bysound.modem.Frame
import io.github.beaglecoder.bysound.modem.Profile
import io.github.beaglecoder.bysound.modem.Protocol
import io.github.beaglecoder.bysound.modem.ReedSolomon
import io.github.beaglecoder.bysound.modem.TONES
import java.security.MessageDigest

// Session frames: acknowledgement / receipt (0x41) and risk signal (0x58). Byte layout in docs/SESSION.md section 3.

/** Reed-Solomon parity bytes of an ack and of a risk frame. */
const val CONTROL_PARITY = 8
const val ACK_DATA = 7
const val RISK_DATA = 10
/** A risk frame heard with this many hops is never relayed again. */
const val RISK_MAX_HOPS = 2
/** Acks travel on mid, receipts on high: they share no tone, so both sides can sound at once. */
val ACK_BAND = Band.MID
val RECEIPT_BAND = Band.HIGH
/** Silence after each control frame (ack, receipt, risk) when they are looped. */
const val CONTROL_GAP_SECONDS = 0.15
const val UNKNOWN_SHORT = "000000"

enum class RiskReason { OTHER, CLONE, OVERLAP, CANCELLED }
enum class Role { SENDER, RECEIVER }

/** Where the receiver read the data: the band and profile of the frame it delivered. */
data class AckHeard(val band: Band, val profile: Profile)

/** [receipt] false: ack (the receiver got the data); true: receipt (the sender heard the ack). [id] is 8 hex characters. */
data class AckFrame(val receipt: Boolean, val id: String, val heard: AckHeard? = null)

/** [own] and [intruder] are 6 hex characters ([UNKNOWN_SHORT] if unknown). */
data class RiskFrame(val reason: RiskReason, val source: Role, val hops: Int, val own: String, val intruder: String)

/** SHA-256 of the UTF-8 text, 64 hex characters. */
fun payloadId(text: String): String = hex(MessageDigest.getInstance("SHA-256").digest(text.toByteArray(Charsets.UTF_8)))
/** The 12-byte fingerprint a reference frame carries, 24 hex characters. */
fun fingerprint(text: String) = payloadId(text).substring(0, 24)
/** What an ack carries: 4 bytes, 8 hex characters. */
fun ackId(text: String) = payloadId(text).substring(0, 8)
/** What a risk frame carries: 3 bytes, 6 hex characters. */
fun shortId(text: String) = payloadId(text).substring(0, 6)

internal fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it.toInt() and 255) }
internal fun hex(a: IntArray) = a.joinToString("") { "%02x".format(it) }
internal fun fromHex(s: String) = IntArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16) }
private fun withCrc(data: IntArray): IntArray { val c = Checksums.crc16(data); return data + intArrayOf(c shr 8, c and 255) }

/** High nibble of the ack's first byte: 0 (not stated), or robust << 3 | (channel id + 1). */
private fun heardNibble(h: AckHeard?) = if (h == null) 0 else (if (h.profile == Profile.ROBUST) 8 else 0) or (channelIdOf(h.band) + 1)

internal fun channelIdOf(b: Band): Int = b.channelId

/** Ack data bytes: `[heard << 4 | kind (2 ack, 3 receipt), id0..id3, CRC-16 hi, lo]`. */
fun ackBytes(a: AckFrame): IntArray = withCrc(intArrayOf((heardNibble(a.heard) shl 4) or (if (a.receipt) 3 else 2)) + fromHex(a.id))

/** Risk data bytes: `[reason 1..4, hops << 4 | source, own0..2, intruder0..2, CRC-16 hi, lo]`. */
fun riskBytes(r: RiskFrame): IntArray =
    withCrc(intArrayOf(r.reason.ordinal + 1, ((r.hops and 15) shl 4) or r.source.ordinal) + fromHex(r.own) + fromHex(r.intruder))

private fun controlFrame(magic: Int, data: IntArray, band: Band): Frame {
    val ch = Channel.of(band, Profile.NORMAL)
    val header = Protocol.headerBytes(magic, data.size, band, 0, Profile.NORMAL)
    val body = ReedSolomon.encode(data, CONTROL_PARITY)
    val symbols = Array(Protocol.PREAMBLE.size) { ch.preambleSymbol(it) } + Codec.toSymbols(header, ch.groups) + Codec.toSymbols(body, ch.groups)
    return Frame(header, body, symbols, symbols.size * ch.sym, ch)
}

/** Always the normal profile. [band] defaults to [RECEIPT_BAND] for a receipt and [ACK_BAND] for an ack. */
fun ackFrame(a: AckFrame, band: Band = if (a.receipt) RECEIPT_BAND else ACK_BAND): Frame = controlFrame(Protocol.MAGIC_ACK, ackBytes(a), band)
fun riskFrame(r: RiskFrame, band: Band): Frame = controlFrame(Protocol.MAGIC_RISK, riskBytes(r), band)

private fun crcOk(d: IntArray) = d.size > 2 && Checksums.crc16(d.copyOf(d.size - 2)) == ((d[d.size - 2] shl 8) or d[d.size - 1])

fun parseAck(data: IntArray): AckFrame? {
    if (data.size != ACK_DATA || !crcOk(data)) return null
    val kind = data[0] and 15
    val h = data[0] shr 4
    if (kind != 2 && kind != 3) return null
    val ack = AckFrame(kind == 3, hex(data.copyOfRange(1, 5)))
    if (h == 0) return ack
    val band = Band.entries.firstOrNull { channelIdOf(it) == (h and 7) - 1 } ?: return null
    return ack.copy(heard = AckHeard(band, if (h and 8 != 0) Profile.ROBUST else Profile.NORMAL))
}

fun parseRisk(data: IntArray): RiskFrame? {
    if (data.size != RISK_DATA || !crcOk(data)) return null
    val reason = RiskReason.entries.getOrNull(data[0] - 1) ?: return null
    val source = Role.entries.getOrNull(data[1] and 15) ?: return null
    return RiskFrame(reason, source, data[1] shr 4, hex(data.copyOfRange(2, 5)), hex(data.copyOfRange(5, 8)))
}

/** The incident a risk frame reports, without the hops: a relay is the same incident. */
fun riskKey(r: RiskFrame) = "${r.reason}/${r.source}/${r.own}/${r.intruder}"

/** Two bands share at least one tone. */
fun bandsOverlap(a: Band, b: Band): Boolean = a.bin < b.bin + b.groups * TONES && b.bin < a.bin + a.groups * TONES

/**
 * Bands a risk frame is looped on, in order: mid first (it does not overlap top or high), then the working band (unless it is mid or
 * audible). With [avoid] (the band of what is being reported), only the bands that do not overlap it; if none is left, top or mid; if
 * neither, the default list. Never audible.
 */
fun riskBands(work: Band, avoid: Band? = null): List<Band> {
    val base = if (work == Band.AUDIBLE || work == Band.MID) listOf(Band.MID) else listOf(Band.MID, work)
    if (avoid == null || avoid == Band.AUDIBLE) return base
    val clean = base.filter { !bandsOverlap(it, avoid) }
    if (clean.isNotEmpty()) return clean
    val spare = listOf(Band.MID, Band.TOP).filter { !bandsOverlap(it, avoid) }
    return spare.ifEmpty { base }
}
