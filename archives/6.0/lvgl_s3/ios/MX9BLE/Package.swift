// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MX9BLE",
    platforms: [
        .iOS(.v16),
        .macOS(.v13)
    ],
    products: [
        .library(name: "MX9BLE", targets: ["MX9BLE"])
    ],
    targets: [
        .target(name: "MX9BLE"),
        .testTarget(name: "MX9BLETests", dependencies: ["MX9BLE"])
    ]
)
