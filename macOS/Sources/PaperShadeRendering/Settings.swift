import CoreFoundation
import FilterCore
import Foundation

public enum Preset: Int32, CaseIterable, Hashable {
    case natural = 0, classic, average, paper, highContrast, inverted
    case inkThreshold, inkDither, ink4, ink16, ps1Gray, ps1Color
    case original = 12

    public var title: String {
        switch self {
        case .natural: return "Natural grayscale (Rec. 709)"
        case .classic: return "Classic grayscale (Rec. 601)"
        case .average: return "Equal-channel grayscale"
        case .paper: return "Soft paper grayscale"
        case .highContrast: return "High-contrast grayscale"
        case .inverted: return "Inverted grayscale"
        case .inkThreshold: return "E-ink — crisp black and white"
        case .inkDither: return "E-ink — dithered black and white"
        case .ink4: return "E-ink — 4 shades"
        case .ink16: return "E-ink — 16 shades"
        case .ps1Gray: return "PS1 — grayscale RGB555"
        case .ps1Color: return "PS1 — original color RGB555"
        case .original: return "Original colors (warmth only)"
        }
    }
}

public struct FilterSettings: Equatable {
    public static let frameCaps = [10, 15, 30, 60]
    public static let patternSizes = [1, 2, 3, 4]
    public static let neutralTemperatureKelvin = 6500
    public static let temperatureRange = 1000...6500
    public static let temperaturePresets = [6500, 5500, 4500, 3500, 2700, 2000, 1200]

    public var preset: Preset
    public var framesPerSecond: Int
    public var pixelSize: Int
    public var enabled: Bool
    public var temperatureKelvin: Int

    public var hasEffect: Bool {
        preset != .original || temperatureKelvin != Self.neutralTemperatureKelvin
    }

    public var requiresCapture: Bool { enabled && hasEffect }

    public init(
        preset: Preset = .natural,
        framesPerSecond: Int = 15,
        pixelSize: Int = 1,
        enabled: Bool = false,
        temperatureKelvin: Int = 6500
    ) {
        let validTemperature = Self.isValidTemperature(temperatureKelvin)
        if !Self.isValidFrameCap(framesPerSecond) || !Self.patternSizes.contains(pixelSize) || !validTemperature {
            NSLog("PaperShade: invalid filter settings were normalized to supported defaults.")
        }
        self.preset = preset
        self.framesPerSecond = Self.isValidFrameCap(framesPerSecond) ? framesPerSecond : 15
        self.pixelSize = Self.patternSizes.contains(pixelSize) ? pixelSize : 1
        self.enabled = enabled && validTemperature
        self.temperatureKelvin = validTemperature ? temperatureKelvin : Self.neutralTemperatureKelvin
    }

    public static func isValidFrameCap(_ value: Int) -> Bool {
        guard let unsigned = UInt32(exactly: value) else { return false }
        return PSValidFrameCap(unsigned) == 1
    }

    public static func isValidTemperature(_ value: Int) -> Bool {
        guard let unsigned = UInt32(exactly: value) else { return false }
        return PSValidTemperature(unsigned) == 1
    }

    public static func parseTemperature(_ text: String) throws -> Int {
        guard let value = Int(text.trimmingCharacters(in: .whitespacesAndNewlines)),
              isValidTemperature(value) else {
            throw RenderingError("Enter a whole-number temperature from 1000 to 6500 K.")
        }
        return value
    }

    public mutating func applyTemperature(_ value: Int) throws {
        guard Self.isValidTemperature(value) else {
            throw RenderingError("The selected temperature must be from 1000 to 6500 K.")
        }
        temperatureKelvin = value
        // Turning warmth off must not resume an independently paused style.
        if value != Self.neutralTemperatureKelvin { enabled = true }
    }

    public mutating func enableWarmthOnly() {
        preset = .original
        if temperatureKelvin == Self.neutralTemperatureKelvin { temperatureKelvin = 3500 }
        enabled = true
    }

    public func validated() -> Self {
        Self(preset: preset, framesPerSecond: framesPerSecond, pixelSize: pixelSize,
             enabled: enabled, temperatureKelvin: temperatureKelvin)
    }
}

public final class SettingsStore {
    enum Key {
        static let preset = "preset"
        static let frameCap = "frameCap"
        static let pixelSize = "pixelSize"
        static let enabled = "enabled"
        static let temperatureKelvin = "temperatureKelvin"
    }

