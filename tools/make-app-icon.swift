import AppKit

// Original vector artwork. Render every standard macOS icon size from the same
// geometry so the bundle icon stays reproducible without downloaded assets.
let destination = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)

func color(_ rgb: UInt32) -> NSColor {
    NSColor(srgbRed: CGFloat((rgb >> 16) & 255) / 255,
            green: CGFloat((rgb >> 8) & 255) / 255,
            blue: CGFloat(rgb & 255) / 255, alpha: 1)
}
func rounded(_ rect: NSRect, _ radius: CGFloat, _ fill: NSColor) {
    fill.setFill()
    NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius).fill()
}
func gradient(_ rect: NSRect, _ radius: CGFloat, _ bottom: UInt32, _ top: UInt32) {
    NSGradient(starting: color(bottom), ending: color(top))!
        .draw(in: NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius), angle: 90)
}
func drawPrinter() {
    NSGraphicsContext.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.22)
    shadow.shadowBlurRadius = 24
    shadow.shadowOffset = NSSize(width: 0, height: -10)
    shadow.set()
    gradient(NSRect(x: 64, y: 64, width: 896, height: 896), 196, 0x123653, 0x287D9D)
    NSGraphicsContext.restoreGraphicsState()

    // Rear sheet and its subtle printed lines.
    rounded(NSRect(x: 298, y: 530, width: 428, height: 324), 22, color(0xF7FAFC))
    for y: CGFloat in [790, 746, 702] {
        rounded(NSRect(x: 354, y: y, width: y == 702 ? 200 : 316, height: 13), 6, color(0xB2C5D0))
    }
    rounded(NSRect(x: 258, y: 590, width: 508, height: 40), 12, color(0x153046))

    NSGraphicsContext.saveGraphicsState()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.28)
    shadow.shadowBlurRadius = 22
    shadow.shadowOffset = NSSize(width: 0, height: -16)
    shadow.set()
    gradient(NSRect(x: 168, y: 278, width: 688, height: 348), 64, 0xC0D0DA, 0xF0F5F8)
    NSGraphicsContext.restoreGraphicsState()

    // Top housing, front panel, green status lamp and paper exit.
    gradient(NSRect(x: 168, y: 454, width: 688, height: 172), 58, 0xDEE8EE, 0xFFFFFF)
    rounded(NSRect(x: 168, y: 450, width: 688, height: 28), 0, color(0xDEE8EE))
    rounded(NSRect(x: 226, y: 533, width: 138, height: 15), 7, color(0x9DAFBA))
    rounded(NSRect(x: 746, y: 524, width: 34, height: 34), 17, color(0x258466))
    rounded(NSRect(x: 752, y: 533, width: 16, height: 16), 8, color(0x9FF1B4))
    rounded(NSRect(x: 246, y: 342, width: 532, height: 87), 24, color(0x243F50))
    rounded(NSRect(x: 270, y: 352, width: 484, height: 56), 16, color(0x132D3F))

    NSGraphicsContext.saveGraphicsState()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.18)
    shadow.shadowBlurRadius = 12
    shadow.shadowOffset = NSSize(width: 0, height: -7)
    shadow.set()
    gradient(NSRect(x: 314, y: 166, width: 396, height: 231), 12, 0xFFFFFF, 0xEAF0F4)
    NSGraphicsContext.restoreGraphicsState()
    for y: CGFloat in [330, 285, 240] {
        rounded(NSRect(x: 365, y: y, width: y == 240 ? 180 : 294, height: 13), 6, color(0x698493))
    }
}

let sizes = [(16, "icon_16x16"), (32, "icon_16x16@2x"),
             (32, "icon_32x32"), (64, "icon_32x32@2x"),
             (128, "icon_128x128"), (256, "icon_128x128@2x"),
             (256, "icon_256x256"), (512, "icon_256x256@2x"),
             (512, "icon_512x512"), (1024, "icon_512x512@2x")]
for (size, name) in sizes {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                                 bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                                 isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    let context = NSGraphicsContext(bitmapImageRep: bitmap)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    context.cgContext.clear(CGRect(x: 0, y: 0, width: size, height: size))
    context.cgContext.scaleBy(x: CGFloat(size) / 1024, y: CGFloat(size) / 1024)
    context.cgContext.setShouldAntialias(true)
    drawPrinter()
    context.flushGraphics()
    NSGraphicsContext.restoreGraphicsState()
    try bitmap.representation(using: .png, properties: [:])!
        .write(to: destination.appendingPathComponent(name + ".png"))
}
