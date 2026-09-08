import FilterCore
import Foundation
@testable import PaperShadeRendering
import XCTest

final class SettingsTests: XCTestCase {
    func testDefaultSettingsStartPausedAt15FPS() {
        let value = FilterSettings()
        XCTAssertEqual(value.preset, .natural)
        XCTAssertEqual(value.framesPerSecond, 15)
        XCTAssertEqual(value.pixelSize, 1)
        XCTAssertEqual(value.temperatureKelvin, 6500)
        XCTAssertFalse(value.enabled)
        XCTAssertFalse(value.requiresCapture)
    }

    func testFrameCapValidationUsesCore() {
        for value in -1...120 {
            XCTAssertEqual(FilterSettings.isValidFrameCap(value), [10, 15, 30, 60].contains(value))
            if let unsigned = UInt32(exactly: value) {
                XCTAssertEqual(PSValidFrameCap(unsigned) == 1, FilterSettings.isValidFrameCap(value))
            }
        }
        XCTAssertFalse(FilterSettings.isValidFrameCap(Int.max))
        XCTAssertFalse(FilterSettings.isValidFrameCap(Int.min))
        XCTAssertEqual(FilterSettings(framesPerSecond: Int.max, pixelSize: -1).framesPerSecond, 15)
        XCTAssertEqual(FilterSettings(framesPerSecond: 0, pixelSize: 5).pixelSize, 1)
    }

    func testPreferenceRoundTrips() throws {
        try withStore { store, _ in
            XCTAssertEqual(store.load(), FilterSettings())
            for preset in Preset.allCases {
                for fps in FilterSettings.frameCaps {
                    for size in FilterSettings.patternSizes {
                        let settings = FilterSettings(preset: preset, framesPerSecond: fps, pixelSize: size, enabled: true)
                        store.save(settings)
                        XCTAssertEqual(store.load(), settings)
                    }
                }
            }
            store.save(FilterSettings(enabled: false))
            XCTAssertFalse(store.load().enabled)
        }
    }

    func testTemperatureValidationUsesCore() {
        XCTAssertEqual(FilterSettings.temperatureRange, 1000...6500)
        XCTAssertEqual(FilterSettings.temperaturePresets, [6500, 5500, 4500, 3500, 2700, 2000, 1200])
        for value in -1...6600 {
            XCTAssertEqual(FilterSettings.isValidTemperature(value), (1000...6500).contains(value))
            if let unsigned = UInt32(exactly: value) {
                XCTAssertEqual(PSValidTemperature(unsigned) == 1, FilterSettings.isValidTemperature(value))
            }
        }
        XCTAssertFalse(FilterSettings.isValidTemperature(Int.min))
        XCTAssertFalse(FilterSettings.isValidTemperature(Int.max))
        XCTAssertEqual(PSValidTemperature(UInt32.max), 0)
        for value in [Int.min, -1, 999, 6501, Int.max] {
            let settings = FilterSettings(enabled: true, temperatureKelvin: value)
            XCTAssertEqual(settings.temperatureKelvin, 6500)
            XCTAssertFalse(settings.enabled)
        }
    }

    func testTemperatureRoundTripsWithEveryPreset() throws {
        try withStore { store, _ in
            for preset in Preset.allCases {
                for kelvin in FilterSettings.temperaturePresets + [1000, 3333, 6499] {
                    for enabled in [false, true] {
                        let settings = FilterSettings(
                            preset: preset, framesPerSecond: 30, pixelSize: 3,
                            enabled: enabled, temperatureKelvin: kelvin
                        )
                        store.save(settings)
                        XCTAssertEqual(store.load(), settings)
                        XCTAssertNil(store.lastLoadWarning)
                    }
                }
            }
        }
    }

    func testMissingTemperaturePreservesLegacyPreferencesWithoutWarning() throws {
        try withStore { store, defaults in
            for rawPreset in 0...11 {
                let preset = try XCTUnwrap(Preset(rawValue: Int32(rawPreset)))
                defaults.set(rawPreset, forKey: SettingsStore.Key.preset)
                defaults.set(30, forKey: SettingsStore.Key.frameCap)
                defaults.set(4, forKey: SettingsStore.Key.pixelSize)
                defaults.set(true, forKey: SettingsStore.Key.enabled)
                let loaded = store.load()
                XCTAssertEqual(loaded, FilterSettings(preset: preset, framesPerSecond: 30, pixelSize: 4, enabled: true))
                XCTAssertEqual(loaded.temperatureKelvin, 6500)
                XCTAssertNil(store.lastLoadWarning)
                XCTAssertNil(defaults.object(forKey: SettingsStore.Key.temperatureKelvin))
                XCTAssertEqual(defaults.integer(forKey: SettingsStore.Key.preset), rawPreset)
                XCTAssertEqual(defaults.integer(forKey: SettingsStore.Key.frameCap), 30)
                XCTAssertEqual(defaults.integer(forKey: SettingsStore.Key.pixelSize), 4)
                XCTAssertTrue(defaults.bool(forKey: SettingsStore.Key.enabled))
            }
        }
    }

