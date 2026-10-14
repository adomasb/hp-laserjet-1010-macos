import AppKit
import Darwin

// Installation is completed before the caller starts a helper or creates its window.
enum AppInstallation {
    static func offerIfNeeded(completion: @escaping (Bool) -> Void) {
        precondition(Thread.isMainThread)
        let source = Bundle.main.bundleURL
        let applications = InstallationTransaction.applicationsDirectory
        guard !InstallationTransaction.isInstalled(source) else {
            completion(true)
            return
        }
        let prompt = NSAlert()
        prompt.messageText = "Move to Applications?"
        prompt.informativeText = "Install HP LaserJet 1010 in the main Applications folder (/Applications), then open the installed copy. The original copy will be kept."
        prompt.addButton(withTitle: "Move to Applications")
        prompt.addButton(withTitle: "Not Now")
        NSApp.activate(ignoringOtherApps: true)
        guard prompt.runModal() == .alertFirstButtonReturn else { completion(true); return }
        let transaction = InstallationTransaction(source: source, directory: applications,
                                                  operations: .live)
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let replacing = try transaction.requiresReplacement()
                DispatchQueue.main.async {
                    if replacing {
                        let replace = NSAlert()
                        replace.messageText = "Replace the installed app?"
                        replace.informativeText = "An existing HP LaserJet 1010 is already in /Applications. Replace it with this copy? If it is running, it will be asked to quit first."
                        replace.addButton(withTitle: "Replace")
                        replace.addButton(withTitle: "Cancel")
                        guard replace.runModal() == .alertFirstButtonReturn else { completion(true); return }
                    }
                    DispatchQueue.global(qos: .userInitiated).async {
                        do {
                            try transaction.install(replaceExisting: replacing)
                            DispatchQueue.main.async { completion(false) }
                        } catch {
                            DispatchQueue.main.async { report(error, completion: completion) }
                        }
                    }
                }
            } catch {
                DispatchQueue.main.async { report(error, completion: completion) }
            }
        }
    }

    private static func report(_ error: Error, completion: @escaping (Bool) -> Void) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Could not install the app"
        alert.informativeText = error.localizedDescription + "\n\nIf you do not have permission to install in /Applications, drag the app there in Finder and approve the administrator prompt. You can continue using this copy."
        alert.addButton(withTitle: "Continue")
        alert.runModal()
        completion(true)
    }
}

struct InstallationFailure: LocalizedError {
    let message: String
    var errorDescription: String? { message }
    init(_ message: String) { self.message = message }
}

// Only this backend touches files. Tests inject all operations that can start a
// process or interact with running applications, and use temporary directories.
struct InstallationOperations {
    var copy: (URL, URL) throws -> Void
    var verify: (URL) throws -> Void
    var stopInstalledApp: (URL, String) throws -> Void
    var launch: (URL) throws -> Void
    var move: (URL, URL) throws -> Void

