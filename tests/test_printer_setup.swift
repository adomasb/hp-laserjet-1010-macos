import Foundation
import Darwin

private func require(_ condition: @autoclosure () -> Bool, _ message: String) {
    if !condition() { fputs("FAIL: \(message)\n", stderr); exit(1) }
}

private func awaitResult<T>(_ start: (@escaping (T) -> Void) -> Void, deadline: TimeInterval = 3) -> T {
    var result: T?
    start { result = $0 }
    let end = Date().addingTimeInterval(deadline)
    while result == nil && Date() < end { RunLoop.main.run(until: Date().addingTimeInterval(0.005)) }
    require(result != nil, "completion exceeded test deadline")
    return result!
}

private func spin(_ seconds: TimeInterval = 0.05) {
    RunLoop.main.run(until: Date().addingTimeInterval(seconds))
}

private final class MockCancellation: PrinterCommandCancellation {
    var cancelled = false
    func cancel() { cancelled = true }
}

private final class MockExecutor: PrinterCommandExecuting {
    struct Call { let path: String; let args: [String]; let token: MockCancellation }
    var calls = [Call]()
    var replies = [Result<PrinterCommandResult, Error>]()
    var held = [((Result<PrinterCommandResult, Error>) -> Void)]()
    var hold = false
    func execute(path: String, arguments: [String], timeout: TimeInterval, outputLimit: Int,
                 completion: @escaping (Result<PrinterCommandResult, Error>) -> Void) -> PrinterCommandCancellation {
        require(Thread.isMainThread, "setup must invoke executor on main")
        require(timeout == 8 && outputLimit == 65_536, "every command must be bounded")
        if path == PrinterSetup.helperPath {
            require(arguments == ["--secure-queue", PrinterSetup.stateDirectory.path, "enable"] ||
                    arguments == ["--secure-queue", PrinterSetup.stateDirectory.path, "preserve"], "no credentials in helper arguments")
        } else {
            require(Array(arguments.prefix(2)) == ["-h", "/private/var/run/cupsd"], "pin commands to local CUPS")
        }
        let token = MockCancellation()
        calls.append(Call(path: path, args: arguments, token: token))
        if hold { held.append(completion) }
        else {
            require(!replies.isEmpty, "unexpected command")
            completion(replies.removeFirst()) // Deliberately synchronous: setup must remain safe.
        }
        return token
    }
}

private func reply(_ text: String = "", status: Int32 = 0) -> Result<PrinterCommandResult, Error> {
    .success(PrinterCommandResult(status: status, output: Data(text.utf8)))
}
private let matching = "device for HP_LaserJet_1010_USB: ipp://127.0.0.1:8632/ipp/print\n"
private let mismatch = "device for HP_LaserJet_1010_USB: usb://private-device\n"

private func configure(_ mock: MockExecutor, repair: Bool = false) -> Result<Void, Error> {
    let setup = PrinterSetup(executor: mock)
    return awaitResult { callback in
        setup.configure(repair: repair) { result in
            require(Thread.isMainThread, "setup completion must run on main")
            callback(result)
        }
    }
}

private func expectSuccess(_ result: Result<Void, Error>) {
    if case .failure(let error) = result { require(false, "unexpected failure: \(error.localizedDescription)") }
}
private func expectError(_ result: Result<Void, Error>, _ expected: PrinterSetupError) {
    guard case .failure(let error) = result else { require(false, "expected failure"); return }
    require(error.localizedDescription == expected.localizedDescription, "unexpected error category")
    require(!error.localizedDescription.contains("private-device") && !error.localizedDescription.contains("private-job"),
            "errors must not include CUPS output")
}