    func testMalformedTemperaturesPauseAndPreserveOtherPreferences() throws {
        try withStore { store, defaults in
            let good = FilterSettings(
                preset: .ps1Color, framesPerSecond: 30, pixelSize: 4, enabled: true, temperatureKelvin: 2700
            )
            let invalidValues: [Any] = [
                "3500", true, false, -1, 0, 999, 6501, 3500.5,
                NSNumber(value: Double.infinity), NSNumber(value: Double.nan), Int64.max,
                [3500], ["kelvin": 3500], Data([1, 2, 3])
            ]
            for invalid in invalidValues {
                store.save(good)
                defaults.set(invalid, forKey: SettingsStore.Key.temperatureKelvin)
                XCTAssertEqual(store.load(), FilterSettings(preset: .ps1Color, framesPerSecond: 30, pixelSize: 4))
                XCTAssertNotNil(store.lastLoadWarning, "Malformed temperature: \(invalid)")
            }
            store.save(good)
            XCTAssertEqual(store.load(), good)
            XCTAssertNil(store.lastLoadWarning)
        }
    }

    func testTemperatureEntryRejectsInvalidTextInsteadOfClamping() throws {
        for (text, expected) in [("1000", 1000), (" 3500\n", 3500), ("3333", 3333), ("6500", 6500)] {
            XCTAssertEqual(try FilterSettings.parseTemperature(text), expected)
        }
        for text in ["", " ", "3500 K", "3500.0", "1e3", "NaN", "inf", "3,500", "999", "6501",
                     "-3500", String(Int.max), "999999999999999999999999"] {
            XCTAssertThrowsError(try FilterSettings.parseTemperature(text), text)
        }
    }

    func testApplyingWarmthEnablesButNeutralPreservesPause() throws {
        for preset in Preset.allCases {
            var settings = FilterSettings(preset: preset)
            try settings.applyTemperature(6500)
            XCTAssertFalse(settings.enabled)
            try settings.applyTemperature(3500)
            XCTAssertTrue(settings.enabled)
            XCTAssertEqual(settings.preset, preset)
            XCTAssertEqual(settings.temperatureKelvin, 3500)
            try settings.applyTemperature(6500)
            XCTAssertTrue(settings.enabled)
            settings.enabled = false
            try settings.applyTemperature(6500)
            XCTAssertFalse(settings.enabled)
        }
    }

    func testInvalidTemperatureSelectionDoesNotChangeSettings() {
        let original = FilterSettings(preset: .ink4, framesPerSecond: 30, pixelSize: 2, temperatureKelvin: 2700)
        for invalid in [Int.min, -1, 999, 6501, Int.max] {
            var settings = original
            XCTAssertThrowsError(try settings.applyTemperature(invalid))
            XCTAssertEqual(settings, original)
        }
    }

    func testWarmthOnlyUsesOriginalAndRetainsAnExistingWarmTemperature() {
        var settings = FilterSettings()
        settings.enableWarmthOnly()
        XCTAssertEqual(settings.preset, .original)
        XCTAssertEqual(settings.temperatureKelvin, 3500)
        XCTAssertTrue(settings.enabled)
        XCTAssertTrue(settings.requiresCapture)
        settings = FilterSettings(preset: .ps1Gray, framesPerSecond: 30, pixelSize: 4, temperatureKelvin: 2700)
        settings.enableWarmthOnly()
        XCTAssertEqual(settings, FilterSettings(
            preset: .original, framesPerSecond: 30, pixelSize: 4, enabled: true, temperatureKelvin: 2700
        ))
    }