    static var live: InstallationOperations {
        InstallationOperations(copy: { source, target in
            try run("/usr/bin/ditto", [source.path, target.path], timeout: 90)
        }, verify: { app in
            try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path], timeout: 20)
        }, stopInstalledApp: { url, identifier in
            // Stop the old per-user installation as well when migrating to
            // /Applications. Never terminate this source installer or another app.
            let legacy = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Applications/LaserJet 1010 Helper.app")
            let locations = [url, legacy].map { InstallationTransaction.canonical($0) }
            let matches: [NSRunningApplication] = DispatchQueue.main.sync {
                NSWorkspace.shared.runningApplications.filter {
                    $0.processIdentifier != ProcessInfo.processInfo.processIdentifier &&
                    $0.bundleIdentifier == identifier && $0.bundleURL.map {
                        locations.contains(InstallationTransaction.canonical($0))
                    } == true
                }
            }
            DispatchQueue.main.sync { for app in matches { _ = app.terminate() } }
            let deadline = ProcessInfo.processInfo.systemUptime + 12
            while matches.contains(where: { !$0.isTerminated }) && ProcessInfo.processInfo.systemUptime < deadline {
                Thread.sleep(forTimeInterval: 0.1)
            }
            guard matches.allSatisfy({ $0.isTerminated }) else {
                throw InstallationFailure("The installed app did not quit within 12 seconds. Quit it and try again.")
            }
        }, launch: { url in
            let result = InstallationLaunchResult()
            DispatchQueue.main.async {
                let configuration = NSWorkspace.OpenConfiguration()
                configuration.createsNewApplicationInstance = true
                configuration.activates = true
                NSWorkspace.shared.openApplication(at: url, configuration: configuration) { app, error in
                    if let app = app, app.bundleURL.map({ InstallationTransaction.canonical($0) }) != InstallationTransaction.canonical(url) {
                        result.complete(app: nil, error: InstallationFailure("macOS opened a different copy of the app."))
                        return
                    }
                    result.complete(app: app, error: error)
                }
            }
            try result.wait(timeout: 30)
        }, move: { try FileManager.default.moveItem(at: $0, to: $1) })
    }

    private static func run(_ executable: String, _ arguments: [String], timeout: TimeInterval) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        let done = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in done.signal() }
        try process.run()
        if done.wait(timeout: .now() + timeout) == .timedOut {
            // Only our own copy/signature utility is stopped; never kill an app.
            process.terminate()
            if done.wait(timeout: .now() + 2) == .timedOut {
                kill(process.processIdentifier, SIGKILL)
                _ = done.wait(timeout: .now() + 2)
            }
            throw InstallationFailure("\(URL(fileURLWithPath: executable).lastPathComponent) timed out. Installation was cancelled.")
        }
        guard process.terminationStatus == 0 else {
            throw InstallationFailure("\(URL(fileURLWithPath: executable).lastPathComponent) failed (status \(process.terminationStatus)).")
        }
    }
}

private final class InstallationLaunchResult {
    private let lock = NSLock()
    private let done = DispatchSemaphore(value: 0)
    private var expired = false
    private var completed = false
    private var failure: Error?

    func complete(app: NSRunningApplication?, error: Error?) {
        lock.lock()
        if expired {
            lock.unlock()
            // A late launch must not keep the replacement running after rollback.
            if let app = app { DispatchQueue.main.async { _ = app.terminate() } }
            return
        }
        failure = error ?? (app == nil ? InstallationFailure("The installed app did not open.") : nil)
        completed = true
        lock.unlock()
        done.signal()
    }

    func wait(timeout: TimeInterval) throws {
        if done.wait(timeout: .now() + timeout) == .timedOut {
            lock.lock()
            let didComplete = completed
            expired = !didComplete
            let error = failure
            lock.unlock()
            if !didComplete { throw InstallationFailure("Opening the installed app timed out.") }
            if let error = error { throw error }
            return
        }
        lock.lock()
        let error = failure
        lock.unlock()
        if let error = error { throw error }
    }
}

struct InstallationTransaction {
    static let identifier = "dev.local.hp1010.helper"
    static let applicationsDirectory = URL(fileURLWithPath: "/Applications", isDirectory: true)
    let source: URL
    let directory: URL
    let operations: InstallationOperations
    private let files = FileManager.default
    var destination: URL { directory.appendingPathComponent("LaserJet 1010 Helper.app", isDirectory: true) }

    static func canonical(_ url: URL) -> URL { url.standardizedFileURL.resolvingSymlinksInPath() }

    static func isInstalled(_ app: URL, applications: URL = applicationsDirectory) -> Bool {
        let path = canonical(app).path
        let root = canonical(applications).path
        return path.hasPrefix(root + "/")
    }

    func requiresReplacement() throws -> Bool {
        guard exists(destination) else { return false }
        try validateApp(destination)
        return true
    }

