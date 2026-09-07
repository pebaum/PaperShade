import Carbon
import Foundation

private let paperShadeHotKeySignature: OSType = 0x50534844

private let paperShadeHotKeyHandler: EventHandlerUPP = { _, event, context in
    guard let event, let context else { return OSStatus(eventNotHandledErr) }
    var identifier = EventHotKeyID()
    let status = GetEventParameter(
        event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID),
        nil, UInt32(MemoryLayout<EventHotKeyID>.size), nil, &identifier
    )
    guard status == noErr, identifier.signature == paperShadeHotKeySignature else {
        return OSStatus(eventNotHandledErr)
    }
    let controller = Unmanaged<HotKeyController>.fromOpaque(context).takeUnretainedValue()
    let id = identifier.id
    DispatchQueue.main.async { controller.handle(id) }
    return noErr
}

@MainActor
final class HotKeyController {
    var onToggle: (() -> Void)?
    var onPause: (() -> Void)?
    private(set) var problems: [String] = []
    private var references: [EventHotKeyRef] = []
    private var handler: EventHandlerRef?
    private var registered = false

    func register() {
        guard !registered else { return }
        registered = true
        var eventType = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed)
        )
        let status = InstallEventHandler(
            GetApplicationEventTarget(), paperShadeHotKeyHandler, 1, &eventType,
            Unmanaged.passUnretained(self).toOpaque(), &handler
        )
        guard status == noErr else {
            problems.append("Global shortcuts could not be installed (OSStatus \(status)). Use the menu controls.")
            return
        }
        register(id: 1, modifiers: UInt32(controlKey | optionKey), title: "Control–Option–G")
        register(id: 2, modifiers: UInt32(controlKey | optionKey | shiftKey), title: "Control–Option–Shift–G")
    }

    func unregister() {
        registered = false
        for reference in references { UnregisterEventHotKey(reference) }
        references.removeAll()
        if let handler { RemoveEventHandler(handler) }
        handler = nil
    }

    fileprivate func handle(_ identifier: UInt32) {
        guard registered else { return }
        switch identifier {
        case 1: onToggle?()
        case 2: onPause?()
        default: break
        }
    }

    private func register(id: UInt32, modifiers: UInt32, title: String) {
        var reference: EventHotKeyRef?
        let status = RegisterEventHotKey(
            UInt32(kVK_ANSI_G), modifiers,
            EventHotKeyID(signature: paperShadeHotKeySignature, id: id),
            GetApplicationEventTarget(), 0, &reference
        )
        if status == noErr, let reference {
            references.append(reference)
        } else {
            problems.append("\(title) is unavailable (OSStatus \(status)). Another app may own this shortcut; " +
                "change that app's shortcut or use PaperShade's menu.")
        }
    }
}
