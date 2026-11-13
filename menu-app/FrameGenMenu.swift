import AppKit

private func makeFrameGenIcon(active: Bool) -> NSImage {
    let icon = NSImage(size: NSSize(width: 18, height: 18), flipped: false) { _ in
        NSColor.black.set()

        let streaks = NSBezierPath()
        for (y, end) in [(6.5, 6.0), (9.0, 4.5), (11.5, 5.5)] {
            streaks.move(to: NSPoint(x: 1.5, y: y))
            streaks.line(to: NSPoint(x: end, y: y))
        }
        streaks.lineWidth = 1.25
        streaks.lineCapStyle = .round
        streaks.stroke()

        let bolt = NSBezierPath()
        bolt.move(to: NSPoint(x: 12, y: 16))
        bolt.line(to: NSPoint(x: 6.5, y: 8.25))
        bolt.line(to: NSPoint(x: 10.25, y: 8.25))
        bolt.line(to: NSPoint(x: 9.25, y: 2))
        bolt.line(to: NSPoint(x: 16.5, y: 10))
        bolt.line(to: NSPoint(x: 11.75, y: 10))
        bolt.close()
        bolt.lineWidth = 1.25
        bolt.lineJoinStyle = .round
        if active { bolt.fill() }
        bolt.stroke()
        return true
    }
    icon.isTemplate = true
    icon.accessibilityDescription = "FrameGen"
    return icon
}

private struct Activity: Equatable {
    enum State: String {
        case idle, waiting, active, unsupported, off
        case unsupportedD3D12 = "unsupported-d3d12"
    }

    let enabled: Bool
    let state: State
    let game: String?
    let issue: String?

    init?(_ fields: ArraySlice<Substring>, issue: Substring? = nil) {
        guard fields.count == 3, let state = State(rawValue: String(fields[fields.startIndex + 1])) else {
            return nil
        }
        switch fields[fields.startIndex] {
        case "on": enabled = true
        case "off": enabled = false
        default: return nil
        }
        self.state = state
        self.issue = issue.flatMap { $0 == "-" ? nil : String($0) }
        switch fields[fields.startIndex + 2] {
        case "", "-": game = nil
        case let name where name.lowercased() == "dirtrally2.exe": game = "DiRT Rally 2.0"
        case let name where name.lowercased() == "eurotrucks2.exe": game = "Euro Truck Simulator 2"
        case let name where name.lowercased() == "subnautica.exe": game = "Subnautica"
        case let name where name.lowercased() == "escape simulator 2.exe": game = "Escape Simulator 2"
        case let name where name.lowercased() == "madison vr.exe": game = "MADiSON VR"
        case let name where name.lowercased() == "redmatter2.exe": game = "Red Matter 2"
        case let name where name.lowercased() == "skyrimvr.exe": game = "Skyrim VR"
        case let name: game = String(name)
        }
    }

    var description: String {
        switch state {
        case .active:
            return game.map { "Generating frames in \($0)" } ?? "Generating frames"
        case .unsupported:
            return game.map { "Frame generation unavailable in \($0)" } ?? "Frame generation unavailable in this session"
        case .unsupportedD3D12:
            return game.map { "DirectX 12 frame generation unavailable in \($0)" } ?? "DirectX 12 frame generation unavailable"
        case .waiting:
            return game.map { "Waiting for generated frames in \($0)" } ?? "Waiting for generated frames"
        case .off:
            if enabled { return game.map { "Enabling in \($0)…" } ?? "Enabling frame generation…" }
            return game.map { "Off in \($0)" } ?? "Frame generation off"
        case .idle:
            return enabled ? "No compatible VR session detected" : "No active VR session"
        }
    }
}

private struct Installation: Equatable {
    enum State: String {
        case current, outdated, partial, missing
    }

    let state: State

    init?(_ fields: [Substring]) {
        guard (fields.count == 6 || fields.count == 7),
              let state = State(rawValue: String(fields[0])), !fields[5].isEmpty else {
            return nil
        }
        self.state = state
    }

    var description: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
        switch state {
        case .current:
            return "Installed · Up to date (\(version))"
        case .outdated:
            return "Installed · Update to \(version) available"
        case .partial:
            return "Installation incomplete · Repair needed"
        case .missing:
            return "Not installed"
        }
    }

    var actionTitle: String {
        switch state {
        case .current: return "Install"
        case .outdated: return "Update"
        case .partial: return "Repair"
        case .missing: return "Install"
        }
    }
}