    func install(replaceExisting: Bool) throws {
        try validateApp(source)
        let existing = try requiresReplacement()
        guard !existing || replaceExisting else { throw InstallationFailure("Replacement was not approved.") }
        try files.createDirectory(at: directory, withIntermediateDirectories: true)
        let temporary = directory.appendingPathComponent(".hp1010-install-\(UUID().uuidString)", isDirectory: true)
        try files.createDirectory(at: temporary, withIntermediateDirectories: false,
                                  attributes: [.posixPermissions: 0o700])
        let staged = temporary.appendingPathComponent(destination.lastPathComponent, isDirectory: true)
        let backup = temporary.appendingPathComponent("Previous.app", isDirectory: true)
        var preserveBackup = false
        defer { if !preserveBackup { try? files.removeItem(at: temporary) } }
        try operations.copy(source, staged)
        try validateApp(staged)
        try Self.removeSigningDetritus(in: staged)
        try operations.verify(staged)
        // Repeat validation after staging; the destination may have changed.
        let nowExisting = try requiresReplacement()
        guard existing == nowExisting else { throw InstallationFailure("The destination changed during installation. Please try again.") }
        try operations.stopInstalledApp(destination, Self.identifier)
        if nowExisting {
            try validateApp(destination)
            try operations.move(destination, backup)
        }
        var placedReplacement = false
        do {
            try operations.move(staged, destination)
            placedReplacement = true
            try operations.launch(destination)
        } catch {
            do {
                // A failed stage rename leaves no new destination to remove.
                if placedReplacement { try files.removeItem(at: destination) }
                if nowExisting { try operations.move(backup, destination) }
            } catch let rollbackError {
                preserveBackup = true
                throw InstallationFailure("Installation failed and the previous app could not be restored: \(rollbackError.localizedDescription). The recovery files are at \(temporary.path).")
            }
            throw error
        }
        // Keep the previous version until NSWorkspace confirms a successful open.
    }

    // Finder can add these two attributes merely by opening a downloaded app.
    // Clean the private staged copy only; preserve quarantine and all other
    // attributes, and never traverse or modify the target of a symbolic link.
    static func removeSigningDetritus(in root: URL) throws {
        let files = FileManager.default
        let deadline = ProcessInfo.processInfo.systemUptime + 30
        var enumerationError: Error?
        guard let entries = files.enumerator(at: root, includingPropertiesForKeys: nil,
                                            options: [], errorHandler: { _, error in
            enumerationError = error
            return false
        }) else { throw InstallationFailure("Could not inspect the staged app’s metadata.") }

        func clean(_ url: URL) throws {
            guard ProcessInfo.processInfo.systemUptime < deadline else {
                throw InstallationFailure("Cleaning the staged app’s metadata timed out.")
            }
            let type = try files.attributesOfItem(atPath: url.path)[.type] as? FileAttributeType
            if type == .typeSymbolicLink { return }
            for attribute in ["com.apple.FinderInfo", "com.apple.ResourceFork"] {
                if removexattr(url.path, attribute, XATTR_NOFOLLOW) != 0 {
                    let code = errno
                    if code != ENOATTR {
                        throw NSError(domain: NSPOSIXErrorDomain, code: Int(code),
                                      userInfo: [NSLocalizedDescriptionKey: "Could not remove \(attribute) from the staged app: \(String(cString: strerror(code)))."])
                    }
                }
            }
        }

        try clean(root)
        while let entry = entries.nextObject() as? URL {
            let type = try files.attributesOfItem(atPath: entry.path)[.type] as? FileAttributeType
            if type == .typeSymbolicLink { entries.skipDescendants(); continue }
            try clean(entry)
        }
        if let error = enumerationError { throw error }
    }

    private func exists(_ url: URL) -> Bool {
        // attributesOfItem also sees dangling symbolic links.
        (try? files.attributesOfItem(atPath: url.path)) != nil
    }

    private func validateApp(_ url: URL) throws {
        let attributes = try files.attributesOfItem(atPath: url.path)
        guard attributes[.type] as? FileAttributeType == .typeDirectory,
              url.pathExtension.lowercased() == "app",
              let data = try? Data(contentsOf: url.appendingPathComponent("Contents/Info.plist")),
              let info = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any],
              info["CFBundleIdentifier"] as? String == Self.identifier,
              info["CFBundlePackageType"] as? String == "APPL",
              let executable = info["CFBundleExecutable"] as? String,
              !executable.isEmpty, executable == (executable as NSString).lastPathComponent,
              files.isExecutableFile(atPath: url.appendingPathComponent("Contents/MacOS").appendingPathComponent(executable).path) else {
            throw InstallationFailure("\(url.path) is not a regular HP LaserJet 1010 app bundle. It will not be replaced.")
        }
    }
}
