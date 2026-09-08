import AppKit
import CoreGraphics
import PaperShadeRendering
import ServiceManagement

@MainActor
final class AppController: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let store: SettingsStore
    private let smokeTest: Bool
    private var settings: FilterSettings
    private var capture: CaptureController?
    private var hotKeys: HotKeyController?
    private var statusItem: NSStatusItem?
    private var statusLine: NSMenuItem?
    private var toggleItem: NSMenuItem?
    private var permissionItem: NSMenuItem?
    private var infoItem: NSMenuItem?
    private var loginItem: NSMenuItem?
    private var shortcutWarning: NSMenuItem?
    private var warmthStatusLine: NSMenuItem?
    private var customWarmthItem: NSMenuItem?
    private var presetItems: [Preset: NSMenuItem] = [:]
    private var frameItems: [Int: NSMenuItem] = [:]
    private var patternItems: [Int: NSMenuItem] = [:]
    private var warmthItems: [Int: NSMenuItem] = [:]
    private var lastError: String?
    private var terminating = false
    private var repliedToTermination = false

    static var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "development"
    }

    init(smokeTest: Bool = false) {
        let store = SettingsStore()
        self.store = store
        self.settings = store.load()
        self.smokeTest = smokeTest
        super.init()
        lastError = store.lastLoadWarning
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard !smokeTest else { return }
        buildMenu()
        let capture = CaptureController()
        capture.onStateChange = { [weak self] _ in self?.refreshMenu() }
        capture.onFailure = { [weak self] message in
            guard let self else { return }
            self.lastError = message
            self.settings.enabled = false
            self.store.save(self.settings)
            self.refreshMenu()
        }
        self.capture = capture
        let hotKeys = HotKeyController()
        hotKeys.onToggle = { [weak self] in self?.toggle() }
        hotKeys.onPause = { [weak self] in self?.pause() }
        hotKeys.register()
        self.hotKeys = hotKeys
        refreshMenu()
        // A fresh profile defaults to paused; restoring opt-in never triggers a consent prompt.
        capture.apply(settings, userInitiated: false)
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard !terminating else { return .terminateNow }
        terminating = true
        hotKeys?.unregister()
        let cleanup = capture?.prepareToQuit()
        removeStatusItem()
        Task {
            await cleanup?.value
            finishTermination()
        }
        // An unresponsive OS capture call must not prevent quitting; visual teardown is already done.
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in self?.finishTermination() }
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        hotKeys?.unregister()
        _ = capture?.prepareToQuit()
        removeStatusItem()
    }

    func menuWillOpen(_ menu: NSMenu) {
        refreshMenu()
    }

    func runSmokeTest() throws {
        let bundle = Bundle.main
        guard bundle.bundleURL.pathExtension == "app",
              bundle.bundleIdentifier == "com.pebaum.PaperShade",
              bundle.object(forInfoDictionaryKey: "CFBundleExecutable") as? String == "PaperShade",
              bundle.object(forInfoDictionaryKey: "CFBundlePackageType") as? String == "APPL",
              bundle.object(forInfoDictionaryKey: "LSUIElement") as? Bool == true,
              bundle.object(forInfoDictionaryKey: "LSMinimumSystemVersion") as? String == "13.0",
              !(bundle.object(forInfoDictionaryKey: "NSScreenCaptureUsageDescription") as? String ?? "").isEmpty,
              Self.version != "development",
              bundle.url(forResource: "PaperShade", withExtension: "icns") != nil else {
            throw RenderingError("The app bundle metadata or icon is incomplete. Run macOS/build.sh first.")
        }
        buildMenu()
        defer { removeStatusItem() }
        guard statusItem?.menu != nil,
              presetItems.count == 13,
              Set(frameItems.keys) == Set(FilterSettings.frameCaps),
              Set(patternItems.keys) == Set(FilterSettings.patternSizes),
              Set(warmthItems.keys) == Set(FilterSettings.temperaturePresets),
              customWarmthItem != nil,
              capture == nil, hotKeys == nil else {
            throw RenderingError("The menu-only smoke test did not initialize as expected.")
        }
        print("PaperShade \(Self.version): bundle metadata and menu initialization passed; no capture requested.")
    }

    private func buildMenu() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let image = NSImage(systemSymbolName: "circle.lefthalf.filled", accessibilityDescription: "PaperShade") {
            image.isTemplate = true
            item.button?.image = image
        } else {
            item.button?.title = "◐"
        }
        statusItem = item
        let menu = NSMenu()
        menu.autoenablesItems = false
        menu.delegate = self
        statusLine = append("Paused", to: menu)
        statusLine?.isEnabled = false
        menu.addItem(.separator())
        toggleItem = append("Enable PaperShade  ⌃⌥G", action: #selector(toggle), to: menu)
        append("Always Pause  ⌃⌥⇧G", action: #selector(pause), to: menu)
        shortcutWarning = append("Shortcut registration problem — see Error Info", to: menu)
        shortcutWarning?.isEnabled = false
        menu.addItem(.separator())

        let styles = submenu("Style", in: menu)
        for preset in Preset.allCases {
            if preset == .inkThreshold || preset == .ps1Gray || preset == .original { styles.addItem(.separator()) }
            let choice = append(preset.title, action: #selector(selectPreset(_:)), to: styles)
            choice.tag = Int(preset.rawValue)
            presetItems[preset] = choice
        }
        append("Warmth only (original colors)", action: #selector(enableWarmthOnly), to: menu)
        let warmth = submenu("Warmth (Kelvin)", in: menu)
        warmthStatusLine = append("", to: warmth)
        warmthStatusLine?.isEnabled = false
        warmth.addItem(.separator())
        for kelvin in FilterSettings.temperaturePresets {
            let choice = append(
                kelvin == FilterSettings.neutralTemperatureKelvin ? "\(kelvin) K — neutral/off" : "\(kelvin) K",
                action: #selector(selectTemperature(_:)), to: warmth
            )
            choice.tag = kelvin
            warmthItems[kelvin] = choice
        }
        warmth.addItem(.separator())
        customWarmthItem = append("Custom Temperature…", action: #selector(customTemperature), to: warmth)
        let fps = submenu("Frame Rate", in: menu)
        for cap in FilterSettings.frameCaps {
            let choice = append("\(cap) fps\(cap == 15 ? " (default)" : "")",
                                action: #selector(selectFrameCap(_:)), to: fps)
            choice.tag = cap
            frameItems[cap] = choice
        }
        let patterns = submenu("Dither Pattern Size", in: menu)
        for size in FilterSettings.patternSizes {
            let choice = append(size == 1 ? "1 px — accurate PS1 at 6500 K" : "\(size) px — enlarged artistic pattern",
                                action: #selector(selectPattern(_:)), to: patterns)
            choice.tag = size
            patternItems[size] = choice
        }
        append("Effects use GPU capture; Original at 6500 K does not", to: menu).isEnabled = false
        menu.addItem(.separator())
        loginItem = append("Start at Login", action: #selector(toggleLogin), to: menu)
        append("Open Login Items Settings…", action: #selector(openLoginSettings), to: menu)
        permissionItem = append("Grant Screen Recording Access…", action: #selector(grantPermission), to: menu)
        append("Open Screen Recording Settings…", action: #selector(openRecordingSettings), to: menu)
        infoItem = append("Permission & Privacy Info…", action: #selector(showInfo), to: menu)
        append("About PaperShade…", action: #selector(showAbout), to: menu)
        menu.addItem(.separator())
        append("Quit PaperShade", action: #selector(quit), to: menu)
        item.menu = menu
        refreshMenu()
    }

    @discardableResult
    private func append(_ title: String, action: Selector? = nil, to menu: NSMenu) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        menu.addItem(item)
        return item
    }

    private func submenu(_ title: String, in menu: NSMenu) -> NSMenu {
        let child = NSMenu(title: title)
        child.autoenablesItems = false
        append(title, to: menu).submenu = child
        return child
    }

    private func refreshMenu() {
        let title = capture?.state.title ?? "Paused"
        let temperature = "\(settings.temperatureKelvin) K" +
            (settings.temperatureKelvin == FilterSettings.neutralTemperatureKelvin ? " (neutral/off)" : "")
        statusLine?.title = "\(title) — \(temperature)"
        statusItem?.button?.toolTip = "PaperShade — \(title)\n\(settings.preset.title) — \(temperature)"
        toggleItem?.title = settings.enabled ? "Pause PaperShade  ⌃⌥G" : "Enable PaperShade  ⌃⌥G"
        for (preset, item) in presetItems { item.state = preset == settings.preset ? .on : .off }
        for (fps, item) in frameItems { item.state = fps == settings.framesPerSecond ? .on : .off }
        for (size, item) in patternItems { item.state = size == settings.pixelSize ? .on : .off }
        for (kelvin, item) in warmthItems { item.state = kelvin == settings.temperatureKelvin ? .on : .off }
        warmthStatusLine?.title = "Current: \(temperature)"
        let custom = !FilterSettings.temperaturePresets.contains(settings.temperatureKelvin)
        customWarmthItem?.state = custom ? .on : .off
        customWarmthItem?.title = custom ? "Custom Temperature… (\(settings.temperatureKelvin) K)" : "Custom Temperature…"
        let hasShortcutProblems = !(hotKeys?.problems.isEmpty ?? true)
        shortcutWarning?.isHidden = !hasShortcutProblems
        infoItem?.title = lastError != nil || hasShortcutProblems ? "Error / Permission Info…" : "Permission & Privacy Info…"
        if !smokeTest {
            permissionItem?.isEnabled = !CGPreflightScreenCaptureAccess()
            let status = SMAppService.mainApp.status
            loginItem?.state = status == .enabled ? .on : status == .requiresApproval ? .mixed : .off
            loginItem?.isEnabled = Bundle.main.bundleURL.pathExtension == "app"
            loginItem?.toolTip = status == .requiresApproval ?
                "Approval is required in System Settings → General → Login Items." :
                "Explicit opt-in using Apple's public login-item service."
        }
    }

    @objc private func toggle() {
        guard !terminating else { return }
        settings.enabled.toggle()
        lastError = nil
        saveAndApply(userInitiated: true)
    }

    @objc private func pause() {
        guard !terminating else { return }
        settings.enabled = false
        saveAndApply(userInitiated: true)
    }

    @objc private func selectPreset(_ sender: NSMenuItem) {
        guard let raw = Int32(exactly: sender.tag), let preset = Preset(rawValue: raw) else {
            lastError = "The selected preset is invalid."
            refreshMenu()
            return
        }
        settings.preset = preset
        settings.enabled = true
        saveAndApply(userInitiated: true)
    }

    @objc private func enableWarmthOnly() {
        guard !terminating else { return }
        settings.enableWarmthOnly()
        lastError = nil
        saveAndApply(userInitiated: true)
    }

    @objc private func selectTemperature(_ sender: NSMenuItem) {
        applyTemperature(sender.tag)
    }

    @objc private func customTemperature() {
        guard !terminating else { return }
        let dialog = TemperatureDialog(temperatureKelvin: settings.temperatureKelvin)
        guard let value = dialog.runModal(), !terminating else { return }
        applyTemperature(value)
    }

    private func applyTemperature(_ value: Int) {
        guard !terminating else { return }
        do {
            try settings.applyTemperature(value)
            lastError = nil
            saveAndApply(userInitiated: true)
        } catch {
            lastError = error.localizedDescription
            refreshMenu()
            showInfo()
        }
    }

    @objc private func selectFrameCap(_ sender: NSMenuItem) {
        guard FilterSettings.isValidFrameCap(sender.tag) else {
            lastError = "The selected frame cap is invalid."
            refreshMenu()
            return
        }
        settings.framesPerSecond = sender.tag
        saveAndApply(userInitiated: false)
    }

    @objc private func selectPattern(_ sender: NSMenuItem) {
        guard FilterSettings.patternSizes.contains(sender.tag) else {
            lastError = "The selected pattern size is invalid."
            refreshMenu()
            return
        }
        settings.pixelSize = sender.tag
        saveAndApply(userInitiated: false)
    }

    private func saveAndApply(userInitiated: Bool) {
        guard !terminating else { return }
        settings = settings.validated()
        store.save(settings)
        capture?.apply(settings, userInitiated: userInitiated)
        refreshMenu()
    }

    @objc private func grantPermission() {
        guard !terminating else { return }
        let granted = CGRequestScreenCaptureAccess()
        if !granted { lastError = CaptureController.permissionInstructions }
        refreshMenu()
        if !granted { showInfo() }
    }

    @objc private func openRecordingSettings() {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"),
              NSWorkspace.shared.open(url) else {
            lastError = "System Settings could not be opened. Open Privacy & Security > Screen Recording manually."
            refreshMenu()
            return
        }
    }

    @objc private func toggleLogin() {
        do {
            let service = SMAppService.mainApp
            if service.status == .enabled || service.status == .requiresApproval {
                try service.unregister()
            } else {
                try service.register()
                if service.status == .requiresApproval {
                    lastError = "Start at Login requires approval in System Settings → General → Login Items."
                }
            }
        } catch {
            lastError = "Start at Login could not be changed: \(error.localizedDescription). " +
                "Use a built PaperShade.app installed in Applications."
        }
        refreshMenu()
        if lastError != nil { showInfo() }
    }

    @objc private func openLoginSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }

    @objc private func showInfo() {
        let alert = NSAlert()
        // Our application is excluded from capture, so its dialogs must sit above the overlays.
        alert.window.level = .statusBar
        alert.messageText = "PaperShade — Permission & Error Info"
        var sections = [String]()
        if let lastError { sections.append(lastError) }
        sections.append(contentsOf: hotKeys?.problems ?? [])
        sections.append(CaptureController.permissionInstructions)
        sections.append(
            "Processing is local and SDR-only. No audio is captured, no frames are saved, and no networking " +
            "or telemetry is used. Protected video and secure/system surfaces may be blank or unfiltered. " +
            "Other screen-sharing apps can still capture these overlays."
        )
        alert.informativeText = sections.joined(separator: "\n\n")
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Open Screen Recording Settings")
        if alert.runModal() == .alertSecondButtonReturn { openRecordingSettings() }
    }

    @objc private func showAbout() {
        let alert = NSAlert()
        alert.window.level = .statusBar
        alert.messageText = "PaperShade \(Self.version)"
        alert.informativeText =
            "Native AppKit menu-bar app for macOS 13+.\nScreenCaptureKit + Metal; no Electron or WebView.\n\n" +
            "13 styles with manual 1000–6500 K warmth. Current: \(settings.temperatureKelvin) K. " +
            "6500 K is neutral/off. Warmth only keeps the original colors without grayscale. " +
            "Original colors at 6500 K uses no capture, GPU, or Screen Recording permission.\n\n" +
            "At 6500 K, PS1 uses the original signed 4×4 dither and RGB555 bit replication. " +
            "1 px is pixel-accurate; 2–4 px enlarge only the pattern. Warmth tints the final palette, " +
            "so warmed output is not byte-exact RGB555. This is a post-filter white balance effect, " +
            "not automatic sunset scheduling or monitor calibration.\n\n" +
            "15 fps by default; higher frame rates use more GPU power. This is an SDR capture overlay, " +
            "not a system-wide color transform. The live cursor and some system UI remain unfiltered. " +
            "HDR/EDR, protected content, and color-managed applications may not match the source exactly.\n\n" +
            "Local only: no network, telemetry, audio capture, or saved frames. Apple's recording indicator is retained during capture."
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    private func finishTermination() {
        guard !repliedToTermination else { return }
        repliedToTermination = true
        NSApp.reply(toApplicationShouldTerminate: true)
    }

    private func removeStatusItem() {
        if let statusItem { NSStatusBar.system.removeStatusItem(statusItem) }
        statusItem = nil
    }
}
