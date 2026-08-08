// Plan 048 — generates the checked-in HEIC/AVIF test fixtures.
//
// Run on macOS only; the output is committed so CI's Linux runner never needs
// to generate anything. Regenerate only if a fixture's properties must change,
// and re-run the full suite afterwards: several tests assert exact dimensions
// and pixel positions from these files.
//
//   swiftc -O docs/plans/2026-08-08-048-make-fixtures.swift -o /tmp/mkfix
//   /tmp/mkfix tests/data

import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

// 200x100 so a 90-degree rotation is visible in the dimensions alone, with a
// marker block in one corner so a rotation can be verified by pixel position
// and not just by shape — a square fixture would hide both.
func makeImage() -> CGImage {
    let w = 200, h = 100
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8,
                        bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 1))
    ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
    // CoreGraphics' origin is bottom-left, so this lands top-left in the image.
    ctx.setFillColor(CGColor(red: 0, green: 0, blue: 1, alpha: 1))
    ctx.fill(CGRect(x: 0, y: h - 20, width: 20, height: 20))
    return ctx.makeImage()!
}

func write(_ img: CGImage, _ path: String, _ type: UTType, _ props: [CFString: Any]) {
    let url = URL(fileURLWithPath: path) as CFURL
    guard let dst = CGImageDestinationCreateWithURL(url, type.identifier as CFString, 1, nil) else {
        FileHandle.standardError.write("cannot create destination for \(path)\n".data(using: .utf8)!)
        exit(1)
    }
    CGImageDestinationAddImage(dst, img, props as CFDictionary)
    guard CGImageDestinationFinalize(dst) else {
        FileHandle.standardError.write("failed to write \(path)\n".data(using: .utf8)!)
        exit(1)
    }
}

let out = CommandLine.arguments[1]
let img = makeImage()

for o in [1, 6, 8] {
    write(img, "\(out)/orient_\(o).heic", .heic, [kCGImagePropertyOrientation: o])
    write(img, "\(out)/orient_\(o).jpg", .jpeg, [kCGImagePropertyOrientation: o])
}

// A fixture carrying real EXIF, plus a JPEG control written from the same
// source with the same tag values, so the metadata test can compare the two
// extractions key for key and attribute any difference to extraction.
let exifProps: [CFString: Any] = [
    kCGImagePropertyOrientation: 1,
    kCGImagePropertyExifDictionary: [
        kCGImagePropertyExifDateTimeOriginal: "2026:08:08 14:32:11",
        kCGImagePropertyExifFNumber: 4.0,
        kCGImagePropertyExifISOSpeedRatings: [400],
        kCGImagePropertyExifExposureTime: 0.002,
    ],
    kCGImagePropertyTIFFDictionary: [
        kCGImagePropertyTIFFMake: "Canon",
        kCGImagePropertyTIFFModel: "Canon EOS R5",
    ],
]
write(img, "\(out)/exif_sample.heic", .heic, exifProps)
write(img, "\(out)/exif_sample.jpg", .jpeg, exifProps)

if let avif = UTType("public.avif") {
    write(img, "\(out)/sample.avif", avif, [kCGImagePropertyOrientation: 1])
} else {
    FileHandle.standardError.write("public.avif unavailable; no AVIF fixture\n".data(using: .utf8)!)
}

print("fixtures written to \(out)")
