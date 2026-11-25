// Soft-decision helpers: each nibble is a vector of 16 probabilities.
import Foundation

typealias Soft = [[Double]]
let uniformNibble = [Double](repeating: 1.0 / 16, count: 16)

func hard(_ p: [Double]) -> Int {
    var best = 0
    for v in 1..<p.count where p[v] > p[best] { best = v }
    return best
}

/// Gap between the best and the second best probability.
func margin(_ p: [Double]) -> Double {
    var a = -1.0, b = -1.0
    for v in p {
        if v > a { b = a; a = v } else if v > b { b = v }
    }
    return a - b
}

func hardBytes(_ soft: ArraySlice<[Double]>) -> [UInt8] {
    var out: [UInt8] = []
    var i = soft.startIndex
    while i + 1 < soft.endIndex {
        out.append(UInt8((hard(soft[i]) << 4) | hard(soft[i + 1])))
        i += 2
    }
    return out
}
func hardBytes(_ soft: Soft) -> [UInt8] { hardBytes(soft[...]) }

/// Probabilities added across readings.
func summed(_ readings: [Soft]) -> Soft {
    var out = readings[0]
    for r in readings.dropFirst() { for i in 0..<out.count { for v in 0..<16 { out[i][v] += r[i][v] } } }
    return out
}

/// For each nibble, the reading that is surest about it.
func surest(_ readings: [Soft]) -> Soft {
    (0..<readings[0].count).map { i in
        var best = readings[0][i]
        for r in readings.dropFirst() where margin(r[i]) > margin(best) { best = r[i] }
        return best
    }
}

/// Byte positions ordered from least to most sure (ties keep byte order).
func leastSureBytes(_ soft: Soft, _ bytes: Int) -> [Int] {
    let sure = soft.map(margin)
    let byte = { (i: Int) in min(sure[2 * i], sure[2 * i + 1]) }
    return (0..<bytes).sorted { byte($0) != byte($1) ? byte($0) < byte($1) : $0 < $1 }
}
