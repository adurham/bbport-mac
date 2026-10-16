// SPDX-License-Identifier: GPL-2.0-or-later
// bbport launcher for macOS: picks the game folder, edits bbport.ini and the start-up
// environment, and runs tools/macos/run.sh (the Mac counterpart of launcher/bbport_launcher.py).
import AppKit
import SwiftUI

@main
struct BBLauncherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        // No main window: double-clicking the app IS the game (P.T.'s shape). The launcher runs
        // invisibly (an accessory app), starts the game, and leaves when the game does; problems
        // show as alerts. Settings comes first so no window is ever the "first" one macOS opens
        // at launch; the Log window appears only when an alert asks for it (see the AppDelegate,
        // which also closes anything a past session restored).
        Settings {
            LauncherSettingsView()
                .environmentObject(LauncherModel.shared)
        }

        Window("Game Log", id: "log") {
            LogView()
                .environmentObject(LauncherModel.shared)
        }
        .defaultSize(width: 900, height: 560)
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // The launcher is invisible: an accessory app with no window at all. Double-clicking the
        // app IS the game; it starts it and leaves when the game does. A problem shows an alert.
        NSApplication.shared.setActivationPolicy(.accessory)
        DispatchQueue.main.async {
            // Anything a past session restored (the log window) is not welcome: drop it here.
            for window in NSApplication.shared.windows {
                window.isRestorable = false
                window.close()
            }
            let model = LauncherModel.shared
            model.play()
            if !model.isRunning {
                let alert = NSAlert()
                alert.messageText = "Bloodborne PC Port could not start the game"
                alert.informativeText = model.lastProblem
                    ?? "Check the game folder (the log has the details)."
                alert.addButton(withTitle: "Choose Game Folder…")
                alert.addButton(withTitle: "Show the Log")
                alert.addButton(withTitle: "Quit")
                NSApplication.shared.activate(ignoringOtherApps: true)
                switch alert.runModal() {
                case .alertFirstButtonReturn:
                    model.fixGameFolder()
                    model.play()
                case .alertSecondButtonReturn:
                    model.revealLastLog()
                default:
                    NSApplication.shared.terminate(nil)
                }
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        LauncherModel.shared.prepareForQuit()
    }
}

/// Help menu: the user guide, downloads, problem reports and the last log.
struct HelpCommands: View {
    var body: some View {
        Button("bbport User Guide") { Links.open(Links.userGuide) }
        Button("Downloads and Release Notes") { Links.open(Links.releases) }
        Button("Report a Problem…") { Links.open(Links.issues) }
        Divider()
        Button("Show the Last Run's Log") { LauncherModel.shared.revealLastLog() }
        Button("Show the Data Folder") { LauncherModel.shared.reveal(LauncherModel.shared.dataURL) }
    }
}

/// Settings (⌘,): which game engine runs (the release app's own, or a checkout).
struct LauncherSettingsView: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        Form {
            Section {
                if model.engineURL != nil {
                    LabeledContent("Built into this app") {
                        Text(model.appVersion.isEmpty ? "Yes" : "Version \(model.appVersion)")
                    }
                } else {
                    LabeledContent("Embedded at build") {
                        Text(model.embeddedRepoPath ?? "None")
                            .textSelection(.enabled)
                            .truncationMode(.middle)
                    }
                }
                TextField(text: $model.prefs.repoOverride,
                          prompt: Text(model.engineURL != nil ? "Use the built-in engine" : "Use the embedded path")) {
                    Text("Checkout")
                }
                HStack {
                    Button("Choose…") { model.chooseRepository() }
                    Button(model.engineURL != nil ? "Use the Built-in Engine" : "Use the Embedded Path") {
                        model.prefs.repoOverride = ""
                    }
                    .disabled(model.prefs.repoOverride.isEmpty)
                }
                CheckLabel(check: model.repoCheck)
                LabeledContent("Data folder") {
                    Text(model.dataURL.path)
                        .textSelection(.enabled)
                        .truncationMode(.middle)
                }
            } header: {
                Text("Game engine")
            } footer: {
                Text(model.engineURL != nil
                     ? "This app carries its own engine; settings, saves and logs go to the data folder. To run a source checkout instead (tools/macos/build.sh), choose its folder."
                     : "The folder with tools/macos/run.sh. tools/macos/build_launcher.sh embeds the checkout it was run from.")
                    .foregroundStyle(.secondary)
            }
            Section {
                TextField(text: $model.prefs.branch, prompt: Text("Keep the current branch")) {
                    Text("Branch")
                }
            } header: {
                Text("Update (source checkouts)")
            } footer: {
                Text("Update fetches this branch from GitHub and rebuilds. macos-0.4 is the main branch; macos-native has the experimental native GPU process. Empty keeps the current one.")
                    .foregroundStyle(.secondary)
            }
            .disabled(model.usesBuiltInEngine)
        }
        .formStyle(.grouped)
        .frame(width: 580, height: 520)
    }
}
