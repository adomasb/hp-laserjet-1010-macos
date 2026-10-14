import AppKit
import Darwin

private enum TestFailure: Error { case injected(String), assertion(String) }

@main
struct InstallationTests {
    static let files = FileManager.default
    static var count = 0

    static func check(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
        if try !value() { throw TestFailure.assertion(message) }
    }

    static func fails(_ operation: () throws -> Void) throws {
        do { try operation() } catch { return }
        throw TestFailure.assertion("Expected an error")
    }

    static func fixture(_ body: (URL, URL, URL) throws -> Void) throws {
        let root = files.temporaryDirectory.appendingPathComponent("hp1010-install-tests-\(UUID().uuidString)")
        try files.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? files.removeItem(at: root) }
        let source = root.appendingPathComponent("Downloads/Source.app")
        let directory = root.appendingPathComponent("Applications")
        try makeApp(source, marker: "new")
        try body(root, source, directory)
        count += 1
    }

    static func makeApp(_ app: URL, marker: String, identifier: String = InstallationTransaction.identifier) throws {
        try files.createDirectory(at: app.appendingPathComponent("Contents"), withIntermediateDirectories: true)
        try files.createDirectory(at: app.appendingPathComponent("Contents/MacOS"), withIntermediateDirectories: true)
        let executable = app.appendingPathComponent("Contents/MacOS/TestExecutable")
        try Data("test fixture; never executed".utf8).write(to: executable)
        try files.setAttributes([.posixPermissions: 0o700], ofItemAtPath: executable.path)
        let plist: [String: Any] = ["CFBundleIdentifier": identifier, "CFBundlePackageType": "APPL", "CFBundleExecutable": "TestExecutable"]
        try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
            .write(to: app.appendingPathComponent("Contents/Info.plist"))
        try Data(marker.utf8).write(to: app.appendingPathComponent("marker"))
    }

    static func marker(_ app: URL) throws -> String {
        try String(contentsOf: app.appendingPathComponent("marker"), encoding: .utf8)
    }

    static func setAttribute(_ name: String, value: Data, at url: URL) throws {
        let result = value.withUnsafeBytes {
            setxattr(url.path, name, $0.baseAddress, $0.count, 0, XATTR_NOFOLLOW)
        }
        if result != 0 { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
    }

    static func attribute(_ name: String, at url: URL) throws -> Data? {
        let size = getxattr(url.path, name, nil, 0, 0, XATTR_NOFOLLOW)
        if size < 0 {
            if errno == ENOATTR { return nil }
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
        var data = Data(count: size)
        let read = data.withUnsafeMutableBytes {
            getxattr(url.path, name, $0.baseAddress, $0.count, 0, XATTR_NOFOLLOW)
        }
        if read < 0 { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
        return data
    }

    static func operations() -> InstallationOperations {
        InstallationOperations(copy: { try files.copyItem(at: $0, to: $1) },
                               verify: { _ in }, stopInstalledApp: { _, _ in },
                               launch: { _ in }, move: { try files.moveItem(at: $0, to: $1) })
    }

    static func clean(_ directory: URL) throws {
        let names = try files.contentsOfDirectory(atPath: directory.path)
        try check(names.allSatisfy { !$0.hasPrefix(".hp1010-install-") }, "Staging files leaked")
    }

    static func main() throws {
        try fixture { root, source, directory in
            try check(InstallationTransaction.applicationsDirectory.path == "/Applications", "Default is not Finder Applications")
            try check(!InstallationTransaction.isInstalled(files.homeDirectoryForCurrentUser.appendingPathComponent("Applications/One.app")), "Legacy personal folder should offer migration")
            try check(InstallationTransaction.isInstalled(directory.appendingPathComponent("One.app"), applications: directory), "Installation directory not detected")
            try check(InstallationTransaction.isInstalled(URL(fileURLWithPath: "/Applications/Utilities/One.app")), "System Applications not detected")
            try check(!InstallationTransaction.isInstalled(URL(fileURLWithPath: "/Applications-Other/One.app")), "System prefix boundary ignored")
            try check(!InstallationTransaction.isInstalled(root.appendingPathComponent("Applications-backup/One.app"), applications: directory), "Installation prefix boundary ignored")
            try check(!InstallationTransaction.isInstalled(source, applications: directory), "Downloads misidentified")
            try files.createDirectory(at: directory, withIntermediateDirectories: true)
            try files.createDirectory(at: directory.appendingPathComponent("One.app"), withIntermediateDirectories: false)
            let alias = root.appendingPathComponent("Alias")
            try files.createSymbolicLink(at: alias, withDestinationURL: directory)
            try check(InstallationTransaction.isInstalled(alias.appendingPathComponent("One.app"), applications: directory), "Symlink into Applications not resolved")
            let linkOut = directory.appendingPathComponent("Outside.app")
            try files.createSymbolicLink(at: linkOut, withDestinationURL: source)
            try check(!InstallationTransaction.isInstalled(linkOut, applications: directory), "Symlink out of Applications incorrectly accepted")
        }

        try fixture { _, source, directory in
            var ops = operations()
            var launches: [URL] = []
            var stopped = false
            ops.stopInstalledApp = { _, _ in stopped = true }
            ops.launch = { url in
                try check(stopped, "Did not stop legacy installation before fresh launch")
                launches.append(url)
            }
            let transaction = InstallationTransaction(source: source, directory: directory, operations: ops)
            try transaction.install(replaceExisting: false)
            try check(launches == [transaction.destination], "Did not launch the exact installed URL")
            try check(try marker(source) == "new", "Source was changed")
            try check(try marker(transaction.destination) == "new", "Installed app missing")
            try clean(directory)
        }

        try fixture { root, source, directory in
            let finder = Data(repeating: 1, count: 32)
            let resource = Data("test resource fork".utf8)
            let preserved = Data("preserve security-related metadata".utf8)
            let quarantine = Data("0083;00000000;HP1010InstallationTests;".utf8)
            let custom = "dev.local.hp1010.installation.security-test"
            let markerFile = source.appendingPathComponent("marker")
            try setAttribute("com.apple.FinderInfo", value: finder, at: source)
            try setAttribute("com.apple.FinderInfo", value: finder, at: source.appendingPathComponent("Contents"))
            try setAttribute("com.apple.FinderInfo", value: finder, at: markerFile)
            try setAttribute("com.apple.ResourceFork", value: resource, at: markerFile)
            try setAttribute(custom, value: preserved, at: source)
            try setAttribute(custom, value: preserved, at: markerFile)
            try setAttribute("com.apple.quarantine", value: quarantine, at: source)
            let outside = root.appendingPathComponent("Outside")
            try files.createDirectory(at: outside, withIntermediateDirectories: false)
            let outsideFile = outside.appendingPathComponent("keep")
            try Data("outside staged tree".utf8).write(to: outsideFile)
            try setAttribute("com.apple.FinderInfo", value: finder, at: outside)
            try setAttribute("com.apple.ResourceFork", value: resource, at: outsideFile)
            try files.createSymbolicLink(at: source.appendingPathComponent("Contents/ExternalDirectory"), withDestinationURL: outside)
            try files.createSymbolicLink(at: source.appendingPathComponent("Contents/ExternalFile"), withDestinationURL: outsideFile)
            var ops = operations()
            var verified = false
            ops.copy = { from, staged in
                try files.copyItem(at: from, to: staged)
                // Foundation may update quarantine flags while copying. Supply
                // known staged metadata so this tests the cleanup itself.
                try setAttribute("com.apple.quarantine", value: quarantine, at: staged)
            }
            ops.verify = { staged in
                verified = true
                try check(try attribute("com.apple.FinderInfo", at: staged) == nil, "Root Finder metadata survived cleanup")
                try check(try attribute("com.apple.FinderInfo", at: staged.appendingPathComponent("Contents")) == nil, "Nested Finder metadata survived cleanup")
                try check(try attribute("com.apple.FinderInfo", at: staged.appendingPathComponent("marker")) == nil, "File Finder metadata survived cleanup")
                try check(try attribute("com.apple.ResourceFork", at: staged.appendingPathComponent("marker")) == nil, "Resource fork survived cleanup")
                try check(try attribute(custom, at: staged) == preserved, "Custom root metadata was removed")
                try check(try attribute(custom, at: staged.appendingPathComponent("marker")) == preserved, "Custom file metadata was removed")
                try check(try attribute("com.apple.quarantine", at: staged) == quarantine, "Quarantine metadata was removed")
            }
            let transaction = InstallationTransaction(source: source, directory: directory, operations: ops)
            try transaction.install(replaceExisting: false)
            try check(verified, "Staged metadata was not checked before verification")
            try check(try attribute("com.apple.FinderInfo", at: source) == finder, "Source Finder metadata was changed")
            try check(try attribute("com.apple.ResourceFork", at: markerFile) == resource, "Source resource fork was changed")
            try check(try attribute("com.apple.FinderInfo", at: outside) == finder, "Symlink directory target was changed")
            try check(try attribute("com.apple.ResourceFork", at: outsideFile) == resource, "Symlink file target was changed")
            try clean(directory)
        }

        try fixture { _, source, directory in
            var ops = operations()
            var events: [String] = []
            ops.verify = { _ in events.append("verify") }
            ops.stopInstalledApp = { url, identifier in
                try check(url == directory.appendingPathComponent("LaserJet 1010 Helper.app"), "Wrong running-app URL")
                try check(identifier == InstallationTransaction.identifier, "Wrong running-app identifier")
                events.append("stop")
            }
            ops.launch = { _ in events.append("launch") }
            let transaction = InstallationTransaction(source: source, directory: directory, operations: ops)
            try makeApp(transaction.destination, marker: "old")
            try check(try transaction.requiresReplacement(), "Existing app not detected")
            try fails { try transaction.install(replaceExisting: false) }
            try check(try marker(transaction.destination) == "old", "Unapproved replacement changed app")
            try transaction.install(replaceExisting: true)
            try check(events == ["verify", "stop", "launch"], "Incorrect verification/quit/launch ordering")
            try check(try marker(transaction.destination) == "new", "Replacement failed")
            try clean(directory)
        }

        for kind in ["unrelated", "file", "symlink", "dangling"] {
            try fixture { root, source, directory in
                let transaction = InstallationTransaction(source: source, directory: directory, operations: operations())
                try files.createDirectory(at: directory, withIntermediateDirectories: true)
                if kind == "unrelated" { try makeApp(transaction.destination, marker: "other", identifier: "other.app") }
                if kind == "file" { try Data("keep".utf8).write(to: transaction.destination) }
                if kind == "symlink" { try files.createSymbolicLink(at: transaction.destination, withDestinationURL: source) }
                if kind == "dangling" { try files.createSymbolicLink(at: transaction.destination, withDestinationURL: root.appendingPathComponent("missing")) }
                try fails { _ = try transaction.requiresReplacement() }
                try fails { try transaction.install(replaceExisting: true) }
                let names = try files.contentsOfDirectory(atPath: directory.path)
                try check(names == [transaction.destination.lastPathComponent], "Rejected destination was changed")
            }
        }

        for failure in ["copy", "verify", "stop", "swap", "launch"] {
            try fixture { _, source, directory in
                var ops = operations()
                var launches = 0
                ops.launch = { _ in launches += 1; if failure == "launch" { throw TestFailure.injected(failure) } }
                if failure == "copy" { ops.copy = { _, target in
                    try files.createDirectory(at: target, withIntermediateDirectories: true)
                    throw TestFailure.injected(failure)
                } }
                if failure == "verify" { ops.verify = { _ in throw TestFailure.injected(failure) } }
                if failure == "stop" { ops.stopInstalledApp = { _, _ in throw TestFailure.injected(failure) } }
                if failure == "swap" { ops.move = { from, to in
                    if from.lastPathComponent == "LaserJet 1010 Helper.app" && from.deletingLastPathComponent().lastPathComponent.hasPrefix(".hp1010-install-") {
                        throw TestFailure.injected(failure)
                    }
                    try files.moveItem(at: from, to: to)
                } }
                let transaction = InstallationTransaction(source: source, directory: directory, operations: ops)
                try makeApp(transaction.destination, marker: "old")
                try fails { try transaction.install(replaceExisting: true) }
                try check(try marker(transaction.destination) == "old", "\(failure): original not restored")
                try check(try marker(source) == "new", "\(failure): source changed")
                try check(launches == (failure == "launch" ? 1 : 0), "\(failure): premature launch")
                try clean(directory)
            }
        }

        try fixture { _, source, directory in
            var ops = operations()
            ops.launch = { _ in throw TestFailure.injected("launch") }
            let transaction = InstallationTransaction(source: source, directory: directory, operations: ops)
            try fails { try transaction.install(replaceExisting: false) }
            try check(!files.fileExists(atPath: transaction.destination.path), "Failed fresh launch left installed app")
            try clean(directory)
        }

        try fixture { _, source, directory in
            var ops = operations()
            ops.launch = { _ in throw TestFailure.injected("launch") }
            ops.move = { from, to in
                if from.lastPathComponent == "Previous.app" { throw TestFailure.injected("restore") }
                try files.moveItem(at: from, to: to)
            }
            let transaction = InstallationTransaction(source: source, directory: directory, operations: ops)
            try makeApp(transaction.destination, marker: "old")
            try fails { try transaction.install(replaceExisting: true) }
            let recovery = try files.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
                .filter { $0.lastPathComponent.hasPrefix(".hp1010-install-") }
            try check(recovery.count == 1, "Rollback failure lost recovery directory")
            try check(try marker(recovery[0].appendingPathComponent("Previous.app")) == "old", "Rollback failure lost previous app")
        }

        try fixture { _, source, directory in
            var ops = operations()
            ops.copy = { from, to in
                try files.copyItem(at: from, to: to)
                try files.createSymbolicLink(at: directory.appendingPathComponent("LaserJet 1010 Helper.app"), withDestinationURL: source)
            }
            let transaction = InstallationTransaction(source: source, directory: directory, operations: ops)
            try fails { try transaction.install(replaceExisting: true) }
            let type = try files.attributesOfItem(atPath: transaction.destination.path)[.type] as? FileAttributeType
            try check(type == .typeSymbolicLink, "Destination race changed the unexpected symlink")
            try clean(directory)
        }
        print("PASS \(count) installation scenarios; temporary files only, no real application launch or Applications changes")
    }
}
