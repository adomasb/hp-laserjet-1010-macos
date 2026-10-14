import Foundation
import Darwin

protocol PrinterConfiguring: AnyObject {
    func configure(repair: Bool, completion: @escaping (Result<Void, Error>) -> Void)
    func cancel()
}
extension PrinterSetup: PrinterConfiguring {}

protocol HelperProcess: AnyObject {
    var isRunning: Bool { get }
    var terminationStatus: Int32 { get }
    var onExit: (() -> Void)? { get set }
    func launch(arguments: [String]) throws
    func terminate()
    func forceStop()
}

private final class NativeHelperProcess: HelperProcess {
    private let process = Process()
    var onExit: (() -> Void)?
    var isRunning: Bool { process.isRunning }
    var terminationStatus: Int32 { process.terminationStatus }

    init(executable: URL) {
        process.executableURL = executable
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        process.terminationHandler = { [weak self] _ in
            DispatchQueue.main.async { self?.onExit?() }
        }
    }
    func launch(arguments: [String]) throws {
        process.arguments = arguments
        try process.run()
    }
    func terminate() { process.terminate() }
    func forceStop() {
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
    }
}

struct HelperState: Decodable {
    let mode: String
    let service: String
    let lastJobID: Int
    let message: String
    enum CodingKeys: String, CodingKey {
        case mode, service, message
        case lastJobID = "last_job_id"
    }

    static func read(directory: URL, since started: Date) -> HelperState? {
        let file = directory.appendingPathComponent("state.json")
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: file.path),
              let modified = attributes[.modificationDate] as? Date, modified >= started,
              let size = attributes[.size] as? NSNumber, size.intValue <= 16384,
              let data = try? Data(contentsOf: file),
              let state = try? JSONDecoder().decode(HelperState.self, from: data),
              state.mode == "usb" else { return nil }
        return state
    }
}

struct PrintingServiceEnvironment {
    let stateDirectory: URL
    let parentPID: Int32
    var makeProcess: () -> HelperProcess
    var prepareStorage: () throws -> Void
    var readState: (Date) -> HelperState?
    var now: () -> Date
    var schedule: (TimeInterval, @escaping () -> Void) -> Void

    static func live() -> PrintingServiceEnvironment {
        let directory = PrinterSetup.stateDirectory
        let executable = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/hp1010-helper")
        return PrintingServiceEnvironment(
            stateDirectory: directory, parentPID: ProcessInfo.processInfo.processIdentifier,
            makeProcess: { NativeHelperProcess(executable: executable) },
            prepareStorage: {
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                       attributes: [.posixPermissions: 0o700])
            },
            readState: { HelperState.read(directory: directory, since: $0) },
            now: Date.init,
            schedule: { delay, work in DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work) })
    }
}

// Main-thread owner of the helper and printer setup. AppKit only renders this
// state and forwards actions; tests inject time, process, storage, and CUPS setup.
final class PrintingService {
    enum SetupState { case waiting, configuring, ready, failed(String) }
    private let environment: PrintingServiceEnvironment
    private let printerSetup: PrinterConfiguring
    private var helper: HelperProcess?
    private var startedAt: Date?
    private var generation = UUID()
    private var quitting = false
    private var shutdownCompletion: (() -> Void)?
    private(set) var setupState = SetupState.waiting
    private(set) var serviceReady = false
    private(set) var stopping = false
    private(set) var startupError: String?
    private(set) var state: HelperState?
    var onChange: (() -> Void)?
    var running: Bool { helper?.isRunning == true }

    init(environment: PrintingServiceEnvironment = .live(), printerSetup: PrinterConfiguring = PrinterSetup()) {
        self.environment = environment
        self.printerSetup = printerSetup
    }

    func start() {
        precondition(Thread.isMainThread)
        guard !running, !stopping, !quitting else { return }
        startupError = nil
        invalidateSetup()
        setupState = .waiting
        state = nil
        do { try environment.prepareStorage() }
        catch {
            startupError = "Could not create private printer storage: \(error.localizedDescription)"
            onChange?()
            return
        }
        let process = environment.makeProcess()
        process.onExit = { [weak self, weak process] in
            guard let self = self, let process = process, self.helper === process else { return }
            if !self.stopping && process.terminationStatus != 0 {
                self.startupError = "The printing service stopped unexpectedly. Another copy of the app may already be running. Quit any other copy and try starting the service again."
            }
            self.invalidateSetup()
            self.stopping = false
            self.helper = nil
            self.state = nil
            let completion = self.shutdownCompletion
            self.shutdownCompletion = nil
            self.onChange?()
            completion?()
        }
        helper = process
        startedAt = environment.now()
        do {
            try process.launch(arguments: ["--usb", "--state-dir", environment.stateDirectory.path,
                                           "--port", String(HP_USB_PORT), "--parent-pid", String(environment.parentPID)])
        } catch {
            helper = nil
            startupError = "Could not start the printing service: \(error.localizedDescription)"
        }
        poll()
    }

    func stop() {
        precondition(Thread.isMainThread)
        guard let process = helper, process.isRunning, !stopping else { return }
        stopping = true
        invalidateSetup()
        setupState = .waiting
        process.terminate()
        environment.schedule(12) { [weak self, weak process] in
            guard let self = self, let process = process, self.helper === process, process.isRunning else { return }
            self.startupError = "The printing service could not stop normally. It was closed; partial output may exist. Check the printer before submitting another job."
            process.forceStop()
            self.onChange?()
        }
        onChange?()
    }

    // Return whether the caller must wait for its owned child to exit.
    func shutdown(completion: @escaping () -> Void) -> Bool {
        precondition(Thread.isMainThread)
        quitting = true
        guard running else { return false }
        shutdownCompletion = completion
        stop()
        return true
    }

    func poll() {
        precondition(Thread.isMainThread)
        state = running ? startedAt.flatMap(environment.readState) : nil
        serviceReady = running && !stopping && state?.service == "running"
        if case .waiting = setupState, running, !stopping, startupError == nil {
            if serviceReady { configure(repair: false); return }
            if let started = startedAt, environment.now().timeIntervalSince(started) > 15 {
                setupState = .failed("The printing service is taking too long to start. Try again or restart the service.")
            }
        }
        onChange?()
    }

    func retrySetup() {
        precondition(Thread.isMainThread)
        if serviceReady { configure(repair: false) }
        else {
            setupState = .failed("Restart the printing service, then try setup again.")
            onChange?()
        }
    }

    func configure(repair: Bool) {
        precondition(Thread.isMainThread)
        guard serviceReady, running, !stopping else { return }
        if case .configuring = setupState { return }
        setupState = .configuring
        let current = generation
        onChange?()
        printerSetup.configure(repair: repair) { [weak self] result in
            guard let self = self, self.generation == current, self.running, !self.stopping else { return }
            switch result {
            case .success: self.setupState = .ready
            case .failure(let error): self.setupState = .failed(error.localizedDescription)
            }
            self.poll()
        }
    }

    private func invalidateSetup() {
        generation = UUID()
        printerSetup.cancel()
        serviceReady = false
    }
}
