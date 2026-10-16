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
setenv("BB_FRAME_STATS", "1", 1)
setenv("BB_FRAME_LOG", NSHomeDirectory() + "/Games/Bloodborne/frames-app.csv", 1)
let path = ProcessInfo.processInfo.environment["PATH"] ?? "/usr/bin:/bin"
setenv("PATH", "/opt/homebrew/bin:/usr/local/bin:" + path, 1)
guard chdir(repo) == 0 else {
    _ = present("Could not enter \(repo).", repo: repo)
    exit(1)
}

// exec: no launcher remains; run.sh execs the game into this same process.
let argv = ["bash", runSh].map { strdup($0) } + [nil]
execv("/bin/bash", argv)
_ = present("Could not start \(runSh): \(String(cString: strerror(errno)))", repo: repo)
exit(1)
