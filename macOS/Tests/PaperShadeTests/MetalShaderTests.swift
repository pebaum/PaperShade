import CoreVideo
import FilterCore
import Foundation
import Metal
@testable import PaperShadeRendering
import XCTest

final class MetalShaderTests: XCTestCase {
    func testRealMetalAllPresetsAndPatternSizesAgainstCOracle() throws {
        let renderer = try requireRenderer()
        let width = 256
        let height = 64
        let pixels = fixture(width: width, height: height) { x, y in
            if y == 0 { return (UInt8(x), UInt8(x), UInt8(x)) }
            return (UInt8(x), UInt8((x * 73 + y * 19) & 255), UInt8((255 - x + y * 29) & 255))
        }
        let source = try makeSource(renderer: renderer, pixels: pixels, width: width, height: height)
        for preset in Preset.allCases {
            for scale in 1...4 {
                for kelvin in [6500, 4500, 2700, 1200] {
                    let result = try render(
                        renderer: renderer, source: source, preset: preset, scale: scale, temperatureKelvin: kelvin
                    )
                    try compare(result, source: pixels, width: width, height: height,
                                preset: preset, scale: scale, temperatureKelvin: kelvin)
                }
            }
        }
    }

    func testRealMetalOriginalIdentityAndWarmAttenuation() throws {
        let renderer = try requireRenderer()
        let width = 256
        let pixels = fixture(width: width, height: 1) { x, _ in
            if x == 0 { return (0, 0, 0) }
            if x == 255 { return (255, 255, 255) }
            return (UInt8(x), UInt8((x * 73) & 255), UInt8(255 - x))
        }
        let source = try makeSource(renderer: renderer, pixels: pixels, width: width, height: 1)
        let neutral = try render(renderer: renderer, source: source, preset: .original, scale: 1)
        for x in 0..<width {
            let index = x * 4
            XCTAssertEqual(Array(neutral.bytes[index..<(index + 3)]), Array(pixels[index..<(index + 3)]))
            XCTAssertEqual(neutral.bytes[index + 3], 255)
        }
        for kelvin in [4500, 2700, 1200, 1000] {
            let warm = try render(
                renderer: renderer, source: source, preset: .original, scale: 1, temperatureKelvin: kelvin
            )
            try compare(warm, source: pixels, width: width, height: 1,
                        preset: .original, scale: 1, temperatureKelvin: kelvin)
            for x in 0..<width {
                for channel in 0..<3 {
                    XCTAssertLessThanOrEqual(warm.bytes[x * 4 + channel], neutral.bytes[x * 4 + channel])
                }
            }
            XCTAssertEqual(Array(warm.bytes.prefix(4)), [0, 0, 0, 255])
            let white = 255 * 4
            XCTAssertEqual(warm.bytes[white + 2], 255)
            XCTAssertGreaterThan(warm.bytes[white + 2], warm.bytes[white + 1])
            XCTAssertGreaterThan(warm.bytes[white + 1], warm.bytes[white])
        }
    }

