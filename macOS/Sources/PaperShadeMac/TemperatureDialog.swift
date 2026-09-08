import AppKit
import PaperShadeRendering

@MainActor
final class TemperatureDialog: NSObject, NSTextFieldDelegate {
    private let alert = NSAlert()
    private let slider: NSSlider
    private let entry: NSTextField
    private let validation = NSTextField(wrappingLabelWithString: "")

    init(temperatureKelvin: Int) {
        slider = NSSlider(
            value: Double(temperatureKelvin),
            minValue: Double(FilterSettings.temperatureRange.lowerBound),
            maxValue: Double(FilterSettings.temperatureRange.upperBound),
            target: nil, action: nil
        )
        entry = NSTextField(string: String(temperatureKelvin))
        super.init()
        alert.window.level = .statusBar
        alert.messageText = "Custom Warmth (Kelvin)"
        alert.informativeText =
            "1000–6500 K. Lower values warm the selected style; 6500 K turns warmth off. " +
            "Nothing changes until you choose OK."
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")

        let view = NSView(frame: NSRect(x: 0, y: 0, width: 360, height: 128))
        slider.frame = NSRect(x: 0, y: 96, width: 360, height: 24)
        slider.target = self
        slider.action = #selector(sliderChanged)
        slider.isContinuous = true
        slider.setAccessibilityLabel("Warmth in Kelvin")
        view.addSubview(slider)
        let lower = NSTextField(labelWithString: "1000 K — warmer")
        lower.frame = NSRect(x: 0, y: 76, width: 170, height: 18)
        view.addSubview(lower)
        let upper = NSTextField(labelWithString: "6500 K — neutral/off")
        upper.alignment = .right
        upper.frame = NSRect(x: 170, y: 76, width: 190, height: 18)
        view.addSubview(upper)
        let label = NSTextField(labelWithString: "Temperature:")
        label.frame = NSRect(x: 0, y: 42, width: 100, height: 22)
        view.addSubview(label)
        entry.frame = NSRect(x: 100, y: 42, width: 100, height: 24)
        entry.delegate = self
        entry.setAccessibilityLabel("Temperature in Kelvin, 1000 to 6500")
        view.addSubview(entry)
        let units = NSTextField(labelWithString: "K")
        units.frame = NSRect(x: 208, y: 42, width: 24, height: 22)
        view.addSubview(units)
        validation.frame = NSRect(x: 0, y: 0, width: 360, height: 34)
        validation.textColor = .systemRed
        view.addSubview(validation)
        alert.accessoryView = view
        alert.window.initialFirstResponder = entry
    }

    func runModal() -> Int? {
        // The dialog owns only a draft. Cancel never touches settings or capture.
        while alert.runModal() == .alertFirstButtonReturn {
            do {
                return try FilterSettings.parseTemperature(entry.stringValue)
            } catch {
                validation.stringValue = error.localizedDescription
                alert.window.makeFirstResponder(entry)
                entry.selectText(nil)
            }
        }
        return nil
    }

    @objc private func sliderChanged() {
        let value = Int(slider.doubleValue.rounded())
        slider.doubleValue = Double(value)
        entry.stringValue = String(value)
        validation.stringValue = ""
    }

    func controlTextDidChange(_ notification: Notification) {
        do {
            let value = try FilterSettings.parseTemperature(entry.stringValue)
            slider.doubleValue = Double(value)
            validation.stringValue = ""
        } catch {
            // Leave invalid text visible rather than clamping it through the slider.
            validation.stringValue = error.localizedDescription
        }
    }
}
