import CoreFoundation
import FilterCore
import Foundation

public enum Preset: Int32, CaseIterable, Hashable {
    case natural = 0, classic, average, paper, highContrast, inverted
    case inkThreshold, inkDither, ink4, ink16, ps1Gray, ps1Color

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
        }
    }
}

public struct FilterSettings: Equatable {
    public static let frameCaps = [10, 15, 30, 60]
    public static let patternSizes = [1, 2, 3, 4]

    public var preset: Preset
    public var framesPerSecond: Int
    public var pixelSize: Int
    public var enabled: Bool

    public init(
        preset: Preset = .natural,
        framesPerSecond: Int = 15,
        pixelSize: Int = 1,
        enabled: Bool = false
    ) {
        if !Self.isValidFrameCap(framesPerSecond) || !Self.patternSizes.contains(pixelSize) {
            NSLog("PaperShade: invalid filter settings were normalized to supported defaults.")
        }
        self.preset = preset
        self.framesPerSecond = Self.isValidFrameCap(framesPerSecond) ? framesPerSecond : 15
        self.pixelSize = Self.patternSizes.contains(pixelSize) ? pixelSize : 1
        self.enabled = enabled
    }

    public static func isValidFrameCap(_ value: Int) -> Bool {
        guard let unsigned = UInt32(exactly: value) else { return false }
        return PSValidFrameCap(unsigned) == 1
    }

    public func validated() -> Self {
        Self(preset: preset, framesPerSecond: framesPerSecond, pixelSize: pixelSize, enabled: enabled)
    }
}

public final class SettingsStore {
    enum Key {
        static let preset = "preset"
        static let frameCap = "frameCap"
        static let pixelSize = "pixelSize"
        static let enabled = "enabled"
    }

    private let defaults: UserDefaults
    public private(set) var lastLoadWarning: String?

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public func load() -> FilterSettings {
        let rawPreset = integer(forKey: Key.preset).flatMap { Int32(exactly: $0) }
        let preset = rawPreset.flatMap(Preset.init(rawValue:)) ?? .natural
        let enabledNumber = defaults.object(forKey: Key.enabled) as? NSNumber
        let enabled = enabledNumber.map {
            CFGetTypeID($0) == CFBooleanGetTypeID() && $0.boolValue
        } ?? false
        let frameCap = integer(forKey: Key.frameCap)
        let pixelSize = integer(forKey: Key.pixelSize)
        let validFrameCap = frameCap.map(FilterSettings.isValidFrameCap) ?? false
        let validPixelSize = pixelSize.map { FilterSettings.patternSizes.contains($0) } ?? false
        let validEnabled = enabledNumber.map { CFGetTypeID($0) == CFBooleanGetTypeID() } ?? false
        let invalid = (defaults.object(forKey: Key.preset) != nil && rawPreset.flatMap(Preset.init(rawValue:)) == nil)
            || (defaults.object(forKey: Key.frameCap) != nil && !validFrameCap)
            || (defaults.object(forKey: Key.pixelSize) != nil && !validPixelSize)
            || (defaults.object(forKey: Key.enabled) != nil && !validEnabled)
        lastLoadWarning = invalid ? "Invalid saved preferences were repaired using supported defaults. PaperShade starts paused." : nil
        if let lastLoadWarning { NSLog("PaperShade: %@", lastLoadWarning) }
        return FilterSettings(
            preset: preset,
            framesPerSecond: frameCap ?? 15,
            pixelSize: pixelSize ?? 1,
            enabled: enabled && !invalid
        )
    }

    public func save(_ settings: FilterSettings) {
        let settings = settings.validated()
        defaults.set(settings.preset.rawValue, forKey: Key.preset)
        defaults.set(settings.framesPerSecond, forKey: Key.frameCap)
        defaults.set(settings.pixelSize, forKey: Key.pixelSize)
        defaults.set(settings.enabled, forKey: Key.enabled)
    }

    private func integer(forKey key: String) -> Int? {
        guard let number = defaults.object(forKey: key) as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
        let value = number.doubleValue
        guard value.isFinite, value.rounded(.towardZero) == value,
              value >= Double(Int32.min), value <= Double(Int32.max) else { return nil }
        return Int(value)
    }
}

public struct RenderingError: LocalizedError {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

public enum CoreParameters {
    public static func make(preset: Preset, pixelSize: Int) throws -> PSFilterParameters {
        guard PSPresetCount() == UInt32(Preset.allCases.count),
              MemoryLayout<PSFilterParameters>.size == 48,
              MemoryLayout<PSFilterParameters>.stride == 48,
              let size = Int32(exactly: pixelSize) else {
            throw RenderingError("The shared filter ABI or pattern size is invalid.")
        }
        var parameters = PSFilterParameters()
        guard PSGetFilterParameters(preset.rawValue, size, &parameters) == 1 else {
            throw RenderingError("The shared filter core rejected the selected preset or pattern size.")
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
