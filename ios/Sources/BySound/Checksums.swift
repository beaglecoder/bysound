// CRC-8, CRC-16 and CRC-32 as used by bysound v4.
import Foundation

/// CRC-8 of the frame header: polynomial 0x2F, initial value 0xFF.
public func crc8(_ data: [UInt8]) -> UInt8 {
    var c: UInt8 = 0xff
    for byte in data {
        c ^= byte
        for _ in 0..<8 { c = c & 0x80 != 0 ? (c << 1) ^ 0x2f : c << 1 }
    }
    return c
}

/// CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF).
public func crc16(_ data: [UInt8]) -> UInt16 {
    var crc: UInt16 = 0xffff
    for byte in data {
        crc ^= UInt16(byte) << 8
        for _ in 0..<8 { crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1 }
    }
    return crc
}

/// CRC-32 (IEEE, reflected, polynomial 0xEDB88320).
public func crc32(_ data: [UInt8]) -> UInt32 {
    var c: UInt32 = 0xffff_ffff
    for b in data {
        c ^= UInt32(b)
        for _ in 0..<8 { c = c & 1 != 0 ? (c >> 1) ^ 0xedb8_8320 : c >> 1 }
    }
    return ~c
}
