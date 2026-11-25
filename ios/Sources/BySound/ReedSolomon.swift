// Reed-Solomon over GF(2^8) (polynomial 0x11D, generator 2, first root 0), with errors and erasures.
import Foundation

private enum GF {
    static let exp: [Int] = {
        var e = [Int](repeating: 0, count: 512)
        var x = 1
        for i in 0..<255 {
            e[i] = x
            x <<= 1
            if x & 0x100 != 0 { x ^= 0x11d }
        }
        for i in 255..<512 { e[i] = e[i - 255] }
        return e
    }()
    static let log: [Int] = {
        var l = [Int](repeating: 0, count: 256)
        var x = 1
        for i in 0..<255 {
            l[x] = i
            x <<= 1
            if x & 0x100 != 0 { x ^= 0x11d }
        }
        return l
    }()
    static func mul(_ a: Int, _ b: Int) -> Int { a == 0 || b == 0 ? 0 : exp[log[a] + log[b]] }
    static func div(_ a: Int, _ b: Int) -> Int { a == 0 ? 0 : exp[(log[a] + 255 - log[b]) % 255] }
    static func pow(_ a: Int, _ n: Int) -> Int {
        let e = (log[a] * n) % 255
        return exp[e < 0 ? e + 255 : e]
    }
    static func inverse(_ a: Int) -> Int { exp[255 - log[a]] }
}

private func polyScale(_ p: [Int], _ x: Int) -> [Int] { p.map { GF.mul($0, x) } }

private func polyAdd(_ p: [Int], _ q: [Int]) -> [Int] {
    var r = [Int](repeating: 0, count: max(p.count, q.count))
    for (i, v) in p.enumerated() { r[i + r.count - p.count] = v }
    for (i, v) in q.enumerated() { r[i + r.count - q.count] ^= v }
    return r
}

private func polyMul(_ p: [Int], _ q: [Int]) -> [Int] {
    var r = [Int](repeating: 0, count: p.count + q.count - 1)
    for j in 0..<q.count { for i in 0..<p.count { r[i + j] ^= GF.mul(p[i], q[j]) } }
    return r
}

private func polyEval(_ p: [Int], _ x: Int) -> Int {
    var y = p[0]
    for i in 1..<max(p.count, 1) { y = GF.mul(y, x) ^ p[i] }
    return y
}

private var generatorCache: [Int: [Int]] = [:]
private let generatorLock = NSLock()
private func generator(_ nsym: Int) -> [Int] {
    generatorLock.lock()
    defer { generatorLock.unlock() }
    if let g = generatorCache[nsym] { return g }
    var g = [1]
    for i in 0..<nsym { g = polyMul(g, [1, GF.pow(2, i)]) }
    generatorCache[nsym] = g
    return g
}

/// Appends `parity` Reed-Solomon check bytes to `data`.
public func rsEncode(_ data: [UInt8], parity nsym: Int) -> [UInt8] {
    precondition(data.count + nsym <= 255, "Reed-Solomon block too long")
    let gen = generator(nsym)
    var out = data.map(Int.init) + [Int](repeating: 0, count: nsym)
    for i in 0..<data.count {
        let coef = out[i]
        if coef != 0 { for j in 1..<gen.count { out[i + j] ^= GF.mul(gen[j], coef) } }
    }
    return data + out[data.count...].map { UInt8($0) }
}

private func syndromes(_ msg: [Int], _ nsym: Int) -> [Int] {
    var s = [Int](repeating: 0, count: nsym + 1)
    for i in 0..<nsym { s[i + 1] = polyEval(msg, GF.pow(2, i)) }
    return s
}

private func forneySyndromes(_ synd: [Int], _ erasures: [Int], _ n: Int) -> [Int] {
    var f = Array(synd.dropFirst())
    for p in erasures {
        let x = GF.pow(2, n - 1 - p)
        for j in 0..<max(f.count - 1, 0) { f[j] = GF.mul(f[j], x) ^ f[j + 1] }
    }
    return f
}

