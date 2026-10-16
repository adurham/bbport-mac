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

let bundleID = "io.github.bbport.mac"

func fail(_ message: String, repo: String) -> Never {
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
            UserDefaults(suiteName: bundleID)?.set(url.path, forKey: "gameFolder")
        }
    case .alertSecondButtonReturn:
        let log = URL(fileURLWithPath: repo + "/out/last-run.log")
        if FileManager.default.fileExists(atPath: log.path) {
            NSWorkspace.shared.activateFileViewerSelecting([log])
        }
    default:
        break
    }
    exit(1)
}

let defaults = UserDefaults(suiteName: bundleID)
let repo = (Bundle.main.object(forInfoDictionaryKey: "BBRepoPath") as? String) ?? ""
let runSh = repo + "/tools/macos/run.sh"
guard !repo.isEmpty, FileManager.default.isExecutableFile(atPath: runSh) else {
    fail("The checkout with tools/macos/run.sh was not found"
         + (repo.isEmpty ? " (no path is embedded)." : ": \(repo)"),
         repo: repo)
}
let game = (defaults?.string(forKey: "gameFolder")) ?? ""
guard !game.isEmpty, FileManager.default.fileExists(atPath: game + "/eboot.bin") else {
    fail("The game folder with eboot.bin was not found"
         + (game.isEmpty ? " (none is set)." : ": \(game)"),
         repo: repo)
}

// The same environment the settings app used to pass; everything else lives in bbport.ini.
setenv("BB_GAME_DIR", game, 1)
setenv("BB_CONFIG", NSHomeDirectory() + "/Library/Application Support/bbport/bbport.ini", 1)
if defaults?.object(forKey: "nativeGpu") as? Bool ?? true {
    setenv("BB_NATIVE_GPU", "1", 1)
}
let path = ProcessInfo.processInfo.environment["PATH"] ?? "/usr/bin:/bin"
setenv("PATH", "/opt/homebrew/bin:/usr/local/bin:" + path, 1)
guard chdir(repo) == 0 else {
    fail("Could not enter \(repo).", repo: repo)
}

// exec: no launcher remains; run.sh execs the game into this same process.
let argv = ["bash", runSh].map { strdup($0) } + [nil]
execv("/bin/bash", argv)
fail("Could not start \(runSh): \(String(cString: strerror(errno)))", repo: repo)
