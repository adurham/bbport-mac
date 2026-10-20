// bbport (macOS): the game's own launcher — the app binary that becomes the game.
//
// Double-click starts this process as "Bloodborne PC Port"; it checks the game folder and the
// checkout, sets the environment, and execs tools/macos/run.sh, which execs the game. execv
// replaces this process: a moment later there is no launcher left — the process the Finder
// started IS the game (P.T.'s shape; its app's binary is the game too). The happy path never
// starts Cocoa; only a failure does, to say why in a dialog.
import AppKit
import Darwin
import Foundation

// The app's own domain ("io.github.bbport.mac", where `defaults write …` and the retired
// settings app put things). NOT UserDefaults(suiteName:) — with an app's own bundle ID that
// initializer finds nothing, which is how the first build of this file failed.
let defaults = UserDefaults.standard

func present(_ message: String, repo: String) -> String? {
    // A chosen game folder (the picker), else "" (the log was shown), else the process exits.
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    app.activate(ignoringOtherApps: true)
    let alert = NSAlert()
    alert.messageText = "Bloodborne PC Port could not start the game"
    alert.informativeText = message
    alert.addButton(withTitle: "Choose Game Folder…")
    alert.addButton(withTitle: "Show the Log")
    alert.addButton(withTitle: "Quit")
    switch alert.runModal() {
    case .alertFirstButtonReturn:
        let panel = NSOpenPanel()
        panel.message = "Choose the game folder (with eboot.bin)"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url {
            return url.path
        }
        return ""
    case .alertSecondButtonReturn:
        let log = URL(fileURLWithPath: repo + "/out/last-run.log")
        if FileManager.default.fileExists(atPath: log.path) {
            NSWorkspace.shared.activateFileViewerSelecting([log])
        }
        return ""
    default:
        exit(1)
    }
}

func findGameFolder() -> String {
    // What is remembered, else the one under ~/Games/Bloodborne (the owner should not have to
    // point at a path the app can find itself).
    if let path = defaults.string(forKey: "gameFolder"),
       !path.isEmpty, FileManager.default.fileExists(atPath: path + "/eboot.bin") {
        return path
    }
    let games = NSHomeDirectory() + "/Games/Bloodborne"
    if let entries = (try? FileManager.default.contentsOfDirectory(atPath: games))?.sorted() {
        for entry in entries where FileManager.default.fileExists(atPath: games + "/" + entry + "/eboot.bin") {
            let found = games + "/" + entry
            defaults.set(found, forKey: "gameFolder")
            return found
        }
    }
    return defaults.string(forKey: "gameFolder") ?? ""
}

// The memory pre-flight. Bloodborne holds ~15.4 GiB that is structurally unfreeable (~14 GB of it
// IOAccelerator), so starting it into a machine already carrying kernel, WindowServer, browsers and
// VMs is how WindowServer gets jetsammed — twice this week. What a machine has to spare is RAM
// minus wired minus compressor; below the game plus a 4 GiB margin the app says so once. Warn-only:
// the launch always wins if the owner insists, and nothing is shown when memory is fine.
let gameMiB = 15400
let marginMiB = 4096
let defaultThresholdMiB = gameMiB + marginMiB

struct MemoryFacts {
    let totalMiB: Int
    let wiredMiB: Int
    let compressedMiB: Int
    var availableMiB: Int { max(0, totalMiB - wiredMiB - compressedMiB) }
}

// hw.memsize + host_statistics64's VM_STATISTICS counters: both readable without entitlements.
func memoryFacts() -> MemoryFacts? {
    var memsize: UInt64 = 0
    var len = MemoryLayout<UInt64>.size
    guard sysctlbyname("hw.memsize", &memsize, &len, nil, 0) == 0, memsize > 0 else { return nil }
    var stats = vm_statistics64_data_t()
    var count = mach_msg_type_number_t(MemoryLayout<vm_statistics64_data_t>.size
                                       / MemoryLayout<integer_t>.size)
    let kr = withUnsafeMutablePointer(to: &stats) {
        $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
            host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &count)
        }
    }
    guard kr == KERN_SUCCESS else { return nil }
    let page = UInt64(sysconf(_SC_PAGESIZE))
    let mib = 1 << 20
    return MemoryFacts(totalMiB: Int(memsize) / mib,
                       wiredMiB: Int(UInt64(stats.wire_count) * page) / mib,
                       compressedMiB: Int(UInt64(stats.compressor_page_count) * page) / mib)
}

