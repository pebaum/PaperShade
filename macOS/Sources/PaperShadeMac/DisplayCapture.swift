import CoreGraphics
import CoreMedia
import CoreVideo
import FilterCore
import Foundation
import Metal
import PaperShadeRendering
import QuartzCore
import ScreenCaptureKit

final class CaptureOutput: NSObject, SCStreamOutput, SCStreamDelegate {
    let frameQueue: DispatchQueue
    private let renderer: MetalRenderer
    private let cache: CVMetalTextureCache
    private let parameters: PSFilterParameters
    private let width: Int
    private let height: Int
    private let onFirstFrame: @MainActor () -> Void
    private let onFailure: @MainActor (String) -> Void
    private let onSuspension: @MainActor () -> Void
    private let lock = NSLock()
    private var layer: CAMetalLayer?
    private var active = true
    private var inFlight = false
    private var firstFrameDelivered = false

    init(
        display: DisplayDescriptor, layer: CAMetalLayer,
        renderer: MetalRenderer, settings: FilterSettings,
        onFirstFrame: @escaping @MainActor () -> Void,
        onFailure: @escaping @MainActor (String) -> Void,
        onSuspension: @escaping @MainActor () -> Void
    ) throws {
        self.frameQueue = DispatchQueue(label: "com.pebaum.PaperShade.frames.\(display.id)", qos: .userInitiated)
        self.width = display.width
        self.height = display.height
        self.layer = layer
        self.renderer = renderer
        self.cache = try renderer.makeTextureCache()
        self.parameters = try CoreParameters.make(preset: settings.preset, pixelSize: settings.pixelSize)
        self.onFirstFrame = onFirstFrame
        self.onFailure = onFailure
        self.onSuspension = onSuspension
    }

    func invalidate() {
        lock.lock()
        active = false
        layer = nil
        lock.unlock()
        let cache = self.cache
        frameQueue.async { CVMetalTextureCacheFlush(cache, 0) }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        fail("ScreenCaptureKit stopped: \(error.localizedDescription)")
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen else { return }
        autoreleasepool {
            lock.lock()
            let isActive = active
            lock.unlock()
            guard isActive else { return }
            guard CMSampleBufferIsValid(sampleBuffer), CMSampleBufferDataIsReady(sampleBuffer),
                  let attachments = CMSampleBufferGetSampleAttachmentsArray(
                    sampleBuffer, createIfNecessary: false
                  ) as? [[SCStreamFrameInfo: Any]],
                  let rawStatus = attachments.first?[.status] as? Int,
                  let status = SCFrameStatus(rawValue: rawStatus) else {
                fail("ScreenCaptureKit supplied an invalid frame or missing frame status.")
                return
            }
            switch status {
            case .complete: break
            case .idle, .started: return
            case .blank, .suspended:
                suspend()
                return
            case .stopped:
                fail("Screen capture stopped. The overlays were removed.")
                return
            @unknown default:
                fail("ScreenCaptureKit supplied an unsupported frame status.")
                return
            }
            guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer),
                  CVPixelBufferGetWidth(pixelBuffer) == width,
                  CVPixelBufferGetHeight(pixelBuffer) == height else {
                fail("The captured display size changed or a complete image was missing.")
                return
            }
            guard let layer = reserveFrame() else { return }
            do {
                let source = try CapturedTexture(pixelBuffer: pixelBuffer, cache: cache)
                guard let drawable = layer.nextDrawable() else {
                    throw RenderingError("The overlay could not obtain a drawable. Try enabling PaperShade again.")
                }
                guard let commandBuffer = renderer.commandQueue.makeCommandBuffer() else {
                    throw RenderingError("Metal could not allocate a frame command buffer.")
                }
                try renderer.encode(
                    source: source.texture, destination: drawable.texture,
                    parameters: parameters, commandBuffer: commandBuffer
                )
                // The IOSurface and CVMetalTexture must outlive asynchronous GPU sampling.
                commandBuffer.addCompletedHandler { [source, weak self] command in
                    withExtendedLifetime(source) {
                        self?.finishedFrame(error: command.status == .completed ? nil :
                            command.error?.localizedDescription ?? "The Metal command did not complete.")
                    }
                }
                commandBuffer.present(drawable)
                commandBuffer.commit()
            } catch {
                finishedFrame(error: error.localizedDescription)
            }
        }
    }

    private func reserveFrame() -> CAMetalLayer? {
        lock.lock()
        defer { lock.unlock() }
        guard active, !inFlight, let layer else { return nil }
        inFlight = true
        return layer
    }

    private func finishedFrame(error: String?) {
        lock.lock()
        inFlight = false
        let isActive = active
        let first = isActive && error == nil && !firstFrameDelivered
        if first { firstFrameDelivered = true }
        lock.unlock()
        guard isActive else { return }
        if let error {
            fail(error)
        } else if first {
            let callback = onFirstFrame
            // At most one first-frame notification per display, never one task per frame.
            DispatchQueue.main.async { callback() }
        }
    }

    private func fail(_ message: String) {
        lock.lock()
        guard active else { lock.unlock(); return }
        active = false
        layer = nil
        lock.unlock()
        let callback = onFailure
        DispatchQueue.main.async { callback(message) }
    }

    private func suspend() {
        lock.lock()
        guard active else { lock.unlock(); return }
        active = false
        layer = nil
        lock.unlock()
        let callback = onSuspension
        DispatchQueue.main.async { callback() }
    }
}

