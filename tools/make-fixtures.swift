import Foundation
import CoreGraphics
import CoreText

let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
func text(_ context: CGContext, _ value: String, _ x: CGFloat, _ y: CGFloat, _ size: CGFloat) {
    let font = CTFontCreateWithName("Helvetica" as CFString, size, nil)
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: value, attributes: [NSAttributedString.Key(kCTFontAttributeName as String): font]))
    context.textPosition = CGPoint(x: x, y: y)
    CTLineDraw(line, context)
}
var box = CGRect(x: 0, y: 0, width: 595.276, height: 841.89)
let consumer = CGDataConsumer(url: output.appendingPathComponent("preview-fixture.pdf") as CFURL)!
let context = CGContext(consumer: consumer, mediaBox: &box, nil)!
for page in 1...2 {
    context.beginPDFPage(nil)
    context.setFillColor(gray: 1, alpha: 1)
    context.fill(box)
    context.setFillColor(gray: 0, alpha: 1)
    text(context, "LASERJET 1010 SIMULATION", 40, 780, 22)
    text(context, "Preview fixture — page \(page) of 2", 40, 748, 16)
    text(context, "A4 • portrait • 600 dpi • local only", 40, 722, 12)
    context.setStrokeColor(gray: 0, alpha: 1)
    context.setLineWidth(1)
    context.stroke(CGRect(x: 36, y: 36, width: box.width - 72, height: box.height - 72))
    for index in 0..<5 {
        context.setFillColor(gray: CGFloat(index) / 4, alpha: 1)
        context.fill(CGRect(x: 50 + CGFloat(index) * 70, y: 610, width: 60, height: 50))
    }
    context.setFillColor(gray: 0, alpha: 1)
    text(context, "TOP LEFT", 42, 690, 12)
    text(context, "BOTTOM RIGHT", 425, 48, 12)
    context.fill(CGRect(x: 50, y: 500 - CGFloat(page) * 60, width: CGFloat(page) * 80, height: 20))
    text(context, "Page marker: \(page)", 50, 460, 14)
    context.endPDFPage()
}
context.closePDF()

let diagnosticConsumer = CGDataConsumer(url: output.appendingPathComponent("hardware-diagnostic.pdf") as CFURL)!
let diagnostic = CGContext(consumer: diagnosticConsumer, mediaBox: &box, nil)!
diagnostic.beginPDFPage(nil)
diagnostic.setFillColor(gray: 0, alpha: 1)
text(diagnostic, "HP LASERJET 1010 — FIRST HARDWARE TEST", 45, 780, 15)
text(diagnostic, "A4 / 600 dpi / one page / sparse toner coverage", 45, 755, 11)
text(diagnostic, "Text: ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789", 45, 725, 12)
text(diagnostic, "TOP LEFT", 45, 690, 10)
text(diagnostic, "BOTTOM RIGHT", 450, 47, 10)
diagnostic.setStrokeColor(gray: 0, alpha: 1)
diagnostic.setLineWidth(0.5)
diagnostic.stroke(CGRect(x: 36, y: 36, width: box.width - 72, height: box.height - 72))
diagnostic.move(to: CGPoint(x: 45, y: 650))
diagnostic.addLine(to: CGPoint(x: 45 + 100 * 72 / 25.4, y: 650))
diagnostic.strokePath()
text(diagnostic, "100 mm reference line", 45, 630, 10)
for index in 0..<5 {
    diagnostic.setFillColor(gray: CGFloat(index) / 4, alpha: 1)
    diagnostic.fill(CGRect(x: 45 + CGFloat(index) * 42, y: 580, width: 24, height: 18))
}
diagnostic.setFillColor(gray: 0, alpha: 1)
text(diagnostic, "Five patches: black, dark gray, mid gray, light gray, white", 45, 560, 10)
diagnostic.endPDFPage()
diagnostic.closePDF()