private struct Bottle: Equatable, Sendable {
    enum Provider: String, Sendable {
        case crossover, moltenvr

        var title: String {
            switch self {
            case .crossover: return "CrossOver"
            case .moltenvr: return "MoltenVR"
            }
        }
    }

    let provider: Provider
    let name: String
    let path: String?

    var title: String { "\(name) · \(provider.title)" }
    var arguments: [String] {
        ["--provider", provider.rawValue] + (path.map { ["--bottle-path", $0] } ?? ["--bottle", name])
    }

    init(provider: Provider, name: String, path: String? = nil) {
        self.provider = provider
        self.name = name
        self.path = path
    }

    init?(record: Substring) {
        let fields = record.split(separator: "\t", omittingEmptySubsequences: false)
        guard (2...3).contains(fields.count), let provider = Provider(rawValue: String(fields[0])), !fields[1].isEmpty else {
            return nil
        }
        let path = fields.count == 3 ? String(fields[2]) : nil
        guard path == nil || (provider == .moltenvr && path?.hasPrefix("/") == true) else { return nil }
        self.init(provider: provider, name: String(fields[1]), path: path)
    }
}

private struct CommandResult: Sendable {
    let status: Int32
    let output: String
    let diagnostics: String

    var succeeded: Bool { status == 0 }

    var fields: [Substring] {
        output.trimmingCharacters(in: .newlines).split(separator: "\t", omittingEmptySubsequences: false)
    }
}

@MainActor
private final class MenuController: NSObject, NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate {
    private struct Controls {
        let window: NSWindow
        let crossover: NSPopUpButton
        let bottle: NSPopUpButton
        let install: NSButton
        let generation: NSButton
        let installation: NSTextField
        let activity: NSTextField
    }

    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private lazy var inactiveIcon = makeFrameGenIcon(active: false)
    private lazy var activeIcon = makeFrameGenIcon(active: true)
    private let queue = DispatchQueue(label: "FrameGenMenu.commands", qos: .utility)
    private let logURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/FrameGen.log")
    private let defaults = UserDefaults.standard
    private var crossoverPaths: [String] = []
    private var bottles: [Bottle] = []
    private var crossover: String?
    private var bottle: Bottle?
    private var installation: Installation?
    private var activity: Activity?
    private var lastLoggedIssue: String?
    private var activityUnavailable = false
    private var message: String?
    private var busy = false
    private var polling = false
    private var timer: Timer?
    private var menuGeneration: NSMenuItem?
    private var menuInstallation: NSMenuItem?
    private var menuActivity: NSMenuItem?
    private lazy var controls = makeControls()

