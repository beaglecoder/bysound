package io.github.beaglecoder.bysound.modem

import io.github.beaglecoder.bysound.modem.Protocol.ACK_BODY
import io.github.beaglecoder.bysound.modem.Protocol.ACK_LENGTH
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_BYTES
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_HEADER
import io.github.beaglecoder.bysound.modem.Protocol.BLOCK_PARITY
import io.github.beaglecoder.bysound.modem.Protocol.GAP_SECONDS
import io.github.beaglecoder.bysound.modem.Protocol.HEADER_BYTES
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_ACK
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_PAYLOAD
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_REFERENCE
import io.github.beaglecoder.bysound.modem.Protocol.MAGIC_RISK
import io.github.beaglecoder.bysound.modem.Protocol.MAX_BLOCKS
import io.github.beaglecoder.bysound.modem.Protocol.PREAMBLE
import io.github.beaglecoder.bysound.modem.Protocol.PREAMBLE_THRESHOLD
import io.github.beaglecoder.bysound.modem.Protocol.REF_BODY
import io.github.beaglecoder.bysound.modem.Protocol.REF_DATA
import io.github.beaglecoder.bysound.modem.Protocol.RISK_BODY
import io.github.beaglecoder.bysound.modem.Protocol.RISK_LENGTH
import io.github.beaglecoder.bysound.modem.Protocol.VERSION

private const val HOP_HISTORY = 160
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
private val BLOCK_ERASURE_TRIES = intArrayOf(0, 2, 4)

internal enum class State { SEARCH, HEADER, BODY }

private class HeaderCandidate(val magic: Int, val length: Int, val nibbles: IntArray)

private fun nibblesOf(bytes: IntArray) = IntArray(bytes.size * 2) { if (it % 2 == 0) bytes[it / 2] shr 4 else bytes[it / 2] and 15 }

/** Every header a channel can see in a profile (the CRC covers the profile). Non-payload types are recognised only to be skipped. */
private fun headerCandidates(ch: Channel): List<HeaderCandidate> {
    fun head(magic: Int, n: Int, set: Int = 0) = HeaderCandidate(magic, n, nibblesOf(Protocol.headerBytes(magic, n, ch.band, set, ch.profile)))
    val out = arrayListOf(head(MAGIC_REFERENCE, REF_DATA, 0), head(MAGIC_REFERENCE, REF_DATA, 1), head(MAGIC_RISK, RISK_LENGTH), head(MAGIC_ACK, ACK_LENGTH))
    for (t in 1..MAX_BLOCKS) out.add(head(MAGIC_PAYLOAD, t))
    return out
}

/**
 * Reads one band: searches the preamble, reads the header and the blocks of the body and combines repeated laps. Payload frames
 * only; reference, risk and ack frames are recognised from their header and skipped.
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
        val at = plateau()
        if (at + start(PREAMBLE.size) < minEnd) {
            // A data pattern that looks like the preamble gives a lone peak: two senders give a high plateau.
            if (hops >= OVERLAP_MIN_HOPS && best >= OVERLAP_MIN_SCORE) pool.clash()
            return false
        }
        if (!isBlocks) return false
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
        headerSure = agree == n
        bodyReset()
        if (magic != MAGIC_PAYLOAD) {
            // Reference, risk and ack frames are not text: skip their body.
            val body = when (magic) { MAGIC_ACK -> ACK_BODY; MAGIC_RISK -> RISK_BODY; else -> REF_BODY }
            resetSearch()
            searchFrom = frameStart + start(firstBody + symbolsFor(body))
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
        if (blocks.total > 0 && blocks.channel == ch.band && blocks.lastAt >= horizon) {
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
            if (d != null && d.block.index == dataLength - 1 && d.block.total == dataLength) return finishBlocks(d, lastStart + length)
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
                if (d != null && d.block.index == index && d.block.total == dataLength && d.block.chunk.size == 48) return d
            }
        }
        return null
    }

    /** The last block does not say its length: every length is tried with the readings summed over several laps. */
    private fun closeLast(readings: List<Soft>): DecodedBlock? {
        for (candidate in listOf(summed(readings), surest(readings))) {
            val bytes = hardBytes(candidate)
            for (length in BLOCK_HEADER + BLOCK_PARITY + 1..BLOCK_BYTES) {
                val d = Codec.decodeBlock(bytes.copyOfRange(0, length))
                if (d != null && d.block.index == dataLength - 1 && d.block.total == dataLength) return d
            }
        }
        return null
    }

    private fun finishBlocks(last: DecodedBlock?, usedBytes: Int) {
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
            corrected += last.corrected
        } else if (!a.chunks.containsKey(n - 1) && bodySoft.size > 2 * lastStart) {
            val soft = ArrayList(bodySoft.clip(2 * lastStart, 2 * (lastStart + BLOCK_BYTES)))
            while (soft.size < BLOCK_BYTES * 2) soft.add(UNIFORM)
            a.pendingLast = (a.pendingLast + listOf<Soft>(soft)).takeLast(COMBINE_MAX)
            val d = if (a.pendingLast.size > 1) closeLast(a.pendingLast) else null
            if (d != null) {
                a.chunks[n - 1] = d.block.chunk
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
            a.reset()
            if (text != null) return deliver(Heard(text, ch.band, ch.profile, corrected, loops), end)
            pool.failures++
            return
        }
        if (gained == 0) pool.failures++
    }

    /** Not delivered if a frame of another channel started while this one was sounding. */
    private fun deliver(h: Heard, end: Long) {
        val preambleLen = start(PREAMBLE.size)
        val begin = frameStart
        if (pool.starts.any { it.band != ch.band && it.at > begin && it.at + preambleLen < end }) pool.clash()
        else events.onFrame(h.copy(score = frameScore))
    }

    private fun currentProgress(): Double {
        if (state == State.SEARCH) return -1.0
        fun clamp(x: Double) = minOf(maxOf(x, 0.0), 1.0)
        val headerEnd = symbolEnd(firstBody - 1)
        val total = r.total
        if (state == State.HEADER) return HEADER_SHARE * clamp((total - frameStart).toDouble() / (headerEnd - frameStart))
        val known = pool.blocks.knownBytes
        val end = symbolEnd(firstBody + symbolsFor(if (known != 0) known else bodyBytes) - 1)
        return HEADER_SHARE + (1 - HEADER_SHARE) * clamp((total - headerEnd).toDouble() / (end - headerEnd))
    }
}
