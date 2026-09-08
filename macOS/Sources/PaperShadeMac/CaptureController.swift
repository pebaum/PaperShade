import AppKit
import CoreGraphics
import Metal
import PaperShadeRendering
import ScreenCaptureKit

enum CaptureState {
    case paused, noEffect, starting, active(Int), suspended, failed(String), quitting

    var title: String {
        switch self {
        case .paused: return "Paused"
        case .noEffect: return "Enabled — original colors, no effect (no capture/GPU)"
        case .starting: return "Starting…"
        case .active(let count): return "Active — \(count) display\(count == 1 ? "" : "s")"
        case .suspended: return "Paused while the session or displays sleep"
        case .failed: return "Paused — see Error / Permission Info"
        case .quitting: return "Quitting…"
        }
    }
}

@MainActor
final class CaptureController {
    var onStateChange: ((CaptureState) -> Void)?
    var onFailure: ((String) -> Void)?
    private(set) var state: CaptureState = .paused {
        didSet { onStateChange?(state) }
    }
    private var settings = FilterSettings()
    private var requestedEnabled = false
    private var exiting = false
    private var machineSleeping = false
    private var displaysSleeping = false
    private var sessionActive = true
    private var generation: UInt64 = 0
    private var captures: [DisplayCapture] = []
    private var readyDisplays = Set<CGDirectDisplayID>()
    private var startTask: Task<Void, Never>?
    private var cleanupTask: Task<Void, Never>?
    private var debounce: DispatchWorkItem?
    private var firstFrameDeadline: DispatchWorkItem?
    private var observers: [(NotificationCenter, NSObjectProtocol)] = []

    init() {
        observe(NotificationCenter.default, NSApplication.didChangeScreenParametersNotification) { controller in
            controller.restartAfterEvent(delay: 0.4)
        }
        let workspace = NSWorkspace.shared.notificationCenter
        observe(workspace, NSWorkspace.willSleepNotification) { controller in
            controller.machineSleeping = true
            controller.suspend()
        }
        observe(workspace, NSWorkspace.screensDidSleepNotification) { controller in
            controller.displaysSleeping = true
            controller.suspend()
        }
        observe(workspace, NSWorkspace.didWakeNotification) { controller in
            controller.machineSleeping = false
            controller.restartAfterEvent(delay: 0.8)
        }
        observe(workspace, NSWorkspace.screensDidWakeNotification) { controller in
            controller.displaysSleeping = false
            controller.restartAfterEvent(delay: 0.8)
        }
        observe(workspace, NSWorkspace.sessionDidResignActiveNotification) { controller in
            controller.sessionActive = false
            controller.suspend()
        }
        observe(workspace, NSWorkspace.sessionDidBecomeActiveNotification) { controller in
            controller.sessionActive = true
            controller.restartAfterEvent(delay: 0.8)
        }
        observe(workspace, NSWorkspace.didActivateApplicationNotification) { controller in
            if case .suspended = controller.state,
               NSWorkspace.shared.frontmostApplication?.activationPolicy == .regular {
                controller.restartAfterEvent(delay: 0.4)
            }
        }
    }

    func apply(_ settings: FilterSettings, userInitiated: Bool) {
        guard !exiting else { return }
        self.settings = settings.validated()
        requestedEnabled = self.settings.enabled
        if requestedEnabled {
            begin(userInitiated: userInitiated)
        } else {
            invalidate()
            state = .paused
        }
    }

    func prepareToQuit() -> Task<Void, Never>? {
        guard !exiting else { return cleanupTask }
        exiting = true
        requestedEnabled = false
        invalidate()
        for (center, observer) in observers { center.removeObserver(observer) }
        observers.removeAll()
        state = .quitting
        return cleanupTask
    }

    private var canCapture: Bool {
        !exiting && requestedEnabled && settings.requiresCapture &&
            !machineSleeping && !displaysSleeping && sessionActive
    }

    private func isCurrent(_ token: UInt64) -> Bool {
        token == generation && canCapture
    }

    private func begin(userInitiated: Bool) {
        invalidate()
        // Invalidate old generations before bypassing consent, Metal, and ScreenCaptureKit.
        guard settings.hasEffect else { state = .noEffect; return }
        guard canCapture else { state = .suspended; return }
        let token = generation
        if !CGPreflightScreenCaptureAccess() {
            // Only an explicit enable/style/warmth action may trigger Apple's consent prompt.
            if userInitiated { _ = CGRequestScreenCaptureAccess() }
            guard isCurrent(token) else { return }
            guard CGPreflightScreenCaptureAccess() else {
                fail(Self.permissionInstructions)
                return
            }
        }
        state = .starting
        let previousCleanup = cleanupTask
        let settings = self.settings
        startTask = Task { [weak self] in
            await previousCleanup?.value
            guard let self, self.isCurrent(token), !Task.isCancelled else { return }
            do {
                try await self.startGeneration(token, settings: settings)
                if self.isCurrent(token) { self.startTask = nil }
            } catch {
                guard self.isCurrent(token) else { return }
                self.fail(error.localizedDescription)
            }
        }
    }