// RSS, not footprint: cheap and entitlement-free. It under-reports Metal's encoding memory (that
// lives in IOAccelerator), so this names processes to close — it is not an accounting of the gap.
// No sort flag: macOS's -r sorts by CPU and -m is easy to misread, so sort here — by RSS — and fold
// one program's processes together (this machine runs several of the same binary).
func topRSSConsumers(_ n: Int) -> [String] {
    let ps = Process()
    ps.executableURL = URL(fileURLWithPath: "/bin/ps")
    ps.arguments = ["-Ao", "rss,comm"]
    let pipe = Pipe()
    ps.standardOutput = pipe
    guard (try? ps.run()) != nil else { return [] }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    ps.waitUntilExit()
    var folded: [String: (kib: Double, count: Int)] = [:]
    for row in String(decoding: data, as: UTF8.self).split(separator: "\n").dropFirst() {
        let parts = row.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: true)
        guard parts.count == 2, let kib = Double(parts[0]) else { continue }
        let name = (String(parts[1]) as NSString).lastPathComponent
        let seen = folded[name] ?? (0, 0)
        folded[name] = (seen.kib + kib, seen.count + 1)
    }
    return folded.sorted { $0.value.kib > $1.value.kib }.prefix(n).map { name, seen in
        seen.count > 1
            ? String(format: "%@ %.1f GiB (%d processes)", name, seen.kib / 1048576, seen.count)
            : String(format: "%@ %.1f GiB", name, seen.kib / 1048576)
    }
}

// The dialog's body, in one place: the pre-flight-only path prints this same string, so the failing
// path is inspectable (byte for byte what the alert shows) without a screenshot.
func lowMemoryBody(_ facts: MemoryFacts, needMiB: Int) -> String {
    var body = String(format: "This Mac has about %.1f GiB to spare (RAM minus wired and compressed "
                      + "memory); the game needs about %.1f GiB — %.1f GiB for itself plus a %.1f GiB "
                      + "safety margin.",
                      Double(facts.availableMiB) / 1024, Double(defaultThresholdMiB) / 1024,
                      Double(gameMiB) / 1024, Double(marginMiB) / 1024)
    if needMiB != defaultThresholdMiB {
        // Only a test override gets here; say so rather than pretend the game grew.
        body += String(format: " (The bar for this run is overridden to %.1f GiB.)", Double(needMiB) / 1024)
    }
    let top = topRSSConsumers(3)
    if !top.isEmpty {
        body += "\n\nLargest memory users right now (RSS — Metal memory is not counted here, so "
            + "this under-reports):\n" + top.map { "    • " + $0 }.joined(separator: "\n")
    }
    body += "\n\nStarting it anyway can get WindowServer killed."
    return body
}

// Returns true when the owner chose to launch anyway. Plain NSAlert, same shape as the failure
// path: this app has no window to put a banner in.
func warnLowMemory(_ facts: MemoryFacts, needMiB: Int) -> Bool {
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    app.activate(ignoringOtherApps: true)
    let alert = NSAlert()
    alert.messageText = "Not enough memory for Bloodborne"
    alert.informativeText = lowMemoryBody(facts, needMiB: needMiB)
    alert.addButton(withTitle: "Launch anyway") // default button: his launch wins
    alert.addButton(withTitle: "Cancel")
    return alert.runModal() == .alertFirstButtonReturn
}

let repo = (Bundle.main.object(forInfoDictionaryKey: "BBRepoPath") as? String) ?? ""
let runSh = repo + "/tools/macos/run.sh"
guard !repo.isEmpty, FileManager.default.isExecutableFile(atPath: runSh) else {
    _ = present("The checkout with tools/macos/run.sh was not found"
                + (repo.isEmpty ? " (no path is embedded)." : ": \(repo)"), repo: repo)
    exit(1)
}

