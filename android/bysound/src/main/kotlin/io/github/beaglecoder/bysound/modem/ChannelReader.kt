package io.github.beaglecoder.bysound.modem

import io.github.beaglecoder.bysound.modem.Protocol.ACK_BODY
import io.github.beaglecoder.bysound.modem.Protocol.ACK_LENGTH
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_BYTES
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_HEADER
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_PARITY
import io.github.beaglecoder.bysound.modem.Protocol.HEADER_BYTES
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_ACK
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_PAYLOAD
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_REFERENCE
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_RISK
import io.github.beaglecoder.bysound.modem.Protocol.MOTHER_BYTES
import io.github.beaglecoder.bysound.modem.Protocol.MOTHER_PARITY
import io.github.beaglecoder.bysound.modem.Protocol.REF_PARITY
import io.github.beaglecoder.bysound.modem.Protocol.MAX_BLOCKS
import io.github.beaglecoder.bysound.modem.Protocol.PREAMBLE
import io.github.beaglecoder.bysound.modem.Protocol.PREAMBLE_THRESHOLD
import io.github.beaglecoder.bysound.modem.Protocol.REF_BODY
import io.github.beaglecoder.bysound.modem.Protocol.REF_DATA
import io.github.beaglecoder.bysound.modem.Protocol.RISK_BODY
import io.github.beaglecoder.bysound.modem.Protocol.RISK_LENGTH
import io.github.beaglecoder.bysound.modem.Protocol.VERSION

private const val FLOOR_SECONDS = 0.5
private const val FLOOR_WARM_SECONDS = 0.3
private const val FLOOR_WARM_TAU = 0.08
private const val FLOOR_OUTLIER = 4.0
private const val HEADER_SHARE = 0.12
private const val HEADER_MIN_AGREEMENT = 5
private const val HEADER_SURE_AGREEMENT = 7
private const val COMBINE_MAX = 6
private const val COMBINE_SECONDS = 30
private const val OVERLAP_FROM_SECONDS = 0.3
private const val OVERLAP_MIN_HOPS = 2
private const val OVERLAP_MIN_SCORE = 0.45
/** A preamble past the frame's minimum length is taken as the next lap only if it scores at least this share of the frame's. */
private const val REPEAT_SCORE_SHARE = 0.7
private val BLOCK_ERASURE_TRIES = intArrayOf(0, 2, 4)
private val CONTROL_ERASURE_TRIES = intArrayOf(0, 2, 4)
private val REFERENCE_ERASURE_TRIES = intArrayOf(0, 4, 8, 12)
private const val CONTROL_PARITY = 8
private const val REBUILD_TRIES_MAX = 300

/** SMALL: the body of a reference, ack or risk frame (a few bytes, read in one go). */
internal enum class State { SEARCH, HEADER, BODY, SMALL }

private class HeaderCandidate(val magic: Int, val length: Int, val set: Int, val nibbles: IntArray)

private fun nibblesOf(bytes: IntArray) = IntArray(bytes.size * 2) { if (it % 2 == 0) bytes[it / 2] shr 4 else bytes[it / 2] and 15 }

/** Every header a channel can see in a profile (the CRC covers the profile). */
private fun headerCandidates(ch: Channel): List<HeaderCandidate> {
    fun head(magic: Int, n: Int, set: Int = 0) = HeaderCandidate(magic, n, set, nibblesOf(Protocol.headerBytes(magic, n, ch.band, set, ch.profile)))
    val out = arrayListOf(head(MAGIC_REFERENCE, REF_DATA, 0), head(MAGIC_REFERENCE, REF_DATA, 1), head(MAGIC_RISK, RISK_LENGTH), head(MAGIC_ACK, ACK_LENGTH))
    for (t in 1..MAX_BLOCKS) out.add(head(MAGIC_PAYLOAD, t))
    return out
}

/**
 * Reads one band: searches the preamble, reads the header and the blocks of the body and combines repeated laps. Payload frames
 * deliver text; reference frames deliver a fingerprint (several repetitions and both parity sets are combined); ack and risk frames
 * are Reed-Solomon decoded, their CRC-16 checked, and reported as bytes through [ReceiverEvents.onControl].
 */
