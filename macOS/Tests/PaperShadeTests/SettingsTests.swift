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
        XCTAssertFalse(value.enabled)
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
            defaults.set(12, forKey: SettingsStore.Key.preset)
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

    private func withStore(_ body: (SettingsStore, UserDefaults) throws -> Void) throws {
        let name = "com.pebaum.PaperShade.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        try body(SettingsStore(defaults: defaults), defaults)
    }
}
