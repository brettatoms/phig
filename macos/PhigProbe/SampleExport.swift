import Foundation
import Photos

// Exports a handful of assets so plan 038 has real Live Photos to develop
// against. This is deliberately NOT the importer — no dated layout, no state
// file, no indexing. It writes original resource bytes and stops, because 038
// needs the identifier metadata intact and nothing else.
//
// Output goes outside the repository: these are personal photos and must not
// become committed test fixtures.

enum SampleExport {
    static let outputDir = NSString(string: "~/phig-live-samples").expandingTildeInPath

    static func run(liveCount: Int, stillCount: Int, progress: @escaping (String) -> Void) -> String {
        var out: [String] = []
        out.append("sample export → \(outputDir)")

        // Idempotent: every original here lives in iCloud, so re-running the
        // survey must not re-download fixtures that already exist.
        let existing = (try? FileManager.default.contentsOfDirectory(atPath: outputDir)) ?? []
        if !existing.isEmpty {
            out.append("  \(existing.count) files already present — skipping.")
            out.append("  Delete the directory to re-export.")
            return out.joined(separator: "\n") + "\n"
        }

        try? FileManager.default.createDirectory(atPath: outputDir,
                                                 withIntermediateDirectories: true)

        let opts = PHFetchOptions()
        opts.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
        let all = PHAsset.fetchAssets(with: .image, options: opts)

        var lives: [PHAsset] = []
        var stills: [PHAsset] = []
        all.enumerateObjects { asset, _, stop in
            if asset.mediaSubtypes.contains(.photoLive) {
                if lives.count < liveCount { lives.append(asset) }
            } else if stills.count < stillCount {
                stills.append(asset)
            }
            if lives.count >= liveCount && stills.count >= stillCount { stop.pointee = true }
        }

        out.append("selected \(lives.count) Live Photos and \(stills.count) ordinary stills")
        out.append("")

        for (i, asset) in (lives + stills).enumerated() {
            let isLive = asset.mediaSubtypes.contains(.photoLive)
            progress("exporting \(i + 1) of \(lives.count + stills.count)…")
            out.append("[\(isLive ? "live" : "still")] \(asset.localIdentifier)")

            for res in PHAssetResource.assetResources(for: asset) {
                // .photo is the original still; .pairedVideo is the motion.
                // .fullSizePhoto appears only when the asset was edited.
                guard res.type == .photo || res.type == .pairedVideo
                        || res.type == .fullSizePhoto else {
                    out.append("      skipped resource type \(res.type.rawValue) (\(res.uniformTypeIdentifier))")
                    continue
                }
                let dest = URL(fileURLWithPath: outputDir)
                    .appendingPathComponent(res.originalFilename)
                try? FileManager.default.removeItem(at: dest)

                let o = PHAssetResourceRequestOptions()
                o.isNetworkAccessAllowed = true   // every original here is in iCloud
                let sem = DispatchSemaphore(value: 0)
                var failure: Error?
                PHAssetResourceManager.default().writeData(for: res, toFile: dest, options: o) { err in
                    failure = err
                    sem.signal()
                }
                _ = sem.wait(timeout: .now() + 180)

                if let failure {
                    out.append("      FAILED \(res.originalFilename): \(failure.localizedDescription)")
                } else {
                    let size = (try? FileManager.default.attributesOfItem(atPath: dest.path)[.size] as? Int64) ?? 0
                    out.append(String(format: "      %-34s type=%d  %8lld bytes  %@",
                                      (res.originalFilename as NSString).utf8String!,
                                      res.type.rawValue, size ?? 0, res.uniformTypeIdentifier))
                }
            }
        }

        out.append("")
        out.append("These are personal photos. They are for local development of")
        out.append("plan 038's pairing only — do not commit them as test fixtures.")
        return out.joined(separator: "\n") + "\n"
    }
}
