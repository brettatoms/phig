import Photos
import SwiftUI

// Minimal shell: request Photos authorization, run the read-only survey, write
// the report where it can be read from a terminal, and show it.

let reportPath = NSString(string: "~/phig-photos-probe.txt").expandingTildeInPath

@main
struct PhigProbeApp: App {
    var body: some Scene {
        WindowGroup("Phig Probe") {
            ProbeView()
                .frame(minWidth: 720, minHeight: 480)
        }
        .windowResizability(.contentSize)
    }
}

struct ProbeView: View {
    @State private var output = "Requesting access to Photos…"
    @State private var running = true

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if running { ProgressView().controlSize(.small) }
            ScrollView {
                Text(output)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if !running {
                Text("Report written to \(reportPath)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding()
        .task { await start() }
    }

    private func start() async {
        // .readWrite rather than .readOnly: on macOS a read-only grant does not
        // expose PHAssetResource for every asset. Nothing here writes — the
        // usage string in Info.plist says so, and the code has no write call.
        let status = await PHPhotoLibrary.requestAuthorization(for: .readWrite)
        guard status == .authorized || status == .limited else {
            output = "Photos access \(status == .denied ? "denied" : "unavailable").\n\n"
                + "Grant it in System Settings > Privacy & Security > Photos,\n"
                + "then run the probe again."
            running = false
            return
        }

        let text = await Task.detached(priority: .userInitiated) {
            let survey = Probe.run { msg in Task { @MainActor in output = msg } }
            let samples = SampleExport.run(liveCount: 4, stillCount: 2) { msg in
                Task { @MainActor in output = msg }
            }
            return survey + "\n" + String(repeating: "=", count: 72) + "\n" + samples
        }.value

        try? text.write(toFile: reportPath, atomically: true, encoding: .utf8)
        output = text
        running = false
    }
}
