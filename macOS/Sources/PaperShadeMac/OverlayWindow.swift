import AppKit
import CoreGraphics
import Metal
import PaperShadeRendering
import QuartzCore

struct DisplayDescriptor {
    let screen: NSScreen
    let id: CGDirectDisplayID
    let frame: NSRect
    let scale: CGFloat
    let width: Int
    let height: Int

    @MainActor
    static func current() throws -> [DisplayDescriptor] {
        try NSScreen.screens.map { screen in
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else {
                throw RenderingError("macOS did not identify an attached display.")
            }
            let pixels = screen.convertRectToBacking(NSRect(origin: .zero, size: screen.frame.size))
            let width = Int(pixels.width.rounded())
            let height = Int(pixels.height.rounded())
            guard width > 0, height > 0, width <= 16_384, height <= 16_384 else {
                throw RenderingError("The display's backing-pixel size is not supported by this Metal renderer.")
            }
            return DisplayDescriptor(
                screen: screen, id: number.uint32Value, frame: screen.frame,
                scale: screen.backingScaleFactor, width: width, height: height
            )
        }
    }
}

private final class ShadePanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

@MainActor
final class OverlayWindow {
    let layer: CAMetalLayer
    private var panel: ShadePanel?

    init(display: DisplayDescriptor, device: MTLDevice) {
        let layer = CAMetalLayer()
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        layer.framebufferOnly = true
        layer.isOpaque = false
        layer.frame = NSRect(origin: .zero, size: display.frame.size)
        layer.contentsScale = display.scale
        layer.drawableSize = CGSize(width: CGFloat(display.width), height: CGFloat(display.height))
        layer.maximumDrawableCount = 2
        layer.allowsNextDrawableTimeout = true
        layer.displaySyncEnabled = true
        layer.presentsWithTransaction = false
        layer.actions = ["bounds": NSNull(), "position": NSNull(), "contents": NSNull()]
        self.layer = layer

        let panel = ShadePanel(
            contentRect: display.frame,
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered, defer: false, screen: display.screen
        )
        panel.title = "PaperShade overlay \(display.id)"
        panel.isReleasedWhenClosed = false
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.ignoresMouseEvents = true
        panel.acceptsMouseMovedEvents = false
        panel.isMovable = false
        panel.hasShadow = false
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.animationBehavior = .none
        // Stay below system menus/permission UI; do not conceal the privacy indicator.
        panel.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.mainMenuWindow)) - 1)
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        // Keep windows enumerable so ScreenCaptureKit can exclude this application.
        // Self-exclusion is configured on the capture filter, not through sharingType.
        let view = NSView(frame: NSRect(origin: .zero, size: display.frame.size))
        view.wantsLayer = true
        view.layer = layer
        view.layerContentsRedrawPolicy = .never
        panel.contentView = view
        panel.setFrame(display.frame, display: false)
        _ = panel.windowNumber
        panel.orderOut(nil)
        self.panel = panel
    }

    func showRenderedFrame() {
        panel?.orderFrontRegardless()
    }

    func prepareForCapture() {
        // An empty transparent layer must be attached to an ordered window before Metal
        // can allocate onscreen drawables. It covers nothing until the first frame presents.
        panel?.orderFrontRegardless()
    }

    func close() {
        panel?.orderOut(nil)
        panel?.contentView?.layer = nil
        panel?.contentView = nil
        panel?.close()
        panel = nil
    }
}