    private func startGeneration(_ token: UInt64, settings: FilterSettings) async throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw RenderingError("No Metal device is available. PaperShade needs a Metal-capable Mac.")
        }
        let renderer = try MetalRenderer(device: device)
        let displays = try DisplayDescriptor.current()
        guard !displays.isEmpty else { throw RenderingError("No active desktop displays were found.") }
        for display in displays {
            let capture = try DisplayCapture(
                display: display, renderer: renderer, settings: settings,
                onFirstFrame: { [weak self] in self?.firstFrame(displayID: display.id, token: token) },
                onFailure: { [weak self] message in
                    guard let self, self.isCurrent(token) else { return }
                    self.fail("\(display.screen.localizedName): \(message)")
                },
                onSuspension: { [weak self] in
                    guard let self, self.isCurrent(token) else { return }
                    self.suspend()
                }
            )
            captures.append(capture)
        }
        let timeout = DispatchWorkItem { [weak self] in
            guard let self, self.isCurrent(token), self.readyDisplays.count != self.captures.count else { return }
            self.fail("Screen capture did not produce a complete rendered frame within 12 seconds. " +
                "Check Screen Recording permission, then enable PaperShade again.")
        }
        firstFrameDeadline = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 12, execute: timeout)

        // Register every hidden overlay before enumerating apps, including offscreen windows.
        // Excluding the entire process is essential: sharingType alone is not sufficient.
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard isCurrent(token), !Task.isCancelled else { throw CancellationError() }
        guard let application = content.applications.first(where: {
            $0.processID == ProcessInfo.processInfo.processIdentifier
        }) else {
            throw RenderingError("PaperShade could not exclude its own application from capture. " +
                "No overlays were enabled; try again after reopening the app.")
        }
        let excludedApplications = content.applications.filter { candidate in
            candidate.processID == application.processID ||
                (Bundle.main.bundleIdentifier.map { identifier in candidate.bundleIdentifier == identifier } ?? false)
        }
        let currentCaptures = captures
        for capture in currentCaptures {
            guard isCurrent(token), !Task.isCancelled else { throw CancellationError() }
            guard let display = content.displays.first(where: { $0.displayID == capture.display.id }) else {
                throw RenderingError("Display topology changed during startup. Enable PaperShade again.")
            }
            try await capture.start(captureDisplay: display, excluding: excludedApplications)
            guard isCurrent(token), !Task.isCancelled else { throw CancellationError() }
        }
    }

    private func firstFrame(displayID: CGDirectDisplayID, token: UInt64) {
        guard isCurrent(token), let capture = captures.first(where: { $0.display.id == displayID }) else { return }
        capture.showFirstFrame()
        readyDisplays.insert(displayID)
        if readyDisplays.count == captures.count {
            firstFrameDeadline?.cancel()
            firstFrameDeadline = nil
            state = .active(captures.count)
        }
    }

    private func invalidate() {
        generation &+= 1
        debounce?.cancel()
        debounce = nil
        firstFrameDeadline?.cancel()
        firstFrameDeadline = nil
        let previousStart = startTask
        previousStart?.cancel()
        startTask = nil
        let previousCaptures = captures
        // Visual teardown is synchronous; never wait for an asynchronous ScreenCaptureKit stop.
        for capture in previousCaptures { capture.hideAndInvalidate() }
        captures.removeAll()
        readyDisplays.removeAll()
        guard previousStart != nil || !previousCaptures.isEmpty else { return }
        let previousCleanup = cleanupTask
        cleanupTask = Task {
            await previousCleanup?.value
            await previousStart?.value
            for capture in previousCaptures { await capture.stop() }
        }
    }

    private func fail(_ message: String) {
        requestedEnabled = false
        invalidate()
        state = .failed(message)
        onFailure?(message)
    }

    private func suspend() {
        guard !exiting else { return }
        invalidate()
        if requestedEnabled { state = settings.hasEffect ? .suspended : .noEffect }
    }

    private func restartAfterEvent(delay: TimeInterval) {
        guard !exiting, requestedEnabled else { return }
        invalidate()
        guard settings.hasEffect else { state = .noEffect; return }
        guard canCapture else { state = .suspended; return }
        state = .starting
        let token = generation
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.isCurrent(token) else { return }
            self.begin(userInitiated: false)
        }
        debounce = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    private func observe(
        _ center: NotificationCenter, _ name: Notification.Name,
        action: @escaping @MainActor (CaptureController) -> Void
    ) {
        let observer = center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
            DispatchQueue.main.async {
                guard let self, !self.exiting else { return }
                action(self)
            }
        }
        observers.append((center, observer))
    }

    static let permissionInstructions =
        "Active effects use ScreenCaptureKit + Metal and need macOS Screen Recording permission. " +
        "Original colors at 6500 K has no effect and uses no capture or GPU; it needs no permission. " +
        "Open System Settings → Privacy & Security → " +
        "Screen Recording (or Screen & System Audio Recording), enable PaperShade, then quit and reopen it " +
        "if macOS requests a restart. Permission is only requested when you explicitly enable PaperShade " +
        "with an effect, select a style or warmth, or choose Grant Screen Recording Access. " +
        "Apple's recording indicator remains visible during capture."
}
