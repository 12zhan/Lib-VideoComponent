// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "Lib-VideoComponent",
    platforms: [
        .iOS(.v13),
    ],
    products: [
        .library(
            name: "LibVideoComponent",
            targets: ["LibVideoComponent"]
        ),
    ],
    targets: [
        .target(name: "LibVideoComponent"),
    ]
)
