// SPDX-License-Identifier: GPL-3.0-or-later
// macOS launcher: pick resolution, anti-aliasing, volume and music options,
// remember them in private/launcher.json, then exit 0 so launcher.sh starts
// the game. Mirrors tools/launcher.ps1 without the Music folder button: custom
// soundtracks need Media Foundation. Exits 1 if the window is closed.
import AppKit

let root = URL(fileURLWithPath: CommandLine.arguments.count > 1
    ? CommandLine.arguments[1] : FileManager.default.currentDirectoryPath)
let settingsURL = root.appendingPathComponent("private/launcher.json")

let heights = [480, 720, 1080, 1440, 2160]
let samples = [1, 2, 4, 8]
let volumes = [100, 75, 50, 25, 0]

var settings: [String: Any] = [:]
if let data = try? Data(contentsOf: settingsURL),
   let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
    settings = json
}
func intSetting(_ key: String, _ fallback: Int) -> Int {
    (settings[key] as? NSNumber)?.intValue ?? fallback
}
func boolSetting(_ key: String) -> Bool {
    (settings[key] as? NSNumber)?.boolValue ?? false
}
// What the game does when its title screen is left idle; see docs/title-idle.md.
let idleValues = ["default", "no-cutscene", "cpu-exhibition"]
let idleNames = ["Default", "No cutscene", "CPU Exhibition"]
let screenHeight: Int = {
    guard let screen = NSScreen.screens.first ?? NSScreen.main else { return 1080 }
    return Int(screen.frame.height * screen.backingScaleFactor)
}()

final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 260, height: 324),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
    let resolution = NSPopUpButton()
    let msaa = NSPopUpButton()
    let volume = NSPopUpButton()
    let idle = NSPopUpButton()
    let smaa = NSButton(checkboxWithTitle: "SMAA", target: nil, action: nil)
    let shuffle = NSButton(checkboxWithTitle: "Shuffle music", target: nil, action: nil)
    let demoCut = NSButton(checkboxWithTitle: "Shorter demo cuts", target: nil, action: nil)
    let freeCamera = NSButton(checkboxWithTitle: "Free camera", target: nil, action: nil)
    var launched = false

    func addRow(_ title: String, _ popup: NSPopUpButton, _ items: [String], _ index: Int, _ y: CGFloat) {
        let label = NSTextField(labelWithString: title)
        label.frame = NSRect(x: 16, y: y + 3, width: 84, height: 20)
        popup.frame = NSRect(x: 104, y: y, width: 140, height: 26)
        popup.addItems(withTitles: items)
        popup.selectItem(at: max(0, index))
        window.contentView!.addSubview(label)
        window.contentView!.addSubview(popup)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        window.title = "DOAXBV"
        window.delegate = self
        let savedHeight = intSetting("height", screenHeight)
        let nearest = heights.lastIndex(where: { $0 <= savedHeight }) ?? 0
        addRow("Resolution", resolution,
               heights.map { $0 == 480 ? "480p (windowed)" : "\($0)p" }, nearest, 282)
        let savedMsaa = intSetting("msaa", 1)
        addRow("MSAA", msaa, samples.map { $0 == 1 ? "Off" : "\($0)x" },
               samples.firstIndex(of: savedMsaa) ?? 0, 250)
        smaa.frame = NSRect(x: 104, y: 224, width: 140, height: 20)
        smaa.title = "SMAA (unsupported)"
        smaa.state = .off
        smaa.isEnabled = false
        addRow("Volume", volume, volumes.map { $0 == 0 ? "Mute" : "\($0)%" },
               volumes.firstIndex(of: intSetting("volume", 100)) ?? 0, 190)
        shuffle.frame = NSRect(x: 104, y: 164, width: 140, height: 20)
        shuffle.state = boolSetting("shuffle") ? .on : .off
        demoCut.frame = NSRect(x: 104, y: 138, width: 140, height: 20)
        demoCut.state = boolSetting("demo_cut_hold") ? .on : .off
        freeCamera.frame = NSRect(x: 104, y: 112, width: 140, height: 20)
        freeCamera.state = boolSetting("free_camera") ? .on : .off
        let idleLabel = NSTextField(labelWithString: "Title screen idle behavior")
        idleLabel.frame = NSRect(x: 16, y: 82, width: 228, height: 20)
        idle.frame = NSRect(x: 16, y: 54, width: 228, height: 26)
        idle.addItems(withTitles: idleNames)
        idle.selectItem(at: idleValues.firstIndex(of: settings["title_idle"] as? String ?? "") ?? 0)

        let play = NSButton(title: "Play", target: self, action: #selector(play))
        play.frame = NSRect(x: 160, y: 12, width: 88, height: 30)
        play.keyEquivalent = "\r"
        for view in [smaa, shuffle, demoCut, freeCamera, idleLabel, idle, play] { window.contentView!.addSubview(view) }

        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc func play() {
        let chosen: [String: Any] = [
            "height": heights[resolution.indexOfSelectedItem],
            "msaa": samples[msaa.indexOfSelectedItem],
            "smaa": false,
            "volume": volumes[volume.indexOfSelectedItem],
            "shuffle": shuffle.state == .on,
            "demo_cut_hold": demoCut.state == .on,
            "free_camera": freeCamera.state == .on,
            "title_idle": idleValues[idle.indexOfSelectedItem],
        ]
        do {
            try FileManager.default.createDirectory(
                at: settingsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try JSONSerialization.data(
                withJSONObject: chosen, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: settingsURL)
        } catch {
            NSAlert(error: error).runModal()
            return
        }
        launched = true
        NSApp.terminate(nil)
    }

    func windowWillClose(_ notification: Notification) {
        if !launched { exit(1) }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let launcher = Launcher()
app.delegate = launcher
app.run()
exit(launcher.launched ? 0 : 1)
