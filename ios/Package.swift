// swift-tools-version:5.7
import PackageDescription

let package = Package(
    name: "BySound",
    platforms: [.iOS(.v15), .macOS(.v13)],
    products: [
        .library(name: "BySound", targets: ["BySound"]),
    ],
    targets: [
        .target(name: "BySound"),
        .testTarget(name: "BySoundTests", dependencies: ["BySound"]),
    ]
)
