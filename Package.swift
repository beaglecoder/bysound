// swift-tools-version:5.7
// Root manifest so the repo can be added by URL in SwiftPM. Tests live in ios/Package.swift (`cd ios && swift test`).
import PackageDescription

let package = Package(
    name: "BySound",
    platforms: [.iOS(.v15), .macOS(.v13)],
    products: [
        .library(name: "BySound", targets: ["BySound"]),
    ],
    targets: [
        .target(name: "BySound", path: "ios/Sources/BySound"),
    ]
)
