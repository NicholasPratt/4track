// Draws the 8track app icon: eight coloured clip lanes, a punch range and a playhead.
// Regenerate with:  swift Assets/make_icon.swift && iconutil -c icns Assets/8track.iconset -o Assets/8track.icns
import AppKit

func rgb(_ hex: UInt32) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 0xff) / 255, green: CGFloat((hex >> 8) & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: 1)
}

// Same palette as the tracks in the app (AudioEngine.cpp).
let palette: [UInt32] = [0x4f9dff, 0x34c79a, 0xf2b84b, 0xef6f6c, 0xb084f5, 0x4fd1e0, 0xf58fc7, 0xa3d65c]
// Clip start/end per lane, as fractions of the lane width.
let clips: [[(Double, Double)]] = [
    [(0.00, 0.55), (0.62, 1.00)], [(0.12, 0.80)], [(0.00, 0.38), (0.45, 0.92)], [(0.25, 1.00)],
    [(0.05, 0.50)], [(0.30, 0.72), (0.78, 1.00)], [(0.00, 0.66)], [(0.18, 0.58), (0.64, 0.95)],
]

func render(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px) / 1024   // design on a 1024 canvas (macOS icon grid: 824 body, 100 margin)

    let body = NSRect(x: 100 * s, y: 100 * s, width: 824 * s, height: 824 * s)
    let bodyPath = NSBezierPath(roundedRect: body, xRadius: 185 * s, yRadius: 185 * s)
    NSGradient(starting: rgb(0x2a2e36), ending: rgb(0x16181c))!.draw(in: bodyPath, angle: -90)
    bodyPath.addClip()

    let inner = body.insetBy(dx: 110 * s, dy: 150 * s)
    let laneH = inner.height / 8

    // Punch range behind the clips.
    let punch = NSRect(x: inner.minX + inner.width * 0.42, y: inner.minY - 30 * s,
                       width: inner.width * 0.30, height: inner.height + 60 * s)
    rgb(0xe5484d).withAlphaComponent(0.16).setFill()
    punch.fill()
    rgb(0xe5484d).setFill()
    NSBezierPath(roundedRect: NSRect(x: punch.minX, y: punch.maxY, width: punch.width, height: 18 * s),
                 xRadius: 6 * s, yRadius: 6 * s).fill()

    for lane in 0..<8 {
        let y = inner.maxY - CGFloat(lane + 1) * laneH   // lane 0 at the top
        for (a, b) in clips[lane] {
            let r = NSRect(x: inner.minX + inner.width * a, y: y + laneH * 0.16,
                           width: inner.width * (b - a) - 8 * s, height: laneH * 0.68)
            rgb(palette[lane]).setFill()
            NSBezierPath(roundedRect: r, xRadius: 10 * s, yRadius: 10 * s).fill()
        }
    }

    // Playhead.
    let x = inner.minX + inner.width * 0.57
    NSColor.white.setFill()
    NSRect(x: x - 5 * s, y: inner.minY - 30 * s, width: 10 * s, height: inner.height + 60 * s).fill()
    let head = NSBezierPath()
    head.move(to: NSPoint(x: x - 26 * s, y: inner.maxY + 62 * s))
    head.line(to: NSPoint(x: x + 26 * s, y: inner.maxY + 62 * s))
    head.line(to: NSPoint(x: x, y: inner.maxY + 22 * s))
    head.close()
    head.fill()

    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

let dir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "Assets/8track.iconset")
try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    try! render(size).write(to: dir.appendingPathComponent("icon_\(size)x\(size).png"))
    try! render(size * 2).write(to: dir.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
print("wrote \(dir.path)")