private func errorLocator(_ synd: [Int], _ nsym: Int, erasures: Int = 0) -> [Int]? {
    var errLoc = [1]
    var oldLoc = [1]
    let shift = synd.count - nsym
    for i in 0..<max(nsym - erasures, 0) {
        let k = i + shift
        var delta = synd[k]
        if errLoc.count > 1 { for j in 1..<errLoc.count { delta ^= GF.mul(errLoc[errLoc.count - 1 - j], synd[k - j]) } }
        oldLoc.append(0)
        if delta != 0 {
            if oldLoc.count > errLoc.count {
                let newLoc = polyScale(oldLoc, delta)
                oldLoc = polyScale(errLoc, GF.inverse(delta))
                errLoc = newLoc
            }
            errLoc = polyAdd(errLoc, polyScale(oldLoc, delta))
        }
    }
    while !errLoc.isEmpty && errLoc[0] == 0 { errLoc.removeFirst() }
    return (errLoc.count - 1) * 2 + erasures > nsym ? nil : errLoc
}

private func findErrors(_ reversed: [Int], _ n: Int) -> [Int]? {
    var pos: [Int] = []
    for i in 0..<n where polyEval(reversed, GF.pow(2, i)) == 0 { pos.append(n - 1 - i) }
    return pos.count == reversed.count - 1 ? pos : nil
}

private func polyDivRemainder(_ dividend: [Int], _ divisor: [Int]) -> [Int] {
    var out = dividend
    let steps = dividend.count - (divisor.count - 1)
    if steps > 0 {
        for i in 0..<steps {
            let coef = out[i]
            if coef != 0 {
                for j in 1..<divisor.count where divisor[j] != 0 { out[i + j] ^= GF.mul(divisor[j], coef) }
            }
        }
    }
    return Array(out[(out.count - (divisor.count - 1))...])
}

private func correctErrata(_ msg: [Int], _ synd: [Int], _ errPos: [Int]) -> [Int] {
    let coefPos = errPos.map { msg.count - 1 - $0 }
    var errLoc = [1]
    for p in coefPos { errLoc = polyMul(errLoc, polyAdd([1], [GF.pow(2, p), 0])) }
    var divisor = [Int](repeating: 0, count: errLoc.count + 1)
    divisor[0] = 1
    let errEval = Array(polyDivRemainder(polyMul(Array(synd.reversed()), errLoc), divisor).reversed())
    let x = coefPos.map { GF.pow(2, -(255 - $0)) }
    var e = [Int](repeating: 0, count: msg.count)
    for i in 0..<x.count {
        let xiInv = GF.inverse(x[i])
        var prime = 1
        for j in 0..<x.count where j != i { prime = GF.mul(prime, 1 ^ GF.mul(xiInv, x[j])) }
        let y = GF.mul(x[i], polyEval(Array(errEval.reversed()), xiInv))
        if prime == 0 { return msg }
        e[errPos[i]] = GF.div(y, prime)
    }
    return msg.enumerated().map { $0.element ^ e[$0.offset] }
}

/// Corrected data bytes and the number of bytes that changed, or `nil` if the codeword cannot be repaired.
/// With `e` errors and `f` erasures it corrects when `2e + f <= parity`.
public func rsDecode(_ codeword: [UInt8], parity nsym: Int, erasures: [Int] = []) -> (data: [UInt8], corrected: Int)? {
    let cw = codeword.map(Int.init)
    let synd = syndromes(cw, nsym)
    let dataSize = cw.count - nsym
    if synd.allSatisfy({ $0 == 0 }) { return (Array(codeword[..<dataSize]), 0) }
    if erasures.count > nsym { return nil }
    var errPos: [Int]
    if erasures.isEmpty {
        guard let loc = errorLocator(synd, nsym), let found = findErrors(Array(loc.reversed()), cw.count) else { return nil }
        errPos = found
    } else {
        guard let loc = errorLocator(forneySyndromes(synd, erasures, cw.count), nsym, erasures: erasures.count),
              let found = findErrors(Array(loc.reversed()), cw.count) else { return nil }
        errPos = erasures + found.filter { !erasures.contains($0) }
    }
    let fixed = correctErrata(cw, synd, errPos)
    if !syndromes(fixed, nsym).allSatisfy({ $0 == 0 }) { return nil }
    var changed = 0
    for i in 0..<fixed.count where fixed[i] != cw[i] { changed += 1 }
    return (fixed[..<dataSize].map { UInt8($0) }, changed)
}