var game = findGameFolder()
while game.isEmpty || !FileManager.default.fileExists(atPath: game + "/eboot.bin") {
    let message = "The game folder with eboot.bin was not found"
        + (game.isEmpty ? " (none is set)." : ": \(game)")
    let picked = present(message, repo: repo) ?? ""
    if picked.isEmpty {
        exit(1)
    }
    defaults.set(picked, forKey: "gameFolder")
    game = picked
}

// The same environment the settings app used to pass; everything else lives in bbport.ini.
setenv("BB_GAME_DIR", game, 1)
setenv("BB_CONFIG", NSHomeDirectory() + "/Library/Application Support/bbport/bbport.ini", 1)
if defaults.object(forKey: "nativeGpu") as? Bool ?? true {
    setenv("BB_NATIVE_GPU", "1", 1)
}
// Every run leaves per-frame data (the port's stats in the log + a CSV): owner-launched runs had
// none, which left the performance numbers to guesswork.
setenv("BB_ASYNC_STATIC_VI", "1", 1) // async compiles without dynamic vertex input (judged live)
setenv("BB_FRAME_STATS", "1", 1)
setenv("BB_FRAME_LOG", NSHomeDirectory() + "/Games/Bloodborne/frames-app.csv", 1)
let path = ProcessInfo.processInfo.environment["PATH"] ?? "/usr/bin:/bin"
setenv("PATH", "/opt/homebrew/bin:/usr/local/bin:" + path, 1)
guard chdir(repo) == 0 else {
    _ = present("Could not enter \(repo).", repo: repo)
    exit(1)
}

// Warn-only, and never a gate: this runs before the exec so a Cancel leaves nothing behind.
// BB_PREFLIGHT_ONLY=1 answers the same question on stdout and exits without launching — the only
// way to exercise this on a machine where the game must not start. BB_PREFLIGHT_THRESHOLD_MIB
// overrides the bar (test the failing path without actually starving the Mac).
let env = ProcessInfo.processInfo.environment
let thresholdMiB = Int(env["BB_PREFLIGHT_THRESHOLD_MIB"] ?? "") ?? defaultThresholdMiB
if let facts = memoryFacts() {
    let short = facts.availableMiB < thresholdMiB
    if env["BB_PREFLIGHT_ONLY"] == "1" {
        print("preflight: total \(facts.totalMiB) MiB, wired \(facts.wiredMiB) MiB, "
              + "compressor \(facts.compressedMiB) MiB, available \(facts.availableMiB) MiB")
        print("preflight: need \(thresholdMiB) MiB (game \(gameMiB) + margin \(marginMiB))")
        for line in topRSSConsumers(3) {
            print("preflight: top RSS: \(line)")
        }
        if short {
            print("preflight: FAIL — below the bar, so the app would show the dialog (nothing launched here)")
            print("preflight: dialog title: Not enough memory for Bloodborne")
            print("preflight: dialog buttons: Launch anyway (default), Cancel")
            for line in lowMemoryBody(facts, needMiB: thresholdMiB)
                .split(separator: "\n", omittingEmptySubsequences: false) {
                print("preflight: dialog | \(line)")
            }
        } else {
            print("preflight: PASS — enough memory, the game would start with no dialog")
        }
        exit(0) // the decision is on stdout; this mode never launches
    }
    if short, !warnLowMemory(facts, needMiB: thresholdMiB) {
        exit(1) // Cancel. "Launch anyway" falls through to the exec below, as it must.
    }
} else if env["BB_PREFLIGHT_ONLY"] == "1" {
    print("preflight: unavailable (hw.memsize or host_statistics64 failed) — launching as before")
    exit(0)
}

// exec: no launcher remains; run.sh execs the game into this same process.
let argv = ["bash", runSh].map { strdup($0) } + [nil]
execv("/bin/bash", argv)
_ = present("Could not start \(runSh): \(String(cString: strerror(errno)))", repo: repo)
exit(1)
