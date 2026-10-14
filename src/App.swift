import AppKit
import ServiceManagement

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var item: NSStatusItem!
    private let printingService = PrintingService()
    private var timer: Timer?
    private var status = NSTextField(wrappingLabelWithString: "Stopped")
    private var detail = NSTextField(wrappingLabelWithString: "")
    private var startButton = NSButton()
    private var retryButton = NSButton()
    private var loginButton = NSButton()
    private var repairItems: [NSMenuItem] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        if let iconURL = Bundle.main.url(forResource: "AppIcon", withExtension: "icns"),
           let icon = NSImage(contentsOf: iconURL) {
            NSApp.applicationIconImage = icon
        }
        AppInstallation.offerIfNeeded { [weak self] continueHere in
            if continueHere { self?.launchPrintingApp() }
            else { NSApp.terminate(nil) }
        }
    }

    private func launchPrintingApp() {
        let mainMenu = NSMenu()
        let applicationItem = NSMenuItem()
        let applicationMenu = NSMenu()
        applicationMenu.autoenablesItems = false
        let repair = applicationMenu.addItem(withTitle: "Repair Printer Setup", action: #selector(repairPrinter), keyEquivalent: "")
        repair.target = self
        repair.isEnabled = false
        repairItems.append(repair)
        applicationMenu.addItem(.separator())
        let quitItem = applicationMenu.addItem(withTitle: "Quit HP LaserJet 1010", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        applicationItem.submenu = applicationMenu
        mainMenu.addItem(applicationItem)
        NSApp.mainMenu = mainMenu
        let menu = NSMenu()
        menu.autoenablesItems = false
        menu.addItem(withTitle: "Show HP LaserJet 1010", action: #selector(showWindow), keyEquivalent: "")
        menu.addItem(withTitle: "Start / Stop Printing Service", action: #selector(toggleService), keyEquivalent: "")
        let menuRepair = menu.addItem(withTitle: "Repair Printer Setup", action: #selector(repairPrinter), keyEquivalent: "")
        menuRepair.isEnabled = false
        repairItems.append(menuRepair)
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit HP LaserJet 1010", action: #selector(quit), keyEquivalent: "q")
        for child in menu.items { child.target = self }
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = NSImage(systemSymbolName: "printer", accessibilityDescription: "HP LaserJet 1010")
        item.menu = menu
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 540, height: 370),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "HP LaserJet 1010"
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.center()
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        window.contentView!.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor, constant: 26),
            stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor, constant: -26),
            stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor, constant: 24)
        ])
        let title = NSTextField(labelWithString: "HP LaserJet 1010")
        title.font = .systemFont(ofSize: 25, weight: .semibold)
        stack.addArrangedSubview(title)
        let intro = NSTextField(wrappingLabelWithString: "Print from Preview or your browser with ⌘P. The app stays in the menu bar when you close this window.")
        intro.font = .systemFont(ofSize: 13)
        stack.addArrangedSubview(intro)
        status.font = .systemFont(ofSize: 15, weight: .semibold)
        stack.addArrangedSubview(status)
        detail.font = .systemFont(ofSize: 12)
        detail.textColor = .secondaryLabelColor
        detail.preferredMaxLayoutWidth = 475
        stack.addArrangedSubview(detail)
        let controls = NSStackView()
        controls.spacing = 12
        startButton = NSButton(title: "Start Printing Service", target: self, action: #selector(toggleService))
        retryButton = NSButton(title: "Retry Setup", target: self, action: #selector(retrySetup))
        controls.addArrangedSubview(startButton)
        controls.addArrangedSubview(retryButton)
        retryButton.isHidden = true
        stack.addArrangedSubview(controls)
        loginButton = NSButton(checkboxWithTitle: "Open at login", target: self, action: #selector(toggleLogin))
        loginButton.state = SMAppService.mainApp.status == .enabled ? .on : .off
        stack.addArrangedSubview(loginButton)
        let foot = NSTextField(wrappingLabelWithString: "Local to this Mac • 600 dpi • A4 / Letter • one-sided\nConnect the printer by USB and select HP LaserJet 1010 in the print dialog.")
        foot.font = .systemFont(ofSize: 11)
        foot.textColor = .secondaryLabelColor
        stack.addArrangedSubview(foot)
        printingService.onChange = { [weak self] in self?.refresh() }
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.printingService.poll() }
        if !CommandLine.arguments.contains("--login") { showWindow() }
        printingService.start()
    }

    @objc private func showWindow() {
        // Finder can reopen the source while the installation transaction is running.
        guard let window = window else { return }
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
    }
    @objc private func quit() { NSApp.terminate(nil) }
    @objc private func toggleService() {
        if printingService.running { printingService.stop() } else { printingService.start() }
    }
    private func refresh() {
        let running = printingService.running
        let stopping = printingService.stopping
        let setupState = printingService.setupState
        let serviceReady = printingService.serviceReady
        let startupError = printingService.startupError
        let configuring: Bool
        if case .configuring = setupState { configuring = true } else { configuring = false }
        startButton.title = stopping ? "Stopping…" : (running ? "Stop Printing Service" : "Start Printing Service")
        startButton.isEnabled = !stopping
        if case .failed = setupState { retryButton.isHidden = false } else { retryButton.isHidden = true }
        retryButton.isEnabled = running && !stopping && !configuring
        for repair in repairItems { repair.isEnabled = serviceReady && !configuring }
        status.textColor = .labelColor
        if let error = startupError {
            status.stringValue = "Printing service needs attention"
            status.textColor = .systemRed
            detail.stringValue = error
        } else if stopping {
            status.stringValue = "Stopping printing service…"
            detail.stringValue = "Active jobs will be cancelled. Partially transferred jobs are never retried automatically."
        } else if !running {
            status.stringValue = "Printing service stopped"
            detail.stringValue = "Start the printing service before printing."
        } else {
            switch setupState {
            case .waiting:
                status.stringValue = "Starting printing service…"
                detail.stringValue = "Your printer will be set up automatically."
            case .configuring:
                status.stringValue = "Setting up printer…"
                detail.stringValue = "Adding HP LaserJet 1010 to this Mac if needed."
            case .failed(let message):
                status.stringValue = "Printer setup needs attention"
                status.textColor = .systemRed
                detail.stringValue = message
            case .ready:
                status.stringValue = "Printing service running"
                detail.stringValue = (printingService.state?.lastJobID ?? 0) > 0
                    ? (printingService.state?.message ?? "Printing service running.")
                    : "Choose HP LaserJet 1010 in your app’s print dialog."
            }
        }
    }
    @objc private func retrySetup() { printingService.retrySetup() }
    @objc private func repairPrinter() { printingService.configure(repair: true) }
    @objc private func toggleLogin() {
        do {
            if loginButton.state == .on { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
            if SMAppService.mainApp.status == .requiresApproval {
                alert("Login launch needs approval", "Enable HP LaserJet 1010 in System Settings → General → Login Items & Extensions.")
            }
        } catch { alert("Login launch unavailable", "Install the app in your Applications folder before enabling login launch. \(error.localizedDescription)") }
        loginButton.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }
    private func alert(_ title: String, _ message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: "OK")
        alert.beginSheetModal(for: window)
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        timer?.invalidate()
        return printingService.shutdown { NSApp.reply(toApplicationShouldTerminate: true) }
            ? .terminateLater : .terminateNow
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool { showWindow(); return true }
}

@main
private enum LaserJetApplication {
    static func main() {
        let application = NSApplication.shared
        let delegate = AppDelegate()
        application.delegate = delegate
        withExtendedLifetime(delegate) { application.run() }
    }
}