@MainActor
final class DisplayCapture {
    let display: DisplayDescriptor
    private let output: CaptureOutput
    private let settings: FilterSettings
    private var overlay: OverlayWindow?
    private var stream: SCStream?
    private var invalidated = false
    private var outputRegistered = false
    private var started = false

    init(
        display: DisplayDescriptor, renderer: MetalRenderer, settings: FilterSettings,
        onFirstFrame: @escaping @MainActor () -> Void,
        onFailure: @escaping @MainActor (String) -> Void,
        onSuspension: @escaping @MainActor () -> Void
    ) throws {
        self.display = display
        self.settings = settings
        let overlay = OverlayWindow(display: display, device: renderer.device)
        self.overlay = overlay
        do {
            self.output = try CaptureOutput(
                display: display, layer: overlay.layer, renderer: renderer, settings: settings,
                onFirstFrame: onFirstFrame, onFailure: onFailure, onSuspension: onSuspension
            )
        } catch {
            overlay.close()
            throw error
        }
    }

    func start(captureDisplay: SCDisplay, excluding application: SCRunningApplication) async throws {
        guard !invalidated, !Task.isCancelled else { throw CancellationError() }
        let configuration = SCStreamConfiguration()
        configuration.width = display.width
        configuration.height = display.height
        configuration.pixelFormat = kCVPixelFormatType_32BGRA
        configuration.colorSpaceName = CGColorSpace.sRGB
        configuration.showsCursor = false
        configuration.capturesAudio = false
        configuration.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(settings.framesPerSecond))
        configuration.queueDepth = 3
        let filter = SCContentFilter(
            display: captureDisplay, excludingApplications: [application], exceptingWindows: []
        )
        let stream = SCStream(filter: filter, configuration: configuration, delegate: output)
        self.stream = stream
        try stream.addStreamOutput(output, type: .screen, sampleHandlerQueue: output.frameQueue)
        outputRegistered = true
        overlay?.prepareForCapture()
        try await stream.startCapture()
        started = true
        guard !invalidated, !Task.isCancelled else {
            throw CancellationError()
        }
    }

    func showFirstFrame() {
        guard !invalidated else { return }
        overlay?.showRenderedFrame()
    }

    func hideAndInvalidate() {
        guard !invalidated else { return }
        invalidated = true
        output.invalidate()
        overlay?.close()
        overlay = nil
    }

    func stop() async {
        hideAndInvalidate()
        guard let stream else { return }
        self.stream = nil
        if outputRegistered {
            do {
                try stream.removeStreamOutput(output, type: .screen)
            } catch {
                NSLog("PaperShade capture-output cleanup: %@", error.localizedDescription)
            }
            outputRegistered = false
        }
        if started {
            do {
                try await stream.stopCapture()
            } catch {
                NSLog("PaperShade capture-stream cleanup: %@", error.localizedDescription)
            }
            started = false
        }
    }
}
