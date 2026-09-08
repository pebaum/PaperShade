import FilterCore
import Foundation
@testable import PaperShadeRendering
import XCTest

final class CoreContractTests: XCTestCase {
    private let signedPS1: [Int32] = [-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2]
    private let bayer: [Int32] = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]

    func testPresetIdentifiersAndEveryABIOffset() {
        XCTAssertEqual(PSPresetCount(), 13)
        XCTAssertEqual(Preset.allCases.map(\.rawValue), Array(Int32(0)...Int32(12)))
        XCTAssertEqual(Preset.ps1Color.rawValue, 11)
        XCTAssertEqual(Preset.original.rawValue, 12)
        XCTAssertEqual(MemoryLayout<PSFilterParameters>.size, 64)
        XCTAssertEqual(MemoryLayout<PSFilterParameters>.stride, 64)
        XCTAssertEqual(MemoryLayout<PSPixel>.size, 4)
        let offsets = [
            MemoryLayout<PSFilterParameters>.offset(of: \.red),
            MemoryLayout<PSFilterParameters>.offset(of: \.green),
            MemoryLayout<PSFilterParameters>.offset(of: \.blue),
            MemoryLayout<PSFilterParameters>.offset(of: \.gain),
            MemoryLayout<PSFilterParameters>.offset(of: \.offset),
            MemoryLayout<PSFilterParameters>.offset(of: \.quantizer),
            MemoryLayout<PSFilterParameters>.offset(of: \.levels),
            MemoryLayout<PSFilterParameters>.offset(of: \.pixelSize),
            MemoryLayout<PSFilterParameters>.offset(of: \.color),
            MemoryLayout<PSFilterParameters>.offset(of: \.padding0),
            MemoryLayout<PSFilterParameters>.offset(of: \.padding1),
            MemoryLayout<PSFilterParameters>.offset(of: \.padding2),
            MemoryLayout<PSFilterParameters>.offset(of: \.warmthRed),
            MemoryLayout<PSFilterParameters>.offset(of: \.warmthGreen),
            MemoryLayout<PSFilterParameters>.offset(of: \.warmthBlue),
            MemoryLayout<PSFilterParameters>.offset(of: \.warmthPadding)
        ]
        XCTAssertEqual(offsets, stride(from: 0, through: 60, by: 4).map { Optional($0) })
    }

    func testAllPresetParametersAndScales() throws {
        let expected: [(Float, Float, Float, Float, Float, Int32, Int32, Int32)] = [
            (0.2126, 0.7152, 0.0722, 1, 0, 0, 256, 0),
            (0.299, 0.587, 0.114, 1, 0, 0, 256, 0),
            (1 / 3, 1 / 3, 1 / 3, 1, 0, 0, 256, 0),
            (0.2126, 0.7152, 0.0722, 0.88, 0.06, 0, 256, 0),
            (0.2126, 0.7152, 0.0722, 1.5, -0.25, 0, 256, 0),
            (0.2126, 0.7152, 0.0722, -1, 1, 0, 256, 0),
            (0.2126, 0.7152, 0.0722, 1, 0, 1, 2, 0),
            (0.2126, 0.7152, 0.0722, 1, 0, 2, 2, 0),
            (0.2126, 0.7152, 0.0722, 1, 0, 2, 4, 0),
            (0.2126, 0.7152, 0.0722, 1, 0, 2, 16, 0),
            (0.2126, 0.7152, 0.0722, 1, 0, 3, 32, 0),
            (0.2126, 0.7152, 0.0722, 1, 0, 3, 32, 1),
            (0.2126, 0.7152, 0.0722, 1, 0, 0, 256, 1)
        ]
        for preset in Preset.allCases {
            for scale in FilterSettings.patternSizes {
                let actual = try CoreParameters.make(preset: preset, pixelSize: scale)
                let e = expected[Int(preset.rawValue)]
                XCTAssertEqual(actual.red, e.0, accuracy: 0.0000001)
                XCTAssertEqual(actual.green, e.1, accuracy: 0.0000001)
                XCTAssertEqual(actual.blue, e.2, accuracy: 0.0000001)
                XCTAssertEqual(actual.gain, e.3)
                XCTAssertEqual(actual.offset, e.4)
                XCTAssertEqual(actual.quantizer, e.5)
                XCTAssertEqual(actual.levels, e.6)
                XCTAssertEqual(actual.color, e.7)
                XCTAssertEqual(actual.pixelSize, Int32(scale))
                XCTAssertEqual([actual.padding0, actual.padding1, actual.padding2], [0, 0, 0])
                XCTAssertEqual([actual.warmthRed, actual.warmthGreen, actual.warmthBlue, actual.warmthPadding], [1, 1, 1, 0])
                let explicitNeutral = try CoreParameters.make(preset: preset, pixelSize: scale, temperatureKelvin: 6500)
                var legacy = PSFilterParameters()
                XCTAssertEqual(PSGetFilterParameters(preset.rawValue, Int32(scale), &legacy), 1)
                XCTAssertEqual(bytes(of: actual), bytes(of: explicitNeutral))
                XCTAssertEqual(bytes(of: actual), bytes(of: legacy))
            }
        }
    }

    func testInvalidCInputsAreRejectedWithoutWritingOutput() throws {
        var parameters = try CoreParameters.make(preset: .paper, pixelSize: 2)
        let original = bytes(of: parameters)
        for preset in [Int32.min, -1, 13, Int32.max] {
            XCTAssertEqual(PSGetFilterParameters(preset, 1, &parameters), 0)
            XCTAssertEqual(bytes(of: parameters), original)
            XCTAssertEqual(PSGetWarmFilterParameters(preset, 1, 3500, &parameters), 0)
            XCTAssertEqual(bytes(of: parameters), original)
        }
        for scale in [Int32.min, -1, 0, 5, Int32.max] {
            XCTAssertEqual(PSGetFilterParameters(0, scale, &parameters), 0)
            XCTAssertEqual(bytes(of: parameters), original)
            XCTAssertEqual(PSGetWarmFilterParameters(0, scale, 3500, &parameters), 0)
            XCTAssertEqual(bytes(of: parameters), original)
        }
        for kelvin in [Int32.min, -1, 0, 999, 6501, Int32.max] {
            XCTAssertEqual(PSGetWarmFilterParameters(0, 1, kelvin, &parameters), 0)
            XCTAssertEqual(bytes(of: parameters), original)
        }
        XCTAssertEqual(PSGetFilterParameters(0, 1, nil), 0)
        XCTAssertEqual(PSGetWarmFilterParameters(0, 1, 3500, nil), 0)
        XCTAssertThrowsError(try CoreParameters.make(preset: .natural, pixelSize: 0))
        XCTAssertThrowsError(try CoreParameters.make(preset: .natural, pixelSize: Int.max))
        for kelvin in [Int.min, -1, 999, 6501, Int.max] {
            XCTAssertThrowsError(try CoreParameters.make(preset: .original, pixelSize: 1, temperatureKelvin: kelvin))
        }
        let input = PSPixel(red: 1, green: 2, blue: 3, alpha: 4)
        var output = PSPixel(red: 11, green: 12, blue: 13, alpha: 14)
        let sentinel = bytes(of: output)
        XCTAssertEqual(PSReferencePixel(input, -1, 0, &parameters, &output), 0)
        XCTAssertEqual(PSReferencePixel(input, 0, -1, &parameters, &output), 0)
        XCTAssertEqual(PSReferencePixel(input, 0, 0, nil, &output), 0)
        XCTAssertEqual(PSReferencePixel(input, 0, 0, &parameters, nil), 0)
        XCTAssertEqual(bytes(of: output), sentinel)
        let corruptions: [(inout PSFilterParameters) -> Void] = [
            { $0.red = .nan }, { $0.green = .infinity }, { $0.blue = -1 },
            { $0.gain = 17 }, { $0.offset = -.infinity }, { $0.quantizer = 4 },
            { $0.levels = 1 }, { $0.levels = 257 }, { $0.pixelSize = 0 }, { $0.color = 2 }
        ]
        for corrupt in corruptions {
            var invalid = parameters
            corrupt(&invalid)
            XCTAssertEqual(PSReferencePixel(input, 0, 0, &invalid, &output), 0)
            XCTAssertEqual(bytes(of: output), sentinel)
        }
        let warmthFields: [WritableKeyPath<PSFilterParameters, Float>] = [\.warmthRed, \.warmthGreen, \.warmthBlue]
        for field in warmthFields {
            for value in [Float.nan, .infinity, -.infinity, -0.01, 1.01] {
                var invalid = parameters
                invalid[keyPath: field] = value
                XCTAssertEqual(PSReferencePixel(input, 0, 0, &invalid, &output), 0)
                XCTAssertEqual(bytes(of: output), sentinel)
            }
        }
    }

    func testWarmParametersLeaveBaseParametersUnchanged() throws {
        for preset in Preset.allCases {
            for scale in FilterSettings.patternSizes {
                let neutral = try CoreParameters.make(preset: preset, pixelSize: scale)
                for kelvin in [1000, 1200, 2700, 4500, 6499, 6500] {
                    let warm = try CoreParameters.make(preset: preset, pixelSize: scale, temperatureKelvin: kelvin)
                    // The old 48 bytes remain intact; Kelvin gains belong only in the appended register.
                    XCTAssertEqual(Array(bytes(of: warm).prefix(48)), Array(bytes(of: neutral).prefix(48)))
                    XCTAssertEqual(warm.warmthPadding, 0)
                    for gain in [warm.warmthRed, warm.warmthGreen, warm.warmthBlue] {
                        XCTAssertTrue(gain.isFinite)
                        XCTAssertTrue((0...1).contains(gain))
                    }
                    XCTAssertEqual(warm.warmthRed, 1)
                    if kelvin < 6500 {
                        XCTAssertGreaterThan(warm.warmthRed, warm.warmthGreen)
                        XCTAssertGreaterThan(warm.warmthGreen, warm.warmthBlue)
                    }
                }
            }
        }
    }

    func testOriginalNeutralIdentityAndWarmWhite() throws {
        var neutral = try CoreParameters.make(preset: .original, pixelSize: 1)
        for input in [
            PSPixel(red: 128, green: 5, blue: 255, alpha: 0),
            PSPixel(red: 0, green: 0, blue: 0, alpha: 1),
            PSPixel(red: 255, green: 255, blue: 255, alpha: 127)
        ] {
            XCTAssertEqual(bytes(of: try reference(input, x: 0, y: 0, parameters: &neutral)),
                           [input.red, input.green, input.blue, 255])
        }
        let white = PSPixel(red: 255, green: 255, blue: 255, alpha: 0)
        for kelvin in [1000, 1200, 2700, 4500] {
            var warm = try CoreParameters.make(preset: .original, pixelSize: 1, temperatureKelvin: kelvin)
            let result = try reference(white, x: 0, y: 0, parameters: &warm)
            XCTAssertEqual(result.red, 255)
            XCTAssertGreaterThan(result.red, result.green)
            XCTAssertGreaterThan(result.green, result.blue)
            XCTAssertEqual(result.alpha, 255)
            let black = PSPixel(red: 0, green: 0, blue: 0, alpha: 0)
            XCTAssertEqual(bytes(of: try reference(black, x: 0, y: 0, parameters: &warm)), [0, 0, 0, 255])
        }
    }

    func testWarmthAttenuatesAfterPaletteAndPS1Quantization() throws {
        let inputs = [
            PSPixel(red: 128, green: 5, blue: 255, alpha: 0),
            PSPixel(red: 255, green: 255, blue: 255, alpha: 1),
            PSPixel(red: 127, green: 128, blue: 129, alpha: 7),
            PSPixel(red: 0, green: 0, blue: 0, alpha: 255)
        ]
        for preset in Preset.allCases {
            for scale in FilterSettings.patternSizes {
                var neutral = try CoreParameters.make(preset: preset, pixelSize: scale)
                for kelvin in [4500, 2700, 1200] {
                    var warm = try CoreParameters.make(preset: preset, pixelSize: scale, temperatureKelvin: kelvin)
                    for phase in 0..<16 {
                        for input in inputs {
                            let x = (phase & 3) * scale
                            let y = (phase >> 2) * scale
                            let plain = try reference(input, x: x, y: y, parameters: &neutral)
                            let tinted = try reference(input, x: x, y: y, parameters: &warm)
                            XCTAssertEqual(tinted.alpha, 255)
                            XCTAssertEqual(tinted.red, plain.red)
                            XCTAssertLessThanOrEqual(tinted.green, plain.green)
                            XCTAssertLessThanOrEqual(tinted.blue, plain.blue)
                            if neutral.quantizer != 0 || preset == .original {
                                let gains: [Float] = [warm.warmthRed, warm.warmthGreen, warm.warmthBlue]
                                let channels: [UInt8] = [plain.red, plain.green, plain.blue]
                                var expected: [UInt8] = []
                                for channel in 0..<3 {
                                    let normalized: Float = Float(channels[channel]) / 255.0
                                    let attenuated: Float = normalized * gains[channel]
                                    let rounded: Float = (attenuated * 255.0 + 0.5).rounded(.down)
                                    expected.append(UInt8(rounded))
                                }
                                XCTAssertEqual([tinted.red, tinted.green, tinted.blue], expected,
                                               "Warmth must follow quantization: \(preset), \(kelvin) K, phase \(phase)")
                            }
                        }
                    }
                }
            }
        }
    }

    func testDitherGoldenTablesAndInvalidLookups() throws {
        XCTAssertEqual(try CoreParameters.ditherValues(quantizer: 3), signedPS1)
        XCTAssertEqual(try CoreParameters.ditherValues(quantizer: 2), bayer)
        var output: Int32 = 12345
        for quantizer in [Int32(-1), 0, 1, 4, Int32.max] {
            XCTAssertEqual(PSDitherValue(quantizer, 0, &output), 0)
            XCTAssertEqual(output, 12345)
        }
        for index in [Int32(-1), 16, Int32.max] {
            XCTAssertEqual(PSDitherValue(3, index, &output), 0)
            XCTAssertEqual(output, 12345)
        }
        XCTAssertEqual(PSDitherValue(3, 0, nil), 0)
        XCTAssertThrowsError(try CoreParameters.ditherValues(quantizer: 1))
    }

    func testPaletteAndPS1GoldenPixels() throws {
        let input = PSPixel(red: 128, green: 5, blue: 255, alpha: 0)
        var ps1 = try CoreParameters.make(preset: .ps1Color, pixelSize: 1)
        XCTAssertEqual(bytes(of: try reference(input, x: 0, y: 0, parameters: &ps1)), [123, 0, 255, 255])
        XCTAssertEqual(bytes(of: try reference(input, x: 1, y: 0, parameters: &ps1)), [132, 0, 255, 255])
        var threshold = try CoreParameters.make(preset: .inkThreshold, pixelSize: 1)
        XCTAssertEqual(try reference(PSPixel(red: 127, green: 127, blue: 127, alpha: 0),
                                     x: 0, y: 0, parameters: &threshold).red, 0)
        XCTAssertEqual(try reference(PSPixel(red: 128, green: 128, blue: 128, alpha: 0),
                                     x: 0, y: 0, parameters: &threshold).red, 255)
        for (preset, count) in [(Preset.ink4, 4), (.ink16, 16)] {
            var parameters = try CoreParameters.make(preset: preset, pixelSize: 1)
            var palette = Set<UInt8>()
            for value in 0...255 {
                let gray = UInt8(value)
                for phase in 0..<16 {
                    palette.insert(try reference(PSPixel(red: gray, green: gray, blue: gray, alpha: 0),
                                                 x: phase & 3, y: phase >> 2, parameters: &parameters).red)
                }
            }
            XCTAssertEqual(palette, Set((0..<count).map { UInt8($0 * 255 / (count - 1)) }))
        }
    }

    // Always runs, including on GPU-less CI: every 8-bit channel value, 16 phases,
    // four pattern sizes and all presets. PS1 channels have independent integer goldens.
    func testExhaustiveBytePhaseScaleSpaceOnCPU() throws {
        let rgb555 = Set((0..<32).map { UInt8(($0 << 3) | ($0 >> 2)) })
        for preset in Preset.allCases {
            for scale in 1...4 {
                var parameters = try CoreParameters.make(preset: preset, pixelSize: scale)
                for phase in 0..<16 {
                    let x = (phase & 3) * scale
                    let y = (phase >> 2) * scale
                    for value in 0...255 {
                        let input = PSPixel(red: UInt8(value), green: UInt8(255 - value),
                                            blue: UInt8((value * 73 + 29) & 255), alpha: UInt8(value))
                        let actual = try reference(input, x: x, y: y, parameters: &parameters)
                        let repeated = try reference(input, x: x + 4 * scale, y: y + 4 * scale, parameters: &parameters)
                        let enlarged = try reference(input, x: x + scale - 1, y: y + scale - 1, parameters: &parameters)
                        guard actual.alpha == 255, bytes(of: actual) == bytes(of: repeated),
                              bytes(of: actual) == bytes(of: enlarged) else {
                            return XCTFail("Alpha/period/pattern-size mismatch: \(preset), \(scale), \(phase), \(value)")
                        }
                        if preset == .ps1Color {
                            let expected = [input.red, input.green, input.blue].map { channel -> UInt8 in
                                let five = min(255, max(0, Int(channel) + Int(signedPS1[phase]))) >> 3
                                return UInt8((five << 3) | (five >> 2))
                            }
                            guard [actual.red, actual.green, actual.blue] == expected else {
                                return XCTFail("PS1 integer mismatch: \(scale), \(phase), \(value)")
                            }
                        } else if preset == .original {
                            guard [actual.red, actual.green, actual.blue] == [input.red, input.green, input.blue] else {
                                return XCTFail("Neutral Original changed source colors")
                            }
                        } else {
                            guard actual.red == actual.green, actual.green == actual.blue else {
                                return XCTFail("Non-monochrome output for \(preset)")
                            }
                        }
                        if parameters.quantizer == 1 || parameters.quantizer == 2 {
                            guard Int(actual.red) % (255 / Int(parameters.levels - 1)) == 0 else {
                                return XCTFail("Invalid e-ink palette value for \(preset)")
                            }
                        } else if parameters.quantizer == 3 {
                            guard rgb555.contains(actual.red), rgb555.contains(actual.green), rgb555.contains(actual.blue) else {
                                return XCTFail("Invalid RGB555 palette value")
                            }
                        }
                    }
                }
            }
        }
    }

    private func bytes<T>(of value: T) -> [UInt8] {
        withUnsafeBytes(of: value) { Array($0) }
    }

    private func reference(_ input: PSPixel, x: Int, y: Int, parameters: inout PSFilterParameters) throws -> PSPixel {
        var output = PSPixel()
        guard PSReferencePixel(input, Int32(x), Int32(y), &parameters, &output) == 1 else {
            throw RenderingError("The C oracle rejected a valid test input.")
        }
        return output
    }
}