@main
private struct Tests {
    static func main() {
        require(Thread.isMainThread, "tests must start on main")
        let absent = MockExecutor()
        absent.replies = [reply("device for unrelated: usb://unrelated\n"), reply(), reply()]
        expectSuccess(configure(absent))
        require(absent.calls.count == 3, "missing queue must create exactly once")
        require(absent.calls[0].path == "/usr/bin/lpstat" && absent.calls[0].args.last == "-v", "inspect first")
        let create = absent.calls[1]
        require(create.path == "/usr/sbin/lpadmin", "use system lpadmin")
        require(create.args == ["-h", "/private/var/run/cupsd", "-p", "HP_LaserJet_1010_USB", "-D", "HP LaserJet 1010",
                                "-v", PrinterSetup.deviceURI, "-m", "everywhere", "-o", "printer-is-shared=false",
                                "-o", "printer-error-policy=abort-job", "-o", "cupsSNMPSupplies=false", "-o", "hp1010-auth-id-default=unconfigured",
                                "-o", "\(HP_SETUP_STATE_ATTRIBUTE)=pending"], "exact creation options")

        let emptyScheduler = MockExecutor()
        emptyScheduler.replies = [reply("lpstat: No destinations added.\n", status: 1), reply(), reply()]
        expectSuccess(configure(emptyScheduler))
        require(emptyScheduler.calls.count == 3 && emptyScheduler.calls[1].path == "/usr/sbin/lpadmin",
                "exact empty-scheduler diagnostic must allow first queue creation")
        for (text, status) in [("", Int32(1)), ("lpstat: private-device failure\n", 1),
                               ("lpstat: No destinations added.\n" + matching, 1),
                               ("lpstat: No destinations added.\n", 2),
                               ("lpstat: No destinations added.", 1),
                               ("lpstat: No destinations added.\n\n", 1)] {
            let failedInspection = MockExecutor(); failedInspection.replies = [reply(text, status: status)]
            expectError(configure(failedInspection), .commandFailed)
            require(failedInspection.calls.count == 1, "failed or mixed inspection must never create a queue")
        }
        let failedJobs = MockExecutor()
        failedJobs.replies = [reply(matching), reply("lpstat: No destinations added.\n", status: 1)]
        expectError(configure(failedJobs, repair: true), .commandFailed)
        require(failedJobs.calls.count == 2, "empty-destination exception must not apply to job inspection")
        let failedCreation = MockExecutor()
        failedCreation.replies = [reply(), reply("lpstat: No destinations added.\n", status: 1)]
        expectError(configure(failedCreation), .commandFailed)
        require(failedCreation.calls.count == 2, "empty-destination exception must not apply to lpadmin")

        for listing in [matching, matching.replacingOccurrences(of: "HP_LaserJet_1010_USB", with: "hp_laserjet_1010_usb")] {
            let existing = MockExecutor(); existing.replies = [reply(listing), reply()]
            expectSuccess(configure(existing))
            require(existing.calls.count == 2, "automatic matching installs credentials without resetting preferences or jobs")
        }
        let different = MockExecutor(); different.replies = [reply(mismatch)]
        expectError(configure(different), .differentEndpoint)
        require(different.calls.count == 1, "automatic mismatch must not mutate")

        for listing in [matching, mismatch] {
            let repair = MockExecutor(); repair.replies = [reply(listing), reply(), reply(), reply()]
            expectSuccess(configure(repair, repair: true))
            require(repair.calls.count == 4, "explicit repair must check jobs and reapply configuration")
            require(repair.calls[1].args == ["-h", "/private/var/run/cupsd", "-W", "not-completed", "-o", PrinterSetup.queueName], "inspect pending jobs only for intended queue")
            require(repair.calls[2].path == "/usr/sbin/lpadmin" && !repair.calls[2].args.contains("-E"), "repair must not resume a paused queue")
            require(!repair.calls[2].args.contains(where: { $0.hasPrefix(HP_SETUP_STATE_ATTRIBUTE) }),
                    "repair must retain pending setup intent without marking completed queues pending")
            let jobs = MockExecutor(); jobs.replies = [reply(listing), reply("HP_LaserJet_1010_USB-2 private-job 100\n")]
            expectError(configure(jobs, repair: true), .pendingJobs)
            require(jobs.calls.count == 2, "pending jobs must prevent repair")
        }
        let createRepair = MockExecutor(); createRepair.replies = [reply(), reply(), reply()]
        expectSuccess(configure(createRepair, repair: true))
        require(!createRepair.calls[1].args.contains("-E") && createRepair.calls[2].args.last == "enable", "only secure step may enable absent queue")
        require(absent.calls[2].args.last == "enable", "new queue enabled with credential")
        let secureFailure = MockExecutor(); secureFailure.replies = [reply(), reply(), reply(status: 1)]
        expectError(configure(secureFailure), .commandFailed)
        require(secureFailure.calls.count == 3, "credential install failure must fail setup")
        // Retry/relaunch discovers the persisted queue. The C helper consumes its
        // pending marker even in preserve mode (covered by test_local_security.c).
        for repair in [false, true] {
            let recovery = MockExecutor()
            recovery.replies = [reply(), reply(), reply(status: 1)]
            expectError(configure(recovery), .commandFailed)
            recovery.replies = repair ? [reply(matching), reply(), reply(), reply()] : [reply(matching), reply()]
            expectSuccess(configure(recovery, repair: repair))
            require(recovery.calls.last?.args.last == "preserve", "helper decides recovery from persisted setup intent")
            let markerWrites = recovery.calls.filter { $0.args.contains("\(HP_SETUP_STATE_ATTRIBUTE)=pending") }
            require(markerWrites.count == 1, "only initial creation marks setup pending")
        }
        let migrationBusy = MockExecutor(); migrationBusy.replies = [reply(matching), reply(status: 3)]
        expectError(configure(migrationBusy), .pendingJobs)
        let existingSecureFailure = MockExecutor(); existingSecureFailure.replies = [reply(matching), reply(status: 1)]
        expectError(configure(existingSecureFailure), .commandFailed)
        require(existingSecureFailure.calls[1].args.last == "preserve", "migration cannot resume a paused queue")

        let failureCases: [([Result<PrinterCommandResult, Error>], Bool)] = [
            ([reply("private-device", status: 1)], false),
            ([reply(), reply("private-device", status: 1)], false),
            ([reply(mismatch), reply("private-job", status: 1)], true)
        ]
        for (replies, repair) in failureCases {
            let failure = MockExecutor(); failure.replies = replies
            expectError(configure(failure, repair: repair), .commandFailed)
        }
        for listing in ["unrecognized private-device\n", matching + matching, "device for : private-device\n"] {
            let invalid = MockExecutor(); invalid.replies = [reply(listing)]
            expectError(configure(invalid), .invalidResponse)
            require(invalid.calls.count == 1, "ambiguous inspection must never create")
        }
        let invalidUTF = MockExecutor(); invalidUTF.replies = [.success(PrinterCommandResult(status: 0, output: Data([0xff])))]
        expectError(configure(invalidUTF), .invalidResponse)

        for error in [PrinterSetupError.timedOut, .outputLimit, .couldNotStart] {
            let failure = MockExecutor(); failure.replies = [.failure(error)]
            expectError(configure(failure), error)
        }
        let held = MockExecutor(); held.hold = true
        let setup = PrinterSetup(executor: held)
        var oldCallbacks = 0, newCallbacks = 0
        setup.configure(repair: false) { result in oldCallbacks += 1; expectError(result, .cancelled) }
        setup.configure(repair: false) { result in newCallbacks += 1; expectSuccess(result) }
        require(held.calls[0].token.cancelled, "replacement must cancel owned prior command")
        held.held[0](reply()) // stale absence must never install a printer
        held.held[1](reply(matching))
        spin()
        require(held.calls.count == 3 && held.calls[2].args.last == "preserve", "matching queue requires credential installation")
        held.held[2](reply())
        spin()
        require(oldCallbacks == 1 && newCallbacks == 1 && held.calls.count == 3, "stale completion must not race replacement")
        setup.configure(repair: false) { result in oldCallbacks += 1; expectError(result, .cancelled) }
        setup.cancel(); setup.cancel()
        held.held[3](reply())
        spin()
        require(oldCallbacks == 2 && held.calls.count == 4 && held.calls[3].token.cancelled, "cancel must callback once and suppress late mutation")

        testRealProcesses()
        print("Printer setup tests passed (mock CUPS; controlled local subprocesses only).")
    }

