// Identifiers derived from the SHA-256 of a text (docs/SESSION.md section 3.3). None of them is a secret or a proof.
import CryptoKit
import Foundation

/// SHA-256 based identifiers, as lowercase hex.
public enum Ids {
    /// SHA-256 of the UTF-8 bytes of `text`, 64 hex characters.
    public static func payloadId(_ text: String) -> String { hex(Array(SHA256.hash(data: Data(text.utf8)))) }
    /// First 12 bytes (24 hex): what a reference frame carries.
    public static func fingerprint(_ text: String) -> String { String(payloadId(text).prefix(24)) }
    /// First 4 bytes (8 hex): what an ack or receipt carries.
    public static func ackId(_ text: String) -> String { String(payloadId(text).prefix(8)) }
    /// First 3 bytes (6 hex): what a risk frame carries.
    public static func shortId(_ text: String) -> String { String(payloadId(text).prefix(6)) }
    /// `000000`: "no data" in a risk frame.
    public static let unknownShort = "000000"

    static func hex(_ bytes: [UInt8]) -> String { bytes.map { String(format: "%02x", $0) }.joined() }
    static func bytes(fromHex s: String) -> [UInt8] {
        let c = Array(s.utf8)
        return stride(from: 0, to: c.count - 1, by: 2).compactMap { UInt8(String(decoding: c[$0..<($0 + 2)], as: UTF8.self), radix: 16) }
    }
}