    func applicationDidFinishLaunching(_ notification: Notification) {
        crossover = defaults.string(forKey: "SelectedCrossOver")
        if let name = defaults.string(forKey: "SelectedBottle") {
            let provider = defaults.string(forKey: "SelectedBottleProvider").flatMap(Bottle.Provider.init(rawValue:))
            bottle = Bottle(provider: provider ?? .crossover, name: name,
                            path: defaults.string(forKey: "SelectedBottlePath"))
        }
        showControls()
        let timer = Timer(timeInterval: 1.5, target: self, selector: #selector(pollActivity),
                          userInfo: nil, repeats: true)
        timer.tolerance = 0.25
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    func applicationWillTerminate(_ notification: Notification) {
        timer?.invalidate()
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showControls()
        return false
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func windowWillClose(_ notification: Notification) {
        if let window = notification.object as? NSWindow, window === controls.window {
            NSApp.setActivationPolicy(.accessory)
        }
    }

    func menuWillOpen(_ menu: NSMenu) {
        poll(refreshInstallation: true)
    }

    @objc private func showControls() {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        if controls.window.isMiniaturized { controls.window.deminiaturize(nil) }
        controls.window.makeKeyAndOrderFront(nil)
        renderMenu()
        refresh()
    }

    private func makeControls() -> Controls {
        func popup(_ action: Selector) -> NSPopUpButton {
            let control = NSPopUpButton(frame: .zero, pullsDown: false)
            control.target = self
            control.action = action
            control.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            return control
        }
        func statusLabel() -> NSTextField {
            let label = NSTextField(wrappingLabelWithString: "")
            label.maximumNumberOfLines = 2
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            label.textColor = .secondaryLabelColor
            return label
        }

        let crossover = popup(#selector(changeCrossOver(_:)))
        let bottle = popup(#selector(changeBottle(_:)))
        let install = NSButton(title: "Install", target: self, action: #selector(install))
        let generation = NSButton(checkboxWithTitle: "Frame Generation",
                                  target: self, action: #selector(toggle))
        generation.font = .systemFont(ofSize: NSFont.systemFontSize, weight: .semibold)
        let installation = statusLabel()
        let activity = statusLabel()
        installation.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        generation.toolTip = "Applies to compatible VR games in the selected bottle."
        let installRow = NSStackView(views: [install, installation])
        installRow.alignment = .centerY
        installRow.spacing = 9
        installRow.detachesHiddenViews = true

        let settings = NSGridView(views: [
            [NSTextField(labelWithString: "Bottle"), bottle],
            [NSTextField(labelWithString: "CrossOver"), crossover],
        ])
        settings.column(at: 0).width = 74
        settings.column(at: 0).xPlacement = .trailing
        settings.column(at: 1).xPlacement = .fill
        settings.columnSpacing = 12
        settings.rowSpacing = 8

        let separator = NSBox()
        separator.boxType = .separator

        let content = NSView()
        let stack = NSStackView(views: [generation, activity, separator, settings, installRow])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.setCustomSpacing(5, after: generation)
        stack.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor, constant: -16),
            settings.widthAnchor.constraint(equalTo: stack.widthAnchor),
            separator.widthAnchor.constraint(equalTo: stack.widthAnchor),
            activity.widthAnchor.constraint(equalTo: stack.widthAnchor),
            activity.heightAnchor.constraint(greaterThanOrEqualToConstant: 16),
            installRow.widthAnchor.constraint(equalTo: stack.widthAnchor),
        ])

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 430, height: 226),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "FrameGen"
        window.contentView = content
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.center()
        return Controls(window: window, crossover: crossover, bottle: bottle, install: install,
                        generation: generation, installation: installation, activity: activity)
    }

    private var hasBottle: Bool {
        bottle.map(bottles.contains) ?? false
    }

    private func bottleTitle(_ choice: Bottle) -> String {
        guard bottles.contains(where: { $0.title == choice.title && $0 != choice }),
              let path = choice.path else { return choice.title }
        let parent = (path as NSString).deletingLastPathComponent
        return "\(choice.title) — \((parent as NSString).abbreviatingWithTildeInPath)"
    }

    private func refreshControls() {
        let view = controls
        view.bottle.removeAllItems()
        if bottles.isEmpty { view.bottle.addItem(withTitle: "No bottles found") }
        for choice in bottles {
            view.bottle.addItem(withTitle: bottleTitle(choice))
            guard let row = view.bottle.lastItem else { continue }
            row.representedObject = choice
            row.toolTip = choice.path
            if choice == bottle { view.bottle.select(row) }
        }
        view.bottle.isEnabled = !busy && !bottles.isEmpty

        let usesCrossOver = bottle?.provider == .crossover
        view.crossover.removeAllItems()
        if usesCrossOver {
            view.crossover.addItem(withTitle: "Automatic")
            for path in crossoverPaths {
                view.crossover.addItem(withTitle: "")
                guard let row = view.crossover.lastItem else { continue }
                row.title = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
                row.representedObject = path
                row.toolTip = path
                if path == crossover { view.crossover.select(row) }
            }
        } else {
            view.crossover.addItem(withTitle: hasBottle ? "Managed by MoltenVR" : "Select a bottle")
        }
        view.crossover.isEnabled = !busy && hasBottle && usesCrossOver && !crossoverPaths.isEmpty
        view.install.isEnabled = !busy && hasBottle && (!usesCrossOver || !crossoverPaths.isEmpty)
        refreshStatusViews()
    }

    private func refreshStatusViews() {
        let view = controls
        let canToggle = !busy && hasBottle && installation?.state == .current && activity != nil
        let state: NSControl.StateValue = activity?.enabled == true ? .on : .off
        let installationText: String
        if let installation {
            installationText = installation.description
        } else if hasBottle {
            installationText = activityUnavailable ? "Installation status unavailable" : "Checking installation…"
        } else {
            installationText = "Select a bottle"
        }
        let activityText: String
        if let message {
            activityText = message
        } else if !hasBottle {
            activityText = "No CrossOver or MoltenVR bottles found"
        } else if installation == nil {
            activityText = activityUnavailable ? "Could not check the installation" : "Checking game status…"
        } else if installation?.state != .current {
            activityText = "Install the current version to enable frame generation"
        } else if activityUnavailable {
            activityText = "Could not read the game status"
        } else {
            activityText = activity?.description ?? "Checking game status…"
        }

        view.generation.isEnabled = canToggle
        view.generation.state = state
        view.install.title = installation?.actionTitle ?? "Install"
        view.install.isHidden = installation?.state == .current
        view.installation.stringValue = installationText
        view.activity.stringValue = activityText
        menuGeneration?.isEnabled = canToggle
        menuGeneration?.state = state
        menuInstallation?.title = installationText
        menuActivity?.title = activityText
        item.button?.image = activity?.state == .active && !activityUnavailable ? activeIcon : inactiveIcon
        item.button?.toolTip = "FrameGen · \(activityText)"
    }

    private func menuItem(_ title: String, action: Selector? = nil, enabled: Bool = true) -> NSMenuItem {
        let row = NSMenuItem(title: title, action: action, keyEquivalent: "")
        row.target = self
        row.isEnabled = enabled && action != nil
        return row
    }

    private func renderMenu() {
        let menu = NSMenu()
        menu.delegate = self
        menu.autoenablesItems = false
        let generation = menuItem("Frame Generation", action: #selector(toggle))
        let activity = menuItem("")
        let installation = menuItem("")
        menuGeneration = generation
        menuActivity = activity
        menuInstallation = installation
        menu.addItem(generation)
        menu.addItem(activity)
        menu.addItem(installation)
        let bottlePicker = NSMenuItem(title: bottle.map { "Bottle · \($0.title)" } ?? "Bottle",
                                      action: nil, keyEquivalent: "")
        let bottleMenu = NSMenu()
        bottleMenu.autoenablesItems = false
        for choice in bottles {
            let row = menuItem(bottleTitle(choice), action: #selector(selectMenuBottle(_:)), enabled: !busy)
            row.representedObject = choice
            row.toolTip = choice.path
            row.state = choice == bottle ? .on : .off
            bottleMenu.addItem(row)
        }
        if bottles.isEmpty { bottleMenu.addItem(menuItem("No bottles found")) }
        bottlePicker.submenu = bottleMenu
        menu.addItem(bottlePicker)
        menu.addItem(.separator())
        menu.addItem(menuItem("Open FrameGen…", action: #selector(showControls)))
        menu.addItem(menuItem("Refresh", action: #selector(refresh), enabled: !busy))
        menu.addItem(menuItem("Open Log", action: #selector(openLog),
                              enabled: FileManager.default.fileExists(atPath: logURL.path)))
        menu.addItem(menuItem("Quit", action: #selector(quit), enabled: !busy))
        item.menu = menu
        refreshControls()
    }

    private func lines(_ output: String) -> [String] {
        output.split(whereSeparator: \.isNewline).map(String.init)
    }

    private func begin(_ title: String) {
        busy = true
        message = title
        renderMenu()
    }

    private func finish(_ title: String? = nil) {
        busy = false
        message = title
        renderMenu()
    }

    @objc private func refresh() {
        guard !busy else { return }
        begin("Finding bottles…")
        command(["crossovers"]) { [self] result in
            if result.succeeded {
                crossoverPaths = lines(result.output)
                if let crossover, !crossoverPaths.contains(crossover) {
                    self.crossover = nil
                    defaults.removeObject(forKey: "SelectedCrossOver")
                }
            }
            command(["bottles", "--provider", "all"]) { [self] result in
                guard result.succeeded else {
                    bottles = []
                    bottle = nil
                    installation = nil
                    activity = nil
                    finish("Could not find bottles")
                    return
                }
                bottles = result.output.split(whereSeparator: \.isNewline)
                    .compactMap(Bottle.init(record:))
                    .sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
                if !hasBottle {
                    let prior = bottle
                    let migrated = bottles.first { $0.provider == prior?.provider && $0.name == prior?.name }
                    setBottle(migrated ?? bottles.first)
                }
                loadStatus()
            }
        }
    }

    private func updateStatus(_ result: CommandResult) -> Bool {
        let fields = result.fields
        guard result.succeeded, let newInstallation = Installation(fields),
              let newActivity = Activity(fields[1...3], issue: fields.count == 7 ? fields[6] : nil) else { return false }
        installation = newInstallation
        updateActivity(newActivity)
        activityUnavailable = false
        return true
    }

    private func updateActivity(_ current: Activity) {
        activity = current
        if current.state == .idle { lastLoggedIssue = nil }
        guard let issue = current.issue, current.state == .unsupported || current.state == .unsupportedD3D12 else { return }
        let record = "\(current.game ?? "OpenXR application")\t\(issue)"
        guard record != lastLoggedIssue else { return }
        lastLoggedIssue = record
        let log = logURL
        queue.async {
            Self.writeLog(log, command: "activity", result: CommandResult(status: 0, output: record, diagnostics: ""))
        }
    }

    private func loadStatus() {
        guard let bottle else {
            installation = nil
            activity = nil
            finish()
            return
        }
        command(["status"] + bottle.arguments) { [self] result in
            guard updateStatus(result) else {
                installation = nil
                activity = nil
                activityUnavailable = true
                finish()
                return
            }
            finish()
        }
    }

    @objc private func pollActivity() {
        switch installation?.state {
        case .current: poll(refreshInstallation: false)
        // Wine may report the registry write before persisting system.reg.
        case .partial: poll(refreshInstallation: true)
        default: break
        }
    }

    private func poll(refreshInstallation: Bool) {
        guard !busy, !polling, hasBottle, let bottle else { return }
        polling = true
        let checkInstallation = refreshInstallation || installation == nil
        let operation = checkInstallation ? "status" : "activity"
        command([operation] + bottle.arguments, record: false) { [self] result in
            polling = false
            guard self.bottle == bottle, !busy else { return }
            let oldInstallation = installation
            let oldActivity = activity
            let wasUnavailable = activityUnavailable
            let fields = result.fields
            if checkInstallation {
                activityUnavailable = !updateStatus(result)
            } else if result.succeeded, (fields.count == 3 || fields.count == 4),
                      let current = Activity(fields.prefix(3), issue: fields.count == 4 ? fields[3] : nil) {
                updateActivity(current)
                activityUnavailable = false
            } else {
                activityUnavailable = true
            }
            if oldInstallation != installation || oldActivity != activity || wasUnavailable != activityUnavailable {
                refreshStatusViews()
            }
        }
    }

    @objc private func changeCrossOver(_ sender: NSPopUpButton) {
        guard !busy, bottle?.provider == .crossover else { return }
        crossover = sender.selectedItem?.representedObject as? String
        defaults.set(crossover, forKey: "SelectedCrossOver")
    }

    private func setBottle(_ choice: Bottle?) {
        bottle = choice
        installation = nil
        activity = nil
        activityUnavailable = false
        lastLoggedIssue = nil
        defaults.set(choice?.name, forKey: "SelectedBottle")
        defaults.set(choice?.provider.rawValue, forKey: "SelectedBottleProvider")
        defaults.set(choice?.path, forKey: "SelectedBottlePath")
    }

    @objc private func changeBottle(_ sender: NSPopUpButton) {
        selectBottle(sender.selectedItem?.representedObject as? Bottle)
    }

    @objc private func selectMenuBottle(_ sender: NSMenuItem) {
        selectBottle(sender.representedObject as? Bottle)
    }

    private func selectBottle(_ choice: Bottle?) {
        guard !busy, let choice,
              bottles.contains(choice), choice != bottle else { return }
        setBottle(choice)
        begin("Checking \(choice.name)…")
        loadStatus()
    }

    @objc private func install() {
        guard !busy, let bottle else { return }
        let crossoverArguments = bottle.provider == .crossover ? crossover.map { ["--crossover", $0] } ?? [] : []
        begin("Installing FrameGen…")
        command(["install"] + bottle.arguments + crossoverArguments) { [self] result in
            if result.succeeded {
                loadStatus()
            } else {
                reportError("Installation failed", result: result)
            }
        }
    }

    @objc private func toggle() {
        guard !busy, let bottle, installation?.state == .current, let activity else { return }
        let mode = activity.enabled ? "off" : "on"
        begin("Turning Frame Generation \(mode)…")
        command(["mode"] + bottle.arguments + ["--mode", mode]) { [self] result in
            if result.succeeded {
                loadStatus()
            } else {
                reportError("Could not switch Frame Generation \(mode)", result: result)
            }
        }
    }

    private func reportError(_ title: String, result: CommandResult) {
        finish(title)
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        let detail = result.diagnostics.isEmpty ? result.output : result.diagnostics
        alert.informativeText = detail.isEmpty ? "The command exited with status \(result.status)." : String(detail.prefix(700))
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Open Log")
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertSecondButtonReturn { openLog() }
        message = nil
        refreshStatusViews()
    }

    @objc private func openLog() {
        NSWorkspace.shared.open(logURL)
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    private func command(_ arguments: [String], record: Bool = true,
                         completion: @escaping @MainActor @Sendable (CommandResult) -> Void) {
        let executable = Bundle.main.resourceURL?.appendingPathComponent("FrameGenCtl")
        let log = logURL
        queue.async {
            let result = Self.execute(executable, arguments: arguments)
            if record && !result.succeeded {
                Self.writeLog(log, command: arguments.first ?? "", result: result)
            }
            DispatchQueue.main.async { completion(result) }
        }
    }

    private nonisolated static func execute(_ executable: URL?, arguments: [String]) -> CommandResult {
        let files = FileManager.default
        guard let executable, files.isExecutableFile(atPath: executable.path) else {
            return CommandResult(status: -1, output: "", diagnostics: "FrameGenCtl is missing from the application.")
        }
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.standardInput = FileHandle.nullDevice
        do {
            if arguments.first != "install" {
                let pipe = Pipe()
                process.standardOutput = pipe
                process.standardError = pipe
                try process.run()
                try? pipe.fileHandleForWriting.close()
                let text = String(decoding: try pipe.fileHandleForReading.readToEnd() ?? Data(), as: UTF8.self)
                process.waitUntilExit()
                let succeeded = process.terminationStatus == 0
                return CommandResult(status: process.terminationStatus,
                                     output: succeeded ? text : "", diagnostics: succeeded ? "" : text)
            }

            // An installer can start a Wine daemon that inherits its handles.
            // Capture its output in files so the daemon cannot hold a pipe open.
            let capture = files.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            try files.createDirectory(at: capture, withIntermediateDirectories: false,
                                      attributes: [.posixPermissions: 0o700])
            defer { try? files.removeItem(at: capture) }
            let stdout = capture.appendingPathComponent("stdout")
            let stderr = capture.appendingPathComponent("stderr")
            try Data().write(to: stdout)
            try Data().write(to: stderr)
            let outputHandle = try FileHandle(forWritingTo: stdout)
            defer { try? outputHandle.close() }
            let errorHandle = try FileHandle(forWritingTo: stderr)
            defer { try? errorHandle.close() }

            process.standardOutput = outputHandle
            process.standardError = errorHandle
            try process.run()
            process.waitUntilExit()
            return CommandResult(
                status: process.terminationStatus,
                output: String(decoding: try Data(contentsOf: stdout), as: UTF8.self),
                diagnostics: String(decoding: try Data(contentsOf: stderr), as: UTF8.self)
            )
        } catch {
            return CommandResult(status: -1, output: "", diagnostics: error.localizedDescription)
        }
    }

    private nonisolated static func writeLog(_ url: URL, command: String, result: CommandResult) {
        let files = FileManager.default
        do {
            try files.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            guard files.fileExists(atPath: url.path) ||
                    files.createFile(atPath: url.path, contents: nil, attributes: [.posixPermissions: 0o600]) else {
                throw CocoaError(.fileWriteUnknown)
            }
            let handle = try FileHandle(forWritingTo: url)
            defer { try? handle.close() }
            try handle.seekToEnd()
            let detail = result.output + result.diagnostics
            let stamp = ISO8601DateFormatter().string(from: Date())
            try handle.write(contentsOf: Data("\n[\(stamp)] \(command) returned \(result.status)\n\(detail)\n".utf8))
        } catch {
            NSLog("FrameGenMenu could not write its log: %@", error.localizedDescription)
        }
    }
}

let application = NSApplication.shared
private let menuController = MenuController()
application.setActivationPolicy(.accessory)
application.delegate = menuController
application.run()
