// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "PaperShade",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "PaperShadeRendering", targets: ["PaperShadeRendering"]),
        .executable(name: "PaperShade", targets: ["PaperShadeMac"])
    ],
    targets: [
        .target(
            name: "FilterCore",
            path: "core",
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("../src"),
                .unsafeFlags(["-ffp-contract=off"])
            ]
        ),
        .target(
            name: "PaperShadeRendering",
            dependencies: ["FilterCore"],
            path: "macOS/Sources/PaperShadeRendering",
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("IOSurface")
            ]
        ),
        .executableTarget(
            name: "PaperShadeMac",
            dependencies: ["PaperShadeRendering", "FilterCore"],
            path: "macOS/Sources/PaperShadeMac",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("Carbon"),
                .linkedFramework("ServiceManagement")
            ]
        ),
        .testTarget(
            name: "PaperShadeTests",
            dependencies: ["PaperShadeRendering", "FilterCore"],
            path: "macOS/Tests/PaperShadeTests"
        )
    ],
    swiftLanguageVersions: [.v5],
    cxxLanguageStandard: .cxx20
)
