import Foundation
import Photos

// Read-only survey of the Photos library. Writes nothing to Photos and copies
// no pixels; every call here is a metadata read.
//
// It exists because plan 049's iCloud handling was, in its own words, "designed
// defensively rather than from measurement" — TCC blocks a shell from seeing
// the library at all. Four of that plan's six open questions are empirical, and
// these are the numbers that answer them.

struct ProbeReport {
    var lines: [String] = []
    mutating func add(_ s: String) { lines.append(s) }
    var text: String { lines.joined(separator: "\n") + "\n" }
}

enum Probe {
    static func run(progress: @escaping (String) -> Void) -> String {
        var r = ProbeReport()
        r.add("phig Photos library probe — \(ISO8601DateFormatter().string(from: Date()))")
        r.add(String(repeating: "=", count: 72))

        // Proves the Swift-to-libphig link, the bridging header and the rpath
        // all work. This is 050's riskiest unknown, so it is checked here where
        // failure costs nothing.
        r.add("")
        r.add("libphig interop")
        let abi = phig_abi_version()
        r.add("  phig_abi_version() = \(abi >> 16).\(abi & 0xFFFF)  (link + bridging header OK)")

        let status = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        r.add("")
        r.add("authorization: \(describe(status))")
        guard status == .authorized || status == .limited else {
            r.add("  Not authorized — nothing further can be read.")
            return r.text
        }
        if status == .limited {
            r.add("  LIMITED access: the counts below cover only the assets you")
            r.add("  granted, not the whole library. Treat them as a floor.")
        }

        // ---- assets ----
        let all = PHAsset.fetchAssets(with: nil)
        r.add("")
        r.add("assets: \(all.count)")

        var images = 0, videos = 0, audio = 0, other = 0
        var live = 0, favorites = 0, hdr = 0, panorama = 0, screenshot = 0
        var edited = 0
        var sourceUser = 0, sourceCloudShared = 0, sourceItunes = 0
        var utiCounts: [String: Int] = [:]
        // Split by resource role rather than one total. An import that pairs
        // Live Photos writes the still AND its video, so a figure counting only
        // originals understates the disk cost — measured at roughly 1.5x the
        // still per Live Photo.
        var stillBytes: Int64 = 0, stillKnown = 0
        var pairedBytes: Int64 = 0, pairedKnown = 0
        var adjustedBytes: Int64 = 0, adjustedKnown = 0
        var oldest: Date?, newest: Date?
        var noCreationDate = 0

        all.enumerateObjects { asset, idx, _ in
            if idx % 2000 == 0 { progress("scanning asset \(idx) of \(all.count)…") }

            switch asset.mediaType {
            case .image: images += 1
            case .video: videos += 1
            case .audio: audio += 1
            default: other += 1
            }

            if asset.mediaSubtypes.contains(.photoLive) { live += 1 }
            if asset.mediaSubtypes.contains(.photoHDR) { hdr += 1 }
            if asset.mediaSubtypes.contains(.photoPanorama) { panorama += 1 }
            if asset.mediaSubtypes.contains(.photoScreenshot) { screenshot += 1 }
            if asset.isFavorite { favorites += 1 }

            switch asset.sourceType {
            case .typeUserLibrary: sourceUser += 1
            case .typeCloudShared: sourceCloudShared += 1
            case .typeiTunesSynced: sourceItunes += 1
            default: break
            }

            if let d = asset.creationDate {
                if oldest == nil || d < oldest! { oldest = d }
                if newest == nil || d > newest! { newest = d }
            } else {
                noCreationDate += 1
            }

            let resources = PHAssetResource.assetResources(for: asset)

            // An adjusted asset carries a .fullSizePhoto/.fullSizeVideo
            // alongside its original — this is the public signal for "edited in
            // Photos", and it decides open question 1 (originals vs edits).
            if resources.contains(where: { $0.type == .fullSizePhoto || $0.type == .fullSizeVideo }) {
                edited += 1
            }

            for res in resources {
                // fileSize is not public API. Read defensively: if Apple ever
                // drops the key this degrades to "unknown", never crashes.
                let size = res.value(forKey: "fileSize") as? Int64

                switch res.type {
                case .photo, .video:
                    utiCounts[res.uniformTypeIdentifier, default: 0] += 1
                    if let size { stillBytes += size; stillKnown += 1 }
                case .pairedVideo:
                    if let size { pairedBytes += size; pairedKnown += 1 }
                case .fullSizePhoto, .fullSizeVideo:
                    if let size { adjustedBytes += size; adjustedKnown += 1 }
                default:
                    break
                }
            }
        }

        r.add("  images \(images)   videos \(videos)   audio \(audio)   other \(other)")
        r.add("  favorites \(favorites)   live photos \(live)   HDR \(hdr)   panorama \(panorama)   screenshots \(screenshot)")
        r.add("  edited in Photos (has adjusted resource): \(edited)")
        r.add("  source: user library \(sourceUser)   shared \(sourceCloudShared)   iTunes-synced \(sourceItunes)")
        if let o = oldest, let n = newest {
            let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd"
            r.add("  date range: \(f.string(from: o)) … \(f.string(from: n))")
        }
        if noCreationDate > 0 { r.add("  WITHOUT a creation date: \(noCreationDate)  (these cannot be filed by date)") }

        r.add("")
        r.add("formats (original resource UTI)")
        for (uti, n) in utiCounts.sorted(by: { $0.value > $1.value }) {
            r.add(String(format: "  %-28s %6d", (uti as NSString).utf8String!, n))
        }

        r.add("")
        if stillKnown > 0 {
            func gb(_ b: Int64) -> String { String(format: "%.2f GB", Double(b) / 1_073_741_824) }
            r.add("size (an export duplicates this on disk)")
            r.add("  originals (stills + videos):  \(gb(stillBytes))   across \(stillKnown) resources")
            r.add("  paired Live Photo videos:     \(gb(pairedBytes))   across \(pairedKnown) resources")
            r.add("  ------------------------------------------------")
            r.add("  IMPORT PAIRED (still + motion): \(gb(stillBytes + pairedBytes))")
            r.add("  import stills only:             \(gb(stillBytes))")
            if adjustedKnown > 0 {
                r.add("")
                r.add("  adjusted versions, if exporting edits instead of originals:")
                r.add("    \(gb(adjustedBytes)) across \(adjustedKnown) resources")
            }
        } else {
            r.add("size: could not read fileSize from any resource — the estimate")
            r.add("  must be summed during export instead of shown up front.")
        }

        // ---- albums, and how often they overlap ----
        var albumCount = 0
        var assetAlbumCount: [String: Int] = [:]
        for type in [PHAssetCollectionType.album, .smartAlbum] {
            let collections = PHAssetCollection.fetchAssetCollections(with: type, subtype: .any, options: nil)
            collections.enumerateObjects { collection, _, _ in
                if type == .album { albumCount += 1 }
                guard type == .album else { return }
                let members = PHAsset.fetchAssets(in: collection, options: nil)
                members.enumerateObjects { asset, _, _ in
                    assetAlbumCount[asset.localIdentifier, default: 0] += 1
                }
            }
        }
        let inAnyAlbum = assetAlbumCount.count
        let inMultiple = assetAlbumCount.values.filter { $0 > 1 }.count
        r.add("")
        r.add("albums: \(albumCount) user albums")
        r.add("  photos in at least one album: \(inAnyAlbum)")
        r.add("  photos in MORE THAN ONE album: \(inMultiple)   <- decides the gallery-collision rule")

        // ---- iCloud availability, by sample ----
        // No public API reports "is this original on disk". Requesting data with
        // network access disallowed does, but it costs a real request per asset,
        // so this samples rather than sweeping the whole library.
        r.add("")
        r.add("iCloud availability (sampled)")
        let sampleSize = min(40, all.count)
        if sampleSize == 0 {
            r.add("  no assets to sample")
        } else {
            var localCount = 0, cloudCount = 0, failed = 0
            let stride = max(1, all.count / sampleSize)
            let group = DispatchGroup()
            let lock = NSLock()
            for i in Swift.stride(from: 0, to: all.count, by: stride).prefix(sampleSize) {
                let asset = all.object(at: i)
                guard let res = PHAssetResource.assetResources(for: asset)
                    .first(where: { $0.type == .photo || $0.type == .video }) else { continue }
                let opts = PHAssetResourceRequestOptions()
                opts.isNetworkAccessAllowed = false   // the whole point of the probe
                group.enter()
                var got = false
                PHAssetResourceManager.default().requestData(for: res, options: opts) { _ in
                    got = true
                } completionHandler: { error in
                    lock.lock()
                    if error != nil { cloudCount += 1 } else if got { localCount += 1 } else { failed += 1 }
                    lock.unlock()
                    group.leave()
                }
            }
            _ = group.wait(timeout: .now() + 120)
            let n = localCount + cloudCount + failed
            r.add("  sampled \(n): local \(localCount), needs download \(cloudCount), indeterminate \(failed)")
            if n > 0 && cloudCount > 0 {
                let pct = Double(cloudCount) / Double(n) * 100
                r.add(String(format: "  => roughly %.0f%% of the library would need downloading (~%d assets)",
                             pct, Int(pct / 100 * Double(all.count))))
            } else if cloudCount == 0 {
                r.add("  => originals appear to be on disk; import is a local copy, not a download")
            }
        }

        r.add("")
        r.add(String(repeating: "=", count: 72))
        r.add("Nothing was written to the Photos library.")
        return r.text
    }

    private static func describe(_ s: PHAuthorizationStatus) -> String {
        switch s {
        case .authorized: return "authorized (full)"
        case .limited: return "limited"
        case .denied: return "denied"
        case .restricted: return "restricted"
        case .notDetermined: return "not determined"
        @unknown default: return "unknown"
        }
    }
}
