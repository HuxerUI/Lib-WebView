// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "WebView",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "WebView",
            targets: ["WebView"]
        ),
    ],
    targets: [
        .target(
            name: "WebView",
            linkerSettings: [
                .linkedFramework("WebKit"),
            ]
        ),
    ]
)
