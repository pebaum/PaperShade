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
                let result = try render(renderer: renderer, source: source, preset: preset, scale: scale)
                try compare(result, source: pixels, width: width, height: height, preset: preset, scale: scale)
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
            pixels.withUnsafeBytes { input in
                for y in 0..<height {
                    base.advanced(by: y * stride).copyMemory(
                        from: input.baseAddress!.advanced(by: y * width * 4), byteCount: width * 4
                    )
                }
            }
        }
        let cache = try renderer.makeTextureCache()
        let captured = try CapturedTexture(pixelBuffer: pixelBuffer, cache: cache)
        XCTAssertEqual(captured.texture.pixelFormat, .bgra8Unorm)
        let result = try render(renderer: renderer, source: captured.texture, preset: .ps1Color, scale: 1)
        try compare(result, source: pixels, width: width, height: height, preset: .ps1Color, scale: 1)
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
        pixels.withUnsafeBytes {
            texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                            withBytes: $0.baseAddress!, bytesPerRow: width * 4)
        }
        return texture
    }

    private func render(
        renderer: MetalRenderer, source: MTLTexture, preset: Preset, scale: Int
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
        try renderer.encode(source: source, destination: output,
                            parameters: CoreParameters.make(preset: preset, pixelSize: scale), commandBuffer: command)
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
        width: Int, height: Int, preset: Preset, scale: Int
    ) throws {
        var parameters = try CoreParameters.make(preset: preset, pixelSize: scale)
        // Only continuous grayscale permits ±1 for UNORM tie-rounding. Every
        // quantized palette, including PS1 RGB555, must match byte-for-byte.
        let tolerance = parameters.quantizer == 0 ? 1 : 0
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
                if actual[3] != 255 || zip(actual.prefix(3), reference.prefix(3)).contains(where: {
                    abs(Int($0.0) - Int($0.1)) > tolerance
                }) {
                    return XCTFail("Metal != C oracle: \(preset), scale \(scale), (\(x), \(y)), " +
                        "BGRA \(actual) != \(reference), tolerance \(tolerance)")
                }
            }
        }
    }
}