    private let defaults: UserDefaults
    public private(set) var lastLoadWarning: String?

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public func load() -> FilterSettings {
        let rawPreset = integer(forKey: Key.preset).flatMap { Int32(exactly: $0) }
        let preset = rawPreset.flatMap(Preset.init(rawValue:)) ?? .natural
        let storedEnabled = boolean(forKey: Key.enabled)
        let enabled = storedEnabled ?? false
        let frameCap = integer(forKey: Key.frameCap)
        let pixelSize = integer(forKey: Key.pixelSize)
        let temperatureKelvin = integer(forKey: Key.temperatureKelvin)
        let validFrameCap = frameCap.map(FilterSettings.isValidFrameCap) ?? false
        let validPixelSize = pixelSize.map { FilterSettings.patternSizes.contains($0) } ?? false
        let validTemperature = temperatureKelvin.map(FilterSettings.isValidTemperature) ?? false
        let validEnabled = storedEnabled != nil
        let invalid = (defaults.object(forKey: Key.preset) != nil && rawPreset.flatMap(Preset.init(rawValue:)) == nil)
            || (defaults.object(forKey: Key.frameCap) != nil && !validFrameCap)
            || (defaults.object(forKey: Key.pixelSize) != nil && !validPixelSize)
            || (defaults.object(forKey: Key.enabled) != nil && !validEnabled)
            || (defaults.object(forKey: Key.temperatureKelvin) != nil && !validTemperature)
        lastLoadWarning = invalid ? "Invalid saved preferences were ignored and supported defaults were loaded. PaperShade starts paused." : nil
        if let lastLoadWarning { NSLog("PaperShade: %@", lastLoadWarning) }
        return FilterSettings(
            preset: preset,
            framesPerSecond: frameCap ?? 15,
            pixelSize: pixelSize ?? 1,
            enabled: enabled && !invalid,
            temperatureKelvin: temperatureKelvin ?? FilterSettings.neutralTemperatureKelvin
        )
    }

    public func save(_ settings: FilterSettings) {
        let settings = settings.validated()
        defaults.set(settings.preset.rawValue, forKey: Key.preset)
        defaults.set(settings.framesPerSecond, forKey: Key.frameCap)
        defaults.set(settings.pixelSize, forKey: Key.pixelSize)
        defaults.set(settings.enabled, forKey: Key.enabled)
        defaults.set(settings.temperatureKelvin, forKey: Key.temperatureKelvin)
    }

    private func integer(forKey key: String) -> Int? {
        guard let number = defaults.object(forKey: key) as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
        let value = number.doubleValue
        guard value.isFinite, value.rounded(.towardZero) == value,
              value >= Double(Int32.min), value <= Double(Int32.max) else { return nil }
        return Int(value)
    }

    private func boolean(forKey key: String) -> Bool? {
        guard let number = defaults.object(forKey: key) as? NSNumber else { return nil }
        // CFPreferences can coalesce equal NSNumber(1) and Bool(true) writes without
        // changing the stored type. Accept only the canonical numeric/boolean 0 and 1.
        let value = number.doubleValue
        guard value == 0 || value == 1 else { return nil }
        return value == 1
    }
}

public struct RenderingError: LocalizedError {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

public enum CoreParameters {
    public static func make(
        preset: Preset, pixelSize: Int, temperatureKelvin: Int = 6500
    ) throws -> PSFilterParameters {
        guard PSPresetCount() == UInt32(Preset.allCases.count),
              MemoryLayout<PSFilterParameters>.size == 64,
              MemoryLayout<PSFilterParameters>.stride == 64,
              let size = Int32(exactly: pixelSize),
              let kelvin = Int32(exactly: temperatureKelvin) else {
            throw RenderingError("The shared filter ABI, pattern size, or temperature is invalid.")
        }
        var parameters = PSFilterParameters()
        guard PSGetWarmFilterParameters(preset.rawValue, size, kelvin, &parameters) == 1 else {
            throw RenderingError("The shared filter core rejected the selected preset, pattern size, or temperature.")
        }
        return parameters
    }

    public static func ditherValues(quantizer: Int32) throws -> [Int32] {
        try (0..<16).map { index in
            var value: Int32 = 0
            guard PSDitherValue(quantizer, Int32(index), &value) == 1 else {
                throw RenderingError("The shared filter core rejected a dither-table lookup.")
            }
            return value
        }
    }
}
