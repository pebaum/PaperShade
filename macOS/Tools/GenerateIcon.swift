import AppKit
import Foundation

guard CommandLine.arguments.count == 2 else {
    fputs("Usage: GenerateIcon.swift output.iconset\n", stderr)
    exit(2)
}
let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!

func gray(_ value: CGFloat, alpha: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: colorSpace, components: [value, value, value, alpha])!
}

func render(size: Int) throws -> Data {
    guard let bitmap = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8,
        samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
        bytesPerRow: 0, bitsPerPixel: 0
    ), let graphics = NSGraphicsContext(bitmapImageRep: bitmap) else {
        throw NSError(domain: "PaperShade.Icon", code: 1, userInfo: [NSLocalizedDescriptionKey: "Cannot allocate icon bitmap"])
    }
    let context = graphics.cgContext
    context.scaleBy(x: CGFloat(size) / 512, y: CGFloat(size) / 512)
    context.setFillColor(gray(0.12))
    context.addPath(CGPath(roundedRect: CGRect(x: 22, y: 22, width: 468, height: 468),
                           cornerWidth: 106, cornerHeight: 106, transform: nil))
    context.fillPath()
    context.setFillColor(gray(0.48))
    context.addPath(CGPath(roundedRect: CGRect(x: 107, y: 91, width: 296, height: 334),
                           cornerWidth: 22, cornerHeight: 22, transform: nil))
    context.fillPath()
    context.setFillColor(gray(0.94))
    context.addPath(CGPath(roundedRect: CGRect(x: 92, y: 111, width: 296, height: 324),
                           cornerWidth: 22, cornerHeight: 22, transform: nil))
    context.fillPath()
    context.setFillColor(gray(0.18))
    for index in 0..<3 {
        context.fill(CGRect(x: 132, y: CGFloat(362 - index * 40), width: index == 2 ? 110 : 208, height: 14))
    }
    for y in 0..<8 {
        for x in 0..<12 {
            if (x + y) % 2 == 0 || x > 7 {
                context.setFillColor(gray(x > 7 ? 0.18 : 0.44))
                context.fill(CGRect(x: CGFloat(132 + x * 17), y: CGFloat(150 + y * 12), width: 10, height: 8))
            }
        }
    }
    guard let data = bitmap.representation(using: .png, properties: [:]) else {
        throw NSError(domain: "PaperShade.Icon", code: 2, userInfo: [NSLocalizedDescriptionKey: "Cannot encode icon PNG"])
    }
    return data
}

for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let suffix = scale == 2 ? "@2x" : ""
        let file = output.appendingPathComponent("icon_\(points)x\(points)\(suffix).png")
        try render(size: points * scale).write(to: file)
    }
}
