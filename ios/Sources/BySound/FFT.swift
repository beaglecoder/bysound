// Tone energies of one analysis window: a vDSP FFT when the tone grid falls on FFT bins (48 kHz), Goertzel otherwise (44.1 kHz).
import Accelerate
import Foundation

/// Real FFT of a power-of-two window. Not thread safe: one instance per receiver.
final class FFT {
    let n: Int
    private let log2n: vDSP_Length
    private let setup: FFTSetupD
    private let re: UnsafeMutablePointer<Double>
    private let im: UnsafeMutablePointer<Double>

    init?(size: Int) {
        guard size >= 4, size & (size - 1) == 0 else { return nil }
        n = size
        log2n = vDSP_Length(size.trailingZeroBitCount)
        guard let s = vDSP_create_fftsetupD(log2n, FFTRadix(kFFTRadix2)) else { return nil }
        setup = s
        re = .allocate(capacity: size / 2)
        im = .allocate(capacity: size / 2)
    }

    deinit {
        vDSP_destroy_fftsetupD(setup)
        re.deallocate()
        im.deallocate()
    }

    /// Transforms the real window `x` (length `n`); afterwards `energy(bin:)` gives each bin.
    func analyze(_ x: [Double]) {
        var split = DSPDoubleSplitComplex(realp: re, imagp: im)
        x.withUnsafeBufferPointer { p in
            p.baseAddress!.withMemoryRebound(to: DSPDoubleComplex.self, capacity: n / 2) { c in
                vDSP_ctozD(c, 2, &split, 1, vDSP_Length(n / 2))
            }
        }
        vDSP_fft_zripD(setup, &split, 1, log2n, FFTDirection(FFT_FORWARD))
    }

    /// |X(k)|^2 of bin `k` (0 < k < n/2) of the last `analyze`, in Goertzel scale (vDSP real FFTs come out doubled).
    @inline(__always) func energy(bin k: Int) -> Double {
        let r = re[k], i = im[k]
        return (r * r + i * i) / 4
    }
}

/// Energy of each tone in `x` (Goertzel). Four tones per pass so the four recurrences advance together.
func goertzel(_ x: [Double], coeffs: [Double], out: inout [Double]) {
    let n = x.count
    x.withUnsafeBufferPointer { xp in
        var t = 0
        while t + 4 <= coeffs.count {
            let c0 = coeffs[t], c1 = coeffs[t + 1], c2 = coeffs[t + 2], c3 = coeffs[t + 3]
            var a1 = 0.0, a2 = 0.0, b1 = 0.0, b2 = 0.0, d1 = 0.0, d2 = 0.0, e1 = 0.0, e2 = 0.0
            for i in 0..<n {
                let v = xp[i]
                let a0 = v + c0 * a1 - a2; a2 = a1; a1 = a0
                let b0 = v + c1 * b1 - b2; b2 = b1; b1 = b0
                let d0 = v + c2 * d1 - d2; d2 = d1; d1 = d0
                let e0 = v + c3 * e1 - e2; e2 = e1; e1 = e0
            }
            out[t] = a1 * a1 + a2 * a2 - c0 * a1 * a2
            out[t + 1] = b1 * b1 + b2 * b2 - c1 * b1 * b2
            out[t + 2] = d1 * d1 + d2 * d2 - c2 * d1 * d2
            out[t + 3] = e1 * e1 + e2 * e2 - c3 * e1 * e2
            t += 4
        }
        while t < coeffs.count {
            let c = coeffs[t]
            var s1 = 0.0, s2 = 0.0
            for i in 0..<n { let s0 = xp[i] + c * s1 - s2; s2 = s1; s1 = s0 }
            out[t] = s1 * s1 + s2 * s2 - c * s1 * s2
            t += 1
        }
    }
}

/// Tukey window of `size` samples: raised cosine on the edges, 1 elsewhere. `taper` is the smoothed fraction.
func symbolWindow(_ size: Int, taper: Double = 0.1) -> [Double] {
    let edge = taper * Double(size - 1) / 2
    var out = [Double](repeating: 1, count: size)
    if edge <= 0 { return out }
    for i in 0..<size {
        let d = Double(min(i, size - 1 - i))
        if d < edge { out[i] = 0.5 - 0.5 * cos(Double.pi * d / edge) }
    }
    return out
}
