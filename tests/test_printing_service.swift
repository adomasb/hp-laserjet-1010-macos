import Foundation

private func require(_ condition: @autoclosure () -> Bool, _ message: String) {
    if !condition() { fatalError(message) }
}
private struct TestFailure: Error {}

private final class FakeProcess: HelperProcess {
    var isRunning = false
    var terminationStatus: Int32 = 0
    var onExit: (() -> Void)?
    var arguments = [String]()
    var failLaunch = false
    var terminations = 0
    var kills = 0
    func launch(arguments: [String]) throws {
        if failLaunch { throw TestFailure() }
        self.arguments = arguments
        isRunning = true
    }
    func terminate() { terminations += 1 }
    func forceStop() { kills += 1 }
    func exit(_ status: Int32 = 0) {
        isRunning = false
        terminationStatus = status
        onExit?()
    }
}
private final class FakeSetup: PrinterConfiguring {
    var calls = [Bool]()
    var callbacks = [(Result<Void, Error>) -> Void]()
    var cancellations = 0
    func configure(repair: Bool, completion: @escaping (Result<Void, Error>) -> Void) {
        calls.append(repair)
        callbacks.append(completion)
    }
    func cancel() { cancellations += 1 }
}
private final class Harness {
    let setup = FakeSetup()
    var processes = [FakeProcess]()
    var now = Date(timeIntervalSince1970: 100)
    var state: HelperState?
    var storageFails = false
    var launchFails = false
    var scheduled = [(TimeInterval, () -> Void)]()
    lazy var service = PrintingService(environment: PrintingServiceEnvironment(
        stateDirectory: URL(fileURLWithPath: "/tmp/hp1010-test-state"), parentPID: 123,
        makeProcess: {
            let child = FakeProcess()
            child.failLaunch = self.launchFails
            self.processes.append(child)
            return child
        },
        prepareStorage: { if self.storageFails { throw TestFailure() } },
        readState: { _ in self.state }, now: { self.now },
        schedule: { self.scheduled.append(($0, $1)) }), printerSetup: setup)
    func ready() {
        state = HelperState(mode: "usb", service: "running", lastJobID: 0, message: "Ready")
        service.poll()
    }
}

@main
private enum ServiceTests {
    static func main() throws {
        require(Thread.isMainThread, "main-thread tests")
        do {
            let h = Harness()
            h.service.start(); h.service.start()
            require(h.processes.count == 1 && h.service.running, "start is idempotent")
            require(h.processes[0].arguments == ["--usb", "--state-dir", "/tmp/hp1010-test-state",
                                                "--port", String(HP_USB_PORT), "--parent-pid", "123"], "owned helper arguments")
            require(h.setup.calls.isEmpty, "setup waits for readiness")
            h.ready(); h.service.poll()
            require(h.setup.calls == [false], "ready triggers setup once")
            h.setup.callbacks[0](.success(()))
            guard case .ready = h.service.setupState else { fatalError("successful setup") }
            h.service.configure(repair: true)
            require(h.setup.calls == [false, true], "explicit repair")
            h.setup.callbacks[1](.failure(TestFailure()))
            guard case .failed = h.service.setupState else { fatalError("failed repair") }
            h.service.poll()
            require(h.setup.calls.count == 2, "failed setup is not automatically retried")
            h.service.retrySetup()
            require(h.setup.calls == [false, true, false], "explicit retry")
        }
        do {
            let h = Harness(); h.service.start(); h.ready()
            let stale = h.setup.callbacks[0]
            h.service.stop(); h.service.stop()
            require(h.processes[0].terminations == 1 && h.scheduled.count == 1, "one stop/deadline")
            require(h.scheduled[0].0 == 12, "bounded shutdown")
            stale(.success(()))
            guard case .waiting = h.service.setupState else { fatalError("late setup ignored during stop") }
            h.service.start()
            require(h.processes.count == 1, "no restart while stopping")
            let lateExit = h.processes[0].onExit
            h.processes[0].exit()
            h.state = nil; h.service.start()
            stale(.success(())); lateExit?(); h.scheduled[0].1()
            require(h.service.running && h.processes[1].kills == 0, "old callbacks cannot stop replacement")
            guard case .waiting = h.service.setupState else { fatalError("old setup cannot update replacement") }
        }
        do {
            let h = Harness(); h.service.start()
            h.now = h.now.addingTimeInterval(16); h.service.poll()
            guard case .failed = h.service.setupState else { fatalError("readiness timeout") }
            h.ready()
            require(h.setup.calls.isEmpty, "late readiness requires explicit retry")
            h.service.retrySetup()
            require(h.setup.calls == [false], "retry after late readiness")
        }
        do {
            let h = Harness(); h.storageFails = true; h.service.start()
            require(!h.service.running && h.processes.isEmpty && h.service.startupError != nil, "storage failure")
            h.storageFails = false; h.launchFails = true; h.service.start()
            require(!h.service.running && h.service.startupError != nil, "launch failure")
            h.launchFails = false; h.service.start(); h.processes.last!.exit(1)
            require(!h.service.running && h.service.startupError?.contains("unexpectedly") == true, "unexpected exit")
        }
        do {
            let h = Harness(); h.service.start(); h.ready()
            var replies = 0
            require(h.service.shutdown { replies += 1 }, "quit waits for child")
            require(h.setup.cancellations >= 2, "quit cancels setup")
            h.scheduled[0].1()
            require(h.processes[0].kills == 1 && replies == 0, "force only owned child; await exit")
            require(h.service.startupError?.contains("partial output") == true, "forced-stop diagnostic")
            h.processes[0].exit(9); h.processes[0].onExit?()
            require(replies == 1 && !h.service.running, "exactly one quit completion")
            h.service.start()
            require(h.processes.count == 1, "cannot restart after quit")
        }
        do {
            let h = Harness()
            require(!h.service.shutdown {}, "stopped service quits immediately")
            let h2 = Harness(); h2.service.start(); h2.service.stop()
            var replied = false
            require(h2.service.shutdown { replied = true }, "quit during stop waits")
            require(h2.scheduled.count == 1, "quit cannot extend existing stop deadline")
            h2.processes[0].exit()
            require(replied, "quit during stop completes")
        }
        try stateFiles()
        print("PASS printing lifecycle: readiness, retry, stale callbacks, restart, failures, bounded stop and quit; no app or printer launched")
    }

    static func stateFiles() throws {
        let files = FileManager.default
        let directory = files.temporaryDirectory.appendingPathComponent("hp1010-state-test-\(UUID().uuidString)")
        try files.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? files.removeItem(at: directory) }
        let file = directory.appendingPathComponent("state.json")
        let valid = Data("{\"mode\":\"usb\",\"service\":\"running\",\"last_job_id\":2,\"message\":\"Job sent\"}".utf8)
        let past = Date(timeIntervalSince1970: 1)
        require(HelperState.read(directory: directory, since: past) == nil, "missing state")
        try valid.write(to: file)
        require(HelperState.read(directory: directory, since: past)?.lastJobID == 2, "valid state")
        require(HelperState.read(directory: directory, since: Date.distantFuture) == nil, "stale state")
        try Data(repeating: 32, count: 16385).write(to: file)
        require(HelperState.read(directory: directory, since: past) == nil, "oversized state")
        try Data("not-json".utf8).write(to: file)
        require(HelperState.read(directory: directory, since: past) == nil, "malformed state")
        try Data(String(decoding: valid, as: UTF8.self).replacingOccurrences(of: "usb", with: "simulation").utf8).write(to: file)
        require(HelperState.read(directory: directory, since: past) == nil, "wrong mode")
    }
}
