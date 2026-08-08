// Plan 048 — HEIC decode benchmark: ImageIO half. See the .c file for the
// libheif half and the measured results.
//
//   swiftc -O 2026-08-08-048-heic-decode-bench.swift -o bench_imageio
//   ./bench_imageio <file.heic> 8
//
// CGImageSourceCreateImageAtIndex returns a lazy CGImage, so the timing loop
// draws into a bitmap context to force the decode. Timing the CGImage
// creation alone reports ~0 ms and is wrong.

import Foundation
import ImageIO
import CoreGraphics

let path = CommandLine.arguments[1]
let n = Int(CommandLine.arguments[2])!
var best = Double.infinity
for _ in 0..<n {
    let t0 = DispatchTime.now()
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0,
              [kCGImageSourceShouldCache: false] as CFDictionary) else {
        print("ERROR: decode failed"); exit(1)
    }
    // CGImage is lazy; drawing into a bitmap forces the actual decode.
    let w = img.width, h = img.height
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8,
                        bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
    let t1 = DispatchTime.now()
    let ms = Double(t1.uptimeNanoseconds - t0.uptimeNanoseconds) / 1e6
    best = min(best, ms)
    print(String(format: "%.0f ", ms), terminator: "")
}
print(String(format: "\nImageIO best: %.1f ms", best))