internal class ChannelReader(
    private val r: Receiver,
    private val ch: Channel,
    private val bank: ToneBank,
    private val pool: Pool,
    private val events: ReceiverEvents,
) {
    var state = State.SEARCH
    var progress = -1.0
    var nibbleConfidence: DoubleArray = DoubleArray(0)

    private val tones = ch.groups * TONES
    private val coeffs = DoubleArray(tones) { 2 * Math.cos(2 * Math.PI * ch.toneHz(it / TONES, it % TONES) / r.rate) }
    private val floor = DoubleArray(tones)
    private val scratch = DoubleArray(tones)
    private val windowOut = DoubleArray(tones)
    private val symbolW = DoubleArray(tones)
    private val groupD = DoubleArray(TONES)

    /** Tail of the echo (see [softSymbol]); null at the start of a reading. */
    private var previous: DoubleArray? = null
    private var echo = 0.0
    private val candStarts = ArrayList<Long>()
    private val candScores = ArrayList<Double>()
    private var frameScore = 0.0
    private var frameStart = 0L
    private var searchFrom = 0L
    private var magic = 0
    private var set = 0
    /** The header matched entirely. With corrected errors the length may be false (see [tryBlocks]). */
    private var headerSure = true
    private var dataLength = 0
    private var bodySoft = ArrayList<DoubleArray>()
    private var bodyRead = 0
    private var lastTried = 0
    private val preamble = Array(PREAMBLE.size) { ch.preambleSymbol(it) }
    private val candidates = headerCandidates(ch)
    private val headerSymbols = (HEADER_BYTES * 2 + ch.groups - 1) / ch.groups
    private val endOffset = ch.windowStart + r.windowSeconds
    private val preambleSpan = (PREAMBLE.size - 1) * ch.sym + endOffset

    private val offset get() = ch.bin - bank.baseBin
    private val isBlocks get() = magic == MAGIC_PAYLOAD
    private val bodyBytes get() = (dataLength - 1) * BLOCK_BYTES + BLOCK_HEADER + 1 + BLOCK_PARITY
    private val bodyBytesMax get() = dataLength * BLOCK_BYTES
    /** Body bytes of a reference, ack or risk frame. Acks and risk frames are not counted in clash detection. */
    private val smallBody get() = when (magic) { MAGIC_ACK -> ACK_BODY; MAGIC_RISK -> RISK_BODY; else -> REF_BODY }
    private fun symbolsFor(bytes: Int) = (bytes * 2 + ch.groups - 1) / ch.groups
    private fun start(k: Int) = ch.symbolStart(k, r.rate)
    private fun symbolEnd(k: Int): Long = frameStart + start(k) + samples(endOffset, r.rate)
    private val firstBody get() = PREAMBLE.size + headerSymbols
    private fun hopEnd(i: Int): Long = r.winSize.toLong() + i.toLong() * r.hopSize
    private fun energy(hop: Int, t: Int) = bank.at(hop)[offset + t]

    /** Blocks of the text in the frame being read; 0 if none. */
    val blocksInFrame get() = if (isBlocks) dataLength else 0

    fun onHop(index: Int) {
        if (state == State.SEARCH) updateFloor(index)
        when (state) {
            State.SEARCH -> {
                if (nibbleConfidence.isNotEmpty()) nibbleConfidence = DoubleArray(0)
                search(index)
            }
            State.HEADER -> tryHeader()
            State.BODY -> if (!searchInBody(index)) tryBody()
            State.SMALL -> trySmall()
        }
        progress = currentProgress()
    }

    private fun updateFloor(hop: Int) {
        val f = floor
        if (hop == 0) {
            val sorted = DoubleArray(tones) { energy(hop, it) }.also { it.sort() }
            f.fill(sorted[tones shr 1])
            return
        }
        val warm = (hop + 1) * r.hopSeconds < FLOOR_WARM_SECONDS
        val a = r.hopSeconds / (if (warm) FLOOR_WARM_TAU else FLOOR_SECONDS)
        for (t in 0 until tones) {
            val x = energy(hop, t)
            val v = f[t]
            f[t] = v + a * ((if (v > 0) minOf(x, FLOOR_OUTLIER * v) else x) - v)
        }
    }

    private fun whitenMinimum(): Double {
        var s = 0.0
        for (t in 0 until tones) s += floor[t]
        return maxOf(s / tones * 0.05, 1e-20)
    }

    /** Preamble match ending at hop [index]: the frame start and its score, or null if there is not enough history. */
    private fun preambleScore(index: Int): Pair<Long, Double>? {
        val rate = r.rate
        val start = hopEnd(index) - roundHalfUpLong(preambleSpan * rate)
        if (start < 0) return null
        val minimum = whitenMinimum()
        val w = scratch
        var score = 0.0
        for (k in PREAMBLE.indices) {
            val end = start + roundHalfUpLong((k * ch.sym + endOffset) * rate)
            val hop = roundHalfUpLong((end - r.winSize).toDouble() / r.hopSize)
            if (hop < 0 || hop > index || index - hop >= HOP_HISTORY) return null
            val e = bank.at(hop.toInt())
            val off = offset
            for (t in 0 until tones) w[t] = e[off + t] / maxOf(floor[t], minimum)
            for (g in 0 until ch.groups) {
                val base = g * TONES
                var sum = 0.0
                for (v in 0 until TONES) sum += w[base + v]
                if (sum > 1e-12) score += w[base + preamble[k][g]] / sum
            }
        }
        return Pair(start, score / (PREAMBLE.size * ch.groups))
    }

    /** Centre of the plateau of candidates. */
    private fun plateau(): Long {
        val best = candScores.max()
        frameScore = best
        var sum = 0.0
        var n = 0
        for (i in candStarts.indices) if (candScores[i] >= best * 0.97) { sum += candStarts[i]; n++ }
        candStarts.clear()
        candScores.clear()
        return roundHalfUpLong(sum / n)
    }

    private fun search(index: Int) {
        val (start, score) = preambleScore(index) ?: return
        if (start < searchFrom) return
        if (score >= PREAMBLE_THRESHOLD && candStarts.size < 64) {
            candStarts.add(start)
            candScores.add(score)
            return
        }
        if (candStarts.isNotEmpty()) enterFrame(plateau())
    }

    private fun enterFrame(at: Long) {
        frameStart = at
        state = State.HEADER
        measureEcho()
        tryHeader()
    }

    /** Inside a body the preamble is searched too: on top of it is another sender, and after the minimum it is the next lap. */
    private fun searchInBody(index: Int): Boolean {
        val found = preambleScore(index) ?: return false
        val (start, score) = found
        val rate = r.rate
        val minEnd = frameStart + start(firstBody + symbolsFor(bodyBytes))
        if (score >= PREAMBLE_THRESHOLD && start >= frameStart + OVERLAP_FROM_SECONDS * rate && candStarts.size < 64) {
            candStarts.add(start)
            candScores.add(score)
            return false
        }
        if (candStarts.isEmpty()) return false
        val hops = candStarts.size
        val best = candScores.max()
        val own = frameScore
        val at = plateau()
        frameScore = own // plateau() stores the candidate's score; the frame being read keeps its own
        if (at + start(PREAMBLE.size) < minEnd) {
            // A data pattern that looks like the preamble gives a lone peak: two senders give a high plateau.
            if (hops >= OVERLAP_MIN_HOPS && best >= OVERLAP_MIN_SCORE) pool.clash()
            return false
        }
        if (!isBlocks) return false
        // The next lap comes from the same sender, so its preamble scores about as well as this frame's. Body data that happens to
        // match the preamble in a few nibbles scores only that share of it (6 of 16 nibbles: 0.375 in a clean channel, over the
        // threshold) and must not cut off the read: the last block would be lost.
        if (best < REPEAT_SCORE_SHARE * own) return false
        tryBody()
        if (state == State.BODY) finishBlocks(null, bodySoft.size shr 1)
        enterFrame(at)
        return true
    }

    /** Energies of symbol [k] of the current frame, normalised by the noise floor, into [out]. */
    private fun symbolEnergies(k: Int, out: DoubleArray): DoubleArray {
        r.window(frameStart + start(k) + samples(ch.windowStart, r.rate))
        toneEnergies(r, r.windowed, ch.bin, coeffs, windowOut)
        val minimum = whitenMinimum()
        for (t in 0 until tones) out[t] = windowOut[t] / maxOf(floor[t], minimum)
        return out
    }

    private fun measureEcho() {
        val ratios = ArrayList<Double>()
        var last = symbolEnergies(0, DoubleArray(tones))
        var cur = DoubleArray(tones)
        for (k in 1 until preamble.size) {
            symbolEnergies(k, cur)
            for (g in 0 until ch.groups) {
                val t = g * TONES + preamble[k - 1][g]
                if (last[t] > 1e-12) ratios.add(cur[t] / last[t])
            }
            val tmp = last; last = cur; cur = tmp
        }
        ratios.sort()
        echo = if (ratios.isNotEmpty()) minOf(maxOf(ratios[ratios.size shr 1], 0.0), 0.8) else 0.0
        previous = last
    }

    private fun softSymbol(k: Int): List<DoubleArray> {
        val w = symbolEnergies(k, symbolW)
        // The tail is updated in place: each group uses the previous tail first and only then overwrites it.
        val prev = previous ?: DoubleArray(tones).also { previous = it }
        val d = groupD
        val out = ArrayList<DoubleArray>(ch.groups)
        for (g in 0 until ch.groups) {
            val base = g * TONES
            var sum = 0.0
            for (v in 0 until TONES) {
                d[v] = maxOf(w[base + v] - echo * prev[base + v], 0.0)
                sum += d[v]
            }
            for (v in 0 until TONES) prev[base + v] = w[base + v] + echo * prev[base + v]
            if (sum <= 1e-12) out.add(UNIFORM.copyOf())
            else out.add(DoubleArray(TONES) { d[it] / sum })
        }
        return out
    }

    private fun resetSearch(keepConfidence: Boolean = false) {
        state = State.SEARCH
        if (!keepConfidence) nibbleConfidence = DoubleArray(0)
        searchFrom = frameStart + start(1)
        candStarts.clear()
        candScores.clear()
    }

    private fun bodyReset() {
        bodySoft = ArrayList()
        bodyRead = 0
        nibbleConfidence = DoubleArray(0)
        lastTried = BLOCK_HEADER + BLOCK_PARITY
    }

    private fun tryHeader() {
        val first = PREAMBLE.size
        if (r.total < symbolEnd(first + headerSymbols - 1)) return
        val soft = ArrayList<DoubleArray>()
        for (i in 0 until headerSymbols) soft.addAll(softSymbol(first + i))
        val n = HEADER_BYTES * 2
        var best = -1.0
        var choice = candidates[0]
        for (c in candidates) {
            var s = 0.0
            for (i in 0 until n) s += soft[i][c.nibbles[i]]
            if (s > best) { best = s; choice = c }
        }
        var agree = 0
        for (i in 0 until n) if (hard(soft[i]) == choice.nibbles[i]) agree++
        if (foreignHeader(soft)) return resetSearch()
        if (agree < HEADER_MIN_AGREEMENT) return unreadableHeader()
        magic = choice.magic
        dataLength = choice.length
        set = choice.set
        headerSure = agree == n
        bodyReset()
        if (magic != MAGIC_PAYLOAD) {
            if (magic == MAGIC_REFERENCE && agree >= HEADER_SURE_AGREEMENT) {
                events.onHeader(ch.band)
                pool.noteStart(ch.band, frameStart, start(firstBody + symbolsFor(smallBody)).toLong())
            }
            state = State.SMALL
            trySmall()
            return
        }
        if (agree >= HEADER_SURE_AGREEMENT) {
            events.onHeader(ch.band)
            pool.noteStart(ch.band, frameStart, start(firstBody + symbolsFor(bodyBytes)).toLong())
        }
        state = State.BODY
        tryBody()
    }

    /** The header closes its CRC-8 with another channel: it is another sender, not read as ours. */
    private fun foreignHeader(soft: List<DoubleArray>): Boolean {
        val b = hardBytes(soft.subList(0, HEADER_BYTES * 2))
        val (magic, length, crc, version) = b.toList()
        if (version shr 4 != VERSION || ((version shr 1) and 7) == ch.band.channelId) return false
        return crc == Protocol.crc8(intArrayOf(magic, length, version, if (ch.profile == Profile.ROBUST) 1 else 0))
    }

    /** Unreadable header: if this channel was reading a recent repetition, its header holds for this one. */
    private fun unreadableHeader() {
        val horizon = frameStart - COMBINE_SECONDS.toLong() * r.rate
        val blocks = pool.blocks
        val last = pool.readings.lastOrNull()
        if (last != null && last.band == ch.band && last.at >= horizon) {
            magic = last.magic
            dataLength = last.length
            set = last.set xor 1
            headerSure = true
            bodyReset()
            state = State.SMALL
            trySmall()
            return
        } else if (blocks.total > 0 && blocks.channel == ch.band && blocks.lastAt >= horizon) {
            magic = MAGIC_PAYLOAD
            dataLength = blocks.total
        } else {
            pool.headerFails.add(Pool.HeaderFail(frameStart, r.total))
            resetSearch()
            return
        }
        headerSure = true
        bodyReset()
        state = State.BODY
        tryBody()
    }

    private fun tryBody() = tryBlocks()

    /** Reads the whole body of a reference, ack or risk frame, then decodes it. */
    private fun trySmall() {
        val first = firstBody
        val count = symbolsFor(smallBody)
        while (bodyRead < count && r.total >= symbolEnd(first + bodyRead)) {
            bodySoft.addAll(softSymbol(first + bodyRead))
            bodyRead++
        }
        if (bodyRead < count) return
        val end = frameStart + start(first + count)
        val reading: Soft = bodySoft.clip(0, smallBody * 2).toList()
        resetSearch()
        searchFrom = end
        if (magic == MAGIC_REFERENCE) readReference(reading, end) else readControl(reading)
    }

    /** Ack or risk: Reed-Solomon with 0, 2 and 4 erasures on the least sure bytes, then the CRC-16. */
    private fun readControl(reading: Soft) {
        val bytes = hardBytes(reading)
        val doubtful = leastSureBytes(reading, bytes.size)
        for (n in CONTROL_ERASURE_TRIES) {
            val d = ReedSolomon.decode(bytes, CONTROL_PARITY, doubtful.take(n).sorted().toIntArray()) ?: continue
            val data = d.data
            if (data.size != dataLength || data.size < 3) continue
            if (Checksums.crc16(data.copyOf(data.size - 2)) != ((data[data.size - 2] shl 8) or data[data.size - 1])) continue
            events.onControl(magic, data, ch.band)
            return
        }
    }

    private class Decoded(val fingerprint: IntArray, val corrected: Int)

    /** Positions of a reference body (data + the parity of its set) in the 46 bytes of the mother code. */
    private fun motherLayout(set: Int, soft: Soft): Soft {
        val out = ArrayList<DoubleArray>(MOTHER_BYTES * 2)
        repeat(MOTHER_BYTES * 2) { out.add(UNIFORM) }
        val dataNibbles = REF_DATA * 2
        for (i in 0 until minOf(soft.size, dataNibbles + REF_PARITY * 2)) out[if (i < dataNibbles) i else i + (set and 1) * REF_PARITY * 2] = soft[i]
        return out
    }

    private fun disagreement(layouts: List<Soft>): List<Int> {
        if (layouts.size < 2) return emptyList()
        val bytes = layouts.map { hardBytes(it.clip(0, 2 * REF_DATA)) }
        return (0 until REF_DATA).filter { i -> bytes.map { it[i] }.toSet().size > 1 }
    }

    private fun decodeMotherSoft(soft: Soft, have: BooleanArray, disagree: List<Int>): Decoded? {
        val bytes = hardBytes(soft)
        val m = soft.map(::margin)
        fun sure(i: Int) = minOf(m[2 * i], m[2 * i + 1])
        val data = bytes.copyOfRange(0, REF_DATA)
        val parity = Array<IntArray?>(2) { set -> if (have[set]) IntArray(REF_PARITY) { bytes[Codec.parityRange(set).first + it] } else null }
        val dataPositions = (0 until REF_DATA).toList()
        class Variant(val parity: Array<IntArray?>, val base: List<Int>, val known: List<Int>)
        val variants = ArrayList<Variant>()
        for (set in 0..1) {
            if (!have[set]) continue
            val only = arrayOfNulls<IntArray>(2)
            only[set] = parity[set]
            variants.add(Variant(only, emptyList(), dataPositions + Codec.parityRange(set).toList()))
        }
        if (have[0] && have[1]) variants.add(Variant(parity, disagree, (0 until MOTHER_BYTES).toList()))
        for (v in variants) {
            val leastSure = v.known.filter { it !in v.base }.sortedBy { sure(it) }
            for (f in REFERENCE_ERASURE_TRIES) {
                val d = Codec.decodeMother(data, v.parity, v.base + leastSure.take(f)) ?: continue
                return Decoded(d.fingerprint, d.corrected)
            }
        }
        return null
    }

    private fun decodeReference(readings: List<Pair<Int, Soft>>): Decoded? {
        val layouts = readings.map { motherLayout(it.first, it.second) }
        val have = booleanArrayOf(readings.any { it.first and 1 == 0 }, readings.any { it.first and 1 == 1 })
        return decodeMotherSoft(if (layouts.size == 1) layouts[0] else summed(layouts), have, disagreement(layouts))
    }

    /** Current reading plus subsets of the earlier ones (the most readings first), summed or taking the surest digit. */
    private fun rebuildReference(current: Pair<Int, Soft>, earlier: List<Pair<Int, Soft>>): Pair<Decoded, Int>? {
        if (earlier.isEmpty()) return null
        val masks = (1 until (1 shl earlier.size)).sortedByDescending { Integer.bitCount(it) }
        var tries = 0
        for (mask in masks) {
            val readings = listOf(current) + earlier.filterIndexed { i, _ -> (mask shr i) and 1 == 1 }
            val layouts = readings.map { motherLayout(it.first, it.second) }
            val have = booleanArrayOf(readings.any { it.first and 1 == 0 }, readings.any { it.first and 1 == 1 })
            val disagree = disagreement(layouts)
            for (candidate in listOf(summed(layouts), surest(layouts))) {
                if (tries + REFERENCE_ERASURE_TRIES.size > REBUILD_TRIES_MAX) return null
                tries += REFERENCE_ERASURE_TRIES.size
                val d = decodeMotherSoft(candidate, have, disagree)
                if (d != null) return Pair(d, readings.size)
            }
        }
        return null
    }

    private fun readReference(reading: Soft, end: Long) {
        val single = decodeReference(listOf(Pair(set, reading)))
        if (single != null) return finishReference(single, 1, end)
        val horizon = frameStart - COMBINE_SECONDS.toLong() * r.rate
        pool.readings.removeAll { it.at < horizon }
        val same = pool.readings.filter { it.magic == magic && it.length == dataLength }
        pool.readings.add(Pool.Reading(magic, dataLength, set, frameStart, ch.band, reading))
        if (pool.readings.size > COMBINE_MAX * 3) pool.readings.removeAt(0)
        val earlier = same.takeLast(COMBINE_MAX - 1).map { Pair(it.set, it.soft) }
        val rebuilt = rebuildReference(Pair(set, reading), earlier)
        if (rebuilt != null) return finishReference(rebuilt.first, rebuilt.second, end)
        pool.failures++
    }

    private fun finishReference(d: Decoded, combined: Int, end: Long) {
        searchFrom = end
        pool.readings.clear()
        val fp = d.fingerprint.joinToString("") { "%02x".format(it) }
        deliver(end) { events.onReference(HeardReference(fp, ch.band, ch.profile, d.corrected, combined, frameScore)) }
    }

    private fun tryBlocks() {
        val first = firstBody
        val maxSymbols = symbolsFor(bodyBytesMax)
        var read = false
        while (bodyRead < maxSymbols && r.total >= symbolEnd(first + bodyRead)) {
            bodySoft.addAll(softSymbol(first + bodyRead))
            bodyRead++
            read = true
        }
        if (read) {
            val from = maxOf(0, bodySoft.size - BLOCK_BYTES * 2)
            nibbleConfidence = DoubleArray(bodySoft.size - from) { margin(bodySoft[from + it]) }
        }
        // A header with errors may announce a false length, and waiting for that frame leaves the channel deaf. If the first block
        // does not close either, it was not this frame: search again. The sender repeats, so a real frame is not lost.
        if (!headerSure && dataLength > 1 && bodySoft.size shr 1 >= BLOCK_BYTES) {
            val opening = Codec.decodeBlock(hardBytes(bodySoft.subList(0, 2 * BLOCK_BYTES)))
            if (opening == null || opening.block.index != 0 || opening.block.total != dataLength) {
                pool.failures++
                resetSearch()
                return
            }
            headerSure = true
        }
        val lastStart = (dataLength - 1) * BLOCK_BYTES
        val available = bodySoft.size shr 1
        for (length in lastTried + 1..minOf(available - lastStart, BLOCK_BYTES)) {
            lastTried = length
            val bytes = hardBytes(bodySoft.clip(2 * lastStart, 2 * (lastStart + length)))
            val d = Codec.decodeBlock(bytes)
            if (d == null || d.block.index != dataLength - 1 || d.block.total != dataLength) continue
            val check = lastBlockCheck(d.block.chunk)
            if (check != false) return finishBlocks(d, lastStart + length, lastUnchecked = check == null)
        }
        if (bodyRead == maxSymbols && lastTried >= BLOCK_BYTES) finishBlocks(null, bodyBytesMax)
    }

    private fun closeBlock(index: Int, soft: Soft, pending: List<Soft>): DecodedBlock? {
        val candidates = arrayListOf(soft)
        if (pending.isNotEmpty()) { candidates.add(summed(listOf(soft) + pending)); candidates.add(surest(listOf(soft) + pending)) }
        for (candidate in candidates) {
            val bytes = hardBytes(candidate)
            val leastSure = leastSureBytes(candidate, BLOCK_BYTES)
            for (f in BLOCK_ERASURE_TRIES) {
                val d = Codec.decodeBlock(bytes, leastSure.take(f).toIntArray())
                if (d != null && d.block.index == index && d.block.total == dataLength && d.block.chunk.size == Protocol.BLOCK_DATA) return d
            }
        }
        return null
    }

    /** The last block does not say its length: every length is tried with the readings summed over several laps. */
    private fun closeLast(readings: List<Soft>): Pair<DecodedBlock, Boolean>? {
        for (candidate in listOf(summed(readings), surest(readings))) {
            val bytes = hardBytes(candidate)
            for (length in BLOCK_HEADER + BLOCK_PARITY + 1..BLOCK_BYTES) {
                val d = Codec.decodeBlock(bytes.copyOfRange(0, length))
                if (d == null || d.block.index != dataLength - 1 || d.block.total != dataLength) continue
                val check = lastBlockCheck(d.block.chunk)
                if (check != false) return Pair(d, check == null)
            }
        }
        return null
    }

    /**
     * Whether a candidate for the last block gives a text whose CRC-32 checks out. Reed–Solomon alone cannot tell its length: if the
     * last parity byte is 0x00 the codeword without it is also a codeword (c(x) = x·c'(x)), so the block would close one byte short.
     * null: another block is not available yet, so it cannot be told here.
     */
    private fun lastBlockCheck(chunk: IntArray): Boolean? {
        val n = dataLength
        val a = pool.blocks
        val known = a.total == n && a.channel == ch.band && a.lastAt >= frameStart - COMBINE_SECONDS.toLong() * r.rate
        val chunks = ArrayList<IntArray>()
        for (i in 0 until n - 1) {
            var c = if (known) a.chunks[i] else null
            if (c == null && 2 * (i + 1) * BLOCK_BYTES <= bodySoft.size) {
                val soft = bodySoft.clip(2 * i * BLOCK_BYTES, 2 * (i + 1) * BLOCK_BYTES)
                c = closeBlock(i, soft, if (known) a.pending[i] ?: emptyList() else emptyList())?.block?.chunk
            }
            chunks.add(c ?: return null)
        }
        chunks.add(chunk)
        return Codec.assemble(chunks) != null
    }

    private fun finishBlocks(last: DecodedBlock?, usedBytes: Int, lastUnchecked: Boolean = false) {
        val a = pool.blocks
        val rate = r.rate
        val n = dataLength
        val end = frameStart + start(firstBody + symbolsFor(usedBytes))
        a.begin(n, frameStart, frameStart - COMBINE_SECONDS.toLong() * rate, ch.band)
        var gained = 0
        var corrected = 0
        val lastStart = (n - 1) * BLOCK_BYTES
        if (last != null) {
            if (!a.chunks.containsKey(last.block.index)) gained++
            a.chunks[last.block.index] = last.block.chunk
            a.knownBytes = usedBytes
            a.lastUnchecked = lastUnchecked
            corrected += last.corrected
        } else if (!a.chunks.containsKey(n - 1) && bodySoft.size > 2 * lastStart) {
            val soft = ArrayList(bodySoft.clip(2 * lastStart, 2 * (lastStart + BLOCK_BYTES)))
            while (soft.size < BLOCK_BYTES * 2) soft.add(UNIFORM)
            a.pendingLast = (a.pendingLast + listOf<Soft>(soft)).takeLast(COMBINE_MAX)
            val closed = if (a.pendingLast.size > 1) closeLast(a.pendingLast) else null
            if (closed != null) {
                val (d, unchecked) = closed
                a.chunks[n - 1] = d.block.chunk
                a.lastUnchecked = unchecked
                a.pendingLast = emptyList()
                gained++
                corrected += d.corrected
            }
        }
        for (i in 0 until n - 1) {
            if (a.chunks.containsKey(i)) continue
            if (2 * (i + 1) * BLOCK_BYTES > bodySoft.size) break
            val soft = bodySoft.clip(2 * i * BLOCK_BYTES, 2 * (i + 1) * BLOCK_BYTES)
            val d = closeBlock(i, soft, a.pending[i] ?: emptyList())
            if (d != null) {
                a.chunks[i] = d.block.chunk
                a.pending.remove(i)
                gained++
                corrected += d.corrected
            } else {
                a.pending[i] = ((a.pending[i] ?: emptyList()) + listOf<Soft>(soft)).takeLast(COMBINE_MAX - 1)
            }
        }
        a.loops++
        resetSearch()
        // Without the last block we do not know where the frame ended: the next lap may have started before the maximum.
        searchFrom = if (last != null) end else minOf(end, frameStart + start(firstBody + symbolsFor(bodyBytes)))
        if (gained > 0) events.onBlocks(a.chunks.size, n)
        if (a.chunks.size == n) {
            val text = Codec.assemble(List(n) { a.chunks[it]!! })
            val loops = a.loops
            if (text == null && a.lastUnchecked) {
                // The last block's length was a guess (it closed before the other blocks): only it is forgotten, and the next lap
                // closes it again, this time checked against the CRC-32.
                a.chunks.remove(n - 1)
                a.lastUnchecked = false
                pool.failures++
                return
            }
            a.reset()
            if (text != null) return deliver(end) { events.onFrame(Heard(text, ch.band, ch.profile, corrected, loops, frameScore)) }
            pool.failures++
            return
        }
        if (gained == 0) pool.failures++
    }

    /** Not delivered if a frame of another channel started while this one was sounding. */
    private fun deliver(end: Long, emit: () -> Unit) {
        val preambleLen = start(PREAMBLE.size)
        val begin = frameStart
        if (pool.starts.any { it.band != ch.band && it.at > begin && it.at + preambleLen < end }) pool.clash() else emit()
    }

    private fun currentProgress(): Double {
        if (state == State.SEARCH || state == State.SMALL) return -1.0
        fun clamp(x: Double) = minOf(maxOf(x, 0.0), 1.0)
        val headerEnd = symbolEnd(firstBody - 1)
        val total = r.total
        if (state == State.HEADER) return HEADER_SHARE * clamp((total - frameStart).toDouble() / (headerEnd - frameStart))
        val known = pool.blocks.knownBytes
        val end = symbolEnd(firstBody + symbolsFor(if (known != 0) known else bodyBytes) - 1)
        return HEADER_SHARE + (1 - HEADER_SHARE) * clamp((total - headerEnd).toDouble() / (end - headerEnd))
    }
}