    func testRealMetalRejectsNonFiniteOrOutOfRangeWarmthGains() throws {
        let renderer = try requireRenderer()
        let source = try makeSource(renderer: renderer, pixels: [255, 255, 255, 0], width: 1, height: 1)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: 1, height: 1, mipmapped: false
        )
        descriptor.storageMode = .private
        descriptor.usage = .renderTarget
        let destination = try XCTUnwrap(renderer.device.makeTexture(descriptor: descriptor))
        let fields: [WritableKeyPath<PSFilterParameters, Float>] = [\.warmthRed, \.warmthGreen, \.warmthBlue]
        for field in fields {
            for value in [Float.nan, .infinity, -.infinity, -0.01, 1.01] {
                var parameters = try CoreParameters.make(preset: .original, pixelSize: 1)
                parameters[keyPath: field] = value
                let command = try XCTUnwrap(renderer.commandQueue.makeCommandBuffer())
                XCTAssertThrowsError(try renderer.encode(
                    source: source, destination: destination, parameters: parameters, commandBuffer: command
                ))
            }
        }
    }

    func testRealMetalExactPS1RGBEveryByteAndDitherPhase() throws {
        let renderer = try requireRenderer()
        // A 16×16 patch per byte contains all 4×4 phases at every supported scale.
        let width = 256 * 16
        let height = 16
        let pixels = fixture(width: width, height: height) { x, _ in
            let value = x / 16
            return (UInt8(value), UInt8(255 - value), UInt8((value * 73) & 255))
        }
        let source = try makeSource(renderer: renderer, pixels: pixels, width: width, height: height)
        for scale in 1...4 {
            let result = try render(renderer: renderer, source: source, preset: .ps1Color, scale: scale)
            try compare(result, source: pixels, width: width, height: height, preset: .ps1Color, scale: scale)
        }
    }

    func testRealIOSurfaceTextureCachePath() throws {
        let renderer = try requireRenderer()
        let width = 64
        let height = 16
        let pixels = fixture(width: width, height: height) { x, y in
            (UInt8(x * 4), UInt8(y * 16), UInt8((x * 11 + y * 3) & 255))
        }
        let attributes: [String: Any] = [
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:]
        ]
        var optionalBuffer: CVPixelBuffer?
        let status = CVPixelBufferCreate(
            kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA,
            attributes as CFDictionary, &optionalBuffer
        )
        XCTAssertEqual(status, kCVReturnSuccess)
        let pixelBuffer = try XCTUnwrap(optionalBuffer)
        XCTAssertEqual(CVPixelBufferLockBaseAddress(pixelBuffer, []), kCVReturnSuccess)
        do {
            defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, []) }
            let base = try XCTUnwrap(CVPixelBufferGetBaseAddress(pixelBuffer))
            let stride = CVPixelBufferGetBytesPerRow(pixelBuffer)
            try pixels.withUnsafeBytes { input in
                let source = try XCTUnwrap(input.baseAddress)
                for y in 0..<height {
                    base.advanced(by: y * stride).copyMemory(
                        from: source.advanced(by: y * width * 4), byteCount: width * 4
                    )
                }
            }
        }
        let cache = try renderer.makeTextureCache()
        let captured = try CapturedTexture(pixelBuffer: pixelBuffer, cache: cache)
        XCTAssertEqual(captured.texture.pixelFormat, .bgra8Unorm)
        let cases: [(Preset, Int)] = [
            (.ps1Color, 6500), (.original, 6500), (.ps1Color, 2700), (.original, 1200), (.ink4, 4500)
        ]
        for (preset, kelvin) in cases {
            let result = try render(
                renderer: renderer, source: captured.texture, preset: preset, scale: 1, temperatureKelvin: kelvin
            )
            try compare(result, source: pixels, width: width, height: height,
                        preset: preset, scale: 1, temperatureKelvin: kelvin)
        }
        withExtendedLifetime(captured) {}
    }

    private func requireRenderer() throws -> MetalRenderer {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("No Metal device is exposed by this runner. CPU exhaustive/oracle tests still run; GPU output was not verified.")
        }
        return try MetalRenderer(device: device)
    }

    private func fixture(width: Int, height: Int, rgb: (Int, Int) -> (UInt8, UInt8, UInt8)) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: width * height * 4)
        for y in 0..<height {
            for x in 0..<width {
                let (red, green, blue) = rgb(x, y)
                let index = (y * width + x) * 4
                bytes[index] = blue
                bytes[index + 1] = green
                bytes[index + 2] = red
                bytes[index + 3] = UInt8((x + y) & 255)
            }
        }
        return bytes
    }

    private func makeSource(renderer: MetalRenderer, pixels: [UInt8], width: Int, height: Int) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false
        )
        descriptor.usage = .shaderRead
        descriptor.storageMode = renderer.device.hasUnifiedMemory ? .shared : .managed
        let texture = try XCTUnwrap(renderer.device.makeTexture(descriptor: descriptor))
        try pixels.withUnsafeBytes {
            let base = try XCTUnwrap($0.baseAddress)
            texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                            withBytes: base, bytesPerRow: width * 4)
        }
        return texture
    }

    private func render(
        renderer: MetalRenderer, source: MTLTexture, preset: Preset, scale: Int, temperatureKelvin: Int = 6500
    ) throws -> (bytes: [UInt8], rowBytes: Int) {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: source.width, height: source.height, mipmapped: false
        )
        descriptor.storageMode = .private
        descriptor.usage = .renderTarget
        let output = try XCTUnwrap(renderer.device.makeTexture(descriptor: descriptor))
        let rowBytes = (source.width * 4 + 255) & ~255
        let staging = try XCTUnwrap(renderer.device.makeBuffer(
            length: rowBytes * source.height, options: .storageModeShared
        ))
        let command = try XCTUnwrap(renderer.commandQueue.makeCommandBuffer())
        try renderer.encode(
            source: source, destination: output,
            parameters: CoreParameters.make(preset: preset, pixelSize: scale, temperatureKelvin: temperatureKelvin),
            commandBuffer: command
        )
        // CPU readback exists only in tests, never in the application frame path.
        let blit = try XCTUnwrap(command.makeBlitCommandEncoder())
        blit.copy(
            from: output, sourceSlice: 0, sourceLevel: 0,
            sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
            sourceSize: MTLSize(width: source.width, height: source.height, depth: 1),
            to: staging, destinationOffset: 0, destinationBytesPerRow: rowBytes,
            destinationBytesPerImage: rowBytes * source.height
        )
        blit.endEncoding()
        command.commit()
        command.waitUntilCompleted()
        guard command.status == .completed else {
            throw RenderingError(command.error?.localizedDescription ?? "GPU test rendering failed.")
        }
        return (
            Array(UnsafeBufferPointer(start: staging.contents().assumingMemoryBound(to: UInt8.self),
                                      count: rowBytes * source.height)),
            rowBytes
        )
    }

    private func compare(
        _ result: (bytes: [UInt8], rowBytes: Int), source: [UInt8],
        width: Int, height: Int, preset: Preset, scale: Int, temperatureKelvin: Int = 6500
    ) throws {
        var parameters = try CoreParameters.make(preset: preset, pixelSize: scale, temperatureKelvin: temperatureKelvin)
        // Neutral quantized palettes and Original are byte-exact. Continuous gray
        // retains its existing UNORM tolerance; attenuated channels allow ±1 only
        // for post-warm UNORM rounding, never an additional pre-quantization error.
        let neutralTolerance = parameters.quantizer == 0 && parameters.color == 0 ? 1 : 0
        let tolerances = [parameters.warmthBlue, parameters.warmthGreen, parameters.warmthRed].map { gain in
            gain == 0 ? 0 : gain < 1 ? 1 : neutralTolerance
        }
        for y in 0..<height {
            for x in 0..<width {
                let sourceIndex = (y * width + x) * 4
                let destinationIndex = y * result.rowBytes + x * 4
                let pixel = PSPixel(red: source[sourceIndex + 2], green: source[sourceIndex + 1],
                                    blue: source[sourceIndex], alpha: source[sourceIndex + 3])
                var expected = PSPixel()
                guard PSReferencePixel(pixel, Int32(x), Int32(y), &parameters, &expected) == 1 else {
                    throw RenderingError("The C oracle rejected a valid GPU test input.")
                }
                let actual = Array(result.bytes[destinationIndex..<(destinationIndex + 4)])
                let reference = [expected.blue, expected.green, expected.red, expected.alpha]
                if actual[3] != 255 || (0..<3).contains(where: {
                    abs(Int(actual[$0]) - Int(reference[$0])) > tolerances[$0]
                }) {
                    return XCTFail("Metal != C oracle: \(preset), \(temperatureKelvin) K, scale \(scale), (\(x), \(y)), " +
                        "BGRA \(actual) != \(reference), BGR tolerances \(tolerances)")
                }
            }
        }
    }
}