    static func testRealProcesses() {
        let executor = PrinterProcessExecutor()
        let success: Result<PrinterCommandResult, Error> = awaitResult { callback in
            executor.execute(path: "/bin/sh", arguments: ["-c", "printf '%s' \"$LC_ALL\""], timeout: 1, outputLimit: 64, completion: callback)
        }
        guard case .success(let value) = success else { require(false, "controlled command failed"); return }
        require(value.status == 0 && String(data: value.output, encoding: .utf8) == "C", "force C locale and drain output")

        let diagnostic: Result<PrinterCommandResult, Error> = awaitResult { callback in
            executor.execute(path: "/bin/sh", arguments: ["-c", "printf 'lpstat: No destinations added.\\n' >&2; exit 1"],
                             timeout: 1, outputLimit: 64, completion: callback)
        }
        guard case .success(let diagnosticValue) = diagnostic else { require(false, "missing bounded stderr diagnostic"); return }
        require(diagnosticValue.status == 1 && diagnosticValue.output == Data("lpstat: No destinations added.\n".utf8),
                "collect stderr in the same bounded diagnostic pipe")

        let exitFailure: Result<PrinterCommandResult, Error> = awaitResult { callback in
            executor.execute(path: "/bin/sh", arguments: ["-c", "exit 7"], timeout: 1, outputLimit: 64, completion: callback)
        }
        guard case .success(let failedValue) = exitFailure else { require(false, "missing exit status"); return }
        require(failedValue.status == 7, "preserve command exit status")
        let launchFailure: Result<PrinterCommandResult, Error> = awaitResult { callback in
            executor.execute(path: "/nonexistent/hp1010-mock-command", arguments: [], timeout: 1, outputLimit: 64, completion: callback)
        }
        requireFailure(launchFailure, .couldNotStart)
        let overflow: Result<PrinterCommandResult, Error> = awaitResult { callback in
            executor.execute(path: "/bin/sh", arguments: ["-c", "while :; do printf '0123456789abcdef'; done"], timeout: 1, outputLimit: 32, completion: callback)
        }
        requireFailure(overflow, .outputLimit)

        for cancel in [false, true] {
            let pidFile = FileManager.default.temporaryDirectory.appendingPathComponent("hp1010-printer-test-\(UUID().uuidString).pid")
            defer { try? FileManager.default.removeItem(at: pidFile) }
            var callbackCount = 0
            let began = Date()
            let result: Result<PrinterCommandResult, Error> = awaitResult { callback in
                let token = executor.execute(path: "/bin/sh", arguments: ["-c", "echo $$ > \"$1\"; exec /bin/sleep 30", "hp1010-test", pidFile.path],
                                             timeout: cancel ? 2 : 0.1, outputLimit: 64) { result in
                    require(Thread.isMainThread, "executor completion must run on main")
                    callbackCount += 1; callback(result)
                }
                if cancel { DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { token.cancel() } }
            }
            requireFailure(result, cancel ? .cancelled : .timedOut)
            require(Date().timeIntervalSince(began) < 1, "deadline/cancel must return promptly")
            spin(0.1)
            require(callbackCount == 1, "executor callback must fire once")
            guard let raw = try? String(contentsOf: pidFile, encoding: .utf8), let pid = Int32(raw.trimmingCharacters(in: .whitespacesAndNewlines)) else {
                require(false, "controlled process did not report PID"); return
            }
            require(kill(pid, 0) == -1 && errno == ESRCH, "deadline/cancel must kill only its controlled child")
        }
    }

    static func requireFailure(_ result: Result<PrinterCommandResult, Error>, _ expected: PrinterSetupError) {
        guard case .failure(let error) = result else { require(false, "expected executor failure"); return }
        require(error.localizedDescription == expected.localizedDescription, "unexpected executor failure")
    }
}