    func testOnlyEnabledEffectsRequireCapture() {
        for preset in Preset.allCases {
            for kelvin in FilterSettings.temperaturePresets + [1000] {
                for enabled in [false, true] {
                    let settings = FilterSettings(preset: preset, enabled: enabled, temperatureKelvin: kelvin)
                    let effect = preset != .original || kelvin != 6500
                    XCTAssertEqual(settings.hasEffect, effect)
                    XCTAssertEqual(settings.requiresCapture, enabled && effect)
                    XCTAssertEqual(settings.enabled, enabled)
                }
            }
        }
        let neutralOriginal = FilterSettings(preset: .original, enabled: true)
        XCTAssertFalse(neutralOriginal.requiresCapture)
        XCTAssertNotEqual(neutralOriginal, FilterSettings(preset: .original, enabled: false))
    }

    func testMalformedPreferencesFallBackIndependently() throws {
        try withStore { store, defaults in
            let invalidNumbers: [Any] = ["15", true, -1, 0.5, NSNumber(value: Double.infinity), Int64.max]
            for invalid in invalidNumbers {
                defaults.set(invalid, forKey: SettingsStore.Key.preset)
                defaults.set(invalid, forKey: SettingsStore.Key.frameCap)
                defaults.set(invalid, forKey: SettingsStore.Key.pixelSize)
                defaults.set("true", forKey: SettingsStore.Key.enabled)
                XCTAssertEqual(store.load(), FilterSettings())
            }
            defaults.set(13, forKey: SettingsStore.Key.preset)
            defaults.set(24, forKey: SettingsStore.Key.frameCap)
            defaults.set(5, forKey: SettingsStore.Key.pixelSize)
            defaults.set(1, forKey: SettingsStore.Key.enabled)
            XCTAssertEqual(store.load(), FilterSettings())
            defaults.set(11, forKey: SettingsStore.Key.preset)
            defaults.set(30, forKey: SettingsStore.Key.frameCap)
            defaults.set(4, forKey: SettingsStore.Key.pixelSize)
            defaults.set(true, forKey: SettingsStore.Key.enabled)
            XCTAssertEqual(store.load(), FilterSettings(preset: .ps1Color, framesPerSecond: 30, pixelSize: 4, enabled: true))
        }
    }

    func testMutatedInvalidSettingsAreSanitizedOnSave() throws {
        try withStore { store, _ in
            var settings = FilterSettings(preset: .ink4, enabled: true)
            settings.framesPerSecond = Int.min
            settings.pixelSize = Int.max
            store.save(settings)
            XCTAssertEqual(store.load(), FilterSettings(preset: .ink4, enabled: true))
        }
    }

    func testMutatedInvalidTemperatureSavesNeutralAndPaused() throws {
        try withStore { store, _ in
            var settings = FilterSettings(preset: .original, enabled: true, temperatureKelvin: 3500)
            settings.temperatureKelvin = Int.max
            store.save(settings)
            XCTAssertEqual(store.load(), FilterSettings(preset: .original))
        }
    }

    func testMalformedSavedPreferencesPauseAndReportWarning() throws {
        try withStore { store, defaults in
            defaults.set(true, forKey: SettingsStore.Key.enabled)
            defaults.set(144, forKey: SettingsStore.Key.frameCap)
            let settings = store.load()
            XCTAssertFalse(settings.enabled)
            XCTAssertEqual(settings.framesPerSecond, 15)
            XCTAssertNotNil(store.lastLoadWarning)
            store.save(FilterSettings())
            XCTAssertEqual(store.load(), FilterSettings())
            XCTAssertNil(store.lastLoadWarning)
        }
    }

    func testBooleanAndNumericZeroOneRemainInteroperable() throws {
        try withStore { store, defaults in
            store.save(FilterSettings())
            defaults.set(1, forKey: SettingsStore.Key.enabled)
            defaults.set(true, forKey: SettingsStore.Key.enabled)
            XCTAssertTrue(store.load().enabled)
            XCTAssertNil(store.lastLoadWarning)
            defaults.set(0, forKey: SettingsStore.Key.enabled)
            XCTAssertFalse(store.load().enabled)
            XCTAssertNil(store.lastLoadWarning)
            defaults.set(2, forKey: SettingsStore.Key.enabled)
            XCTAssertFalse(store.load().enabled)
            XCTAssertNotNil(store.lastLoadWarning)
        }
    }

    private func withStore(_ body: (SettingsStore, UserDefaults) throws -> Void) throws {
        let name = "com.pebaum.PaperShade.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        try body(SettingsStore(defaults: defaults), defaults)
    }
}
