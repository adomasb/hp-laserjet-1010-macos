import Foundation
import Darwin

// All public entry points and completions run on the main thread. Each call is
// finite (at most four eight-second commands); no print job is ever submitted.
final class PrinterSetup {
    static let queueName = "HP_LaserJet_1010_USB"
    static let deviceURI = "ipp://127.0.0.1:\(HP_USB_PORT)/ipp/print"
    static let stateDirectory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("LaserJet1010Helper/usb", isDirectory: true)
    static let helperPath = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/hp1010-helper").path
    private let executor: PrinterCommandExecuting
    private var operationID: UUID?
    private var completion: ((Result<Void, Error>) -> Void)?
    private var command: PrinterCommandCancellation?

    init() { executor = PrinterProcessExecutor() }
    init(executor: PrinterCommandExecuting) { self.executor = executor }

    func configure(repair: Bool, completion: @escaping (Result<Void, Error>) -> Void) {
        precondition(Thread.isMainThread)
        cancel()
        let id = UUID()
        operationID = id
        self.completion = completion
        run("/usr/bin/lpstat", ["-h", "/private/var/run/cupsd", "-v"], id: id, emptyDestinationsAllowed: true) { result in
            switch result {
            case .failure(let error): self.finish(id, .failure(error))
            case .success(let output):
                do {
                    let existing = try Self.existingURI(output)
                    if existing == nil { self.install(id: id, existing: false) }
                    else if repair { self.checkJobs(id: id) }
                    else if existing == Self.deviceURI { self.secure(id: id, enable: false) }
                    else { self.finish(id, .failure(PrinterSetupError.differentEndpoint)) }
                } catch { self.finish(id, .failure(error)) }
            }
        }
    }

    func cancel() {
        precondition(Thread.isMainThread)
        guard let id = operationID else { return }
        command?.cancel()
        finish(id, .failure(PrinterSetupError.cancelled))
    }

    private func checkJobs(id: UUID) {
        run("/usr/bin/lpstat", ["-h", "/private/var/run/cupsd", "-W", "not-completed", "-o", Self.queueName], id: id) { result in
            switch result {
            case .failure(let error): self.finish(id, .failure(error))
            case .success(let output):
                guard output.allSatisfy({ $0 == 10 || $0 == 13 || $0 == 32 || $0 == 9 }) else {
                    self.finish(id, .failure(PrinterSetupError.pendingJobs))
                    return
                }
                self.install(id: id, existing: true)
            }
        }
    }

    private func install(id: UUID, existing: Bool) {
        var args = ["-h", "/private/var/run/cupsd", "-p", Self.queueName, "-D", "HP LaserJet 1010",
                    "-v", Self.deviceURI, "-m", "everywhere"]
        // A new queue is enabled only when its private credential is installed.
        args += ["-o", "printer-is-shared=false", "-o", "printer-error-policy=abort-job",
                 "-o", "cupsSNMPSupplies=false", "-o", "hp1010-auth-id-default=unconfigured"]
        // Persist first-install intent in CUPS so retry/relaunch can finish enabling
        // this queue. Repair must preserve this marker and any deliberate pause.
        if !existing { args += ["-o", "\(HP_SETUP_STATE_ATTRIBUTE)=pending"] }
        run("/usr/sbin/lpadmin", args, id: id) { result in
            switch result {
            case .failure(let error): self.finish(id, .failure(error))
            case .success: self.secure(id: id, enable: !existing)
            }
        }
    }

    private func secure(id: UUID, enable: Bool) {
        // Pass only the private directory path. The helper reads the credential
        // and sends it directly to the fixed local CUPS socket, never via argv.
        run(Self.helperPath, ["--secure-queue", Self.stateDirectory.path, enable ? "enable" : "preserve"], id: id) { result in
            self.finish(id, result.map { _ in () })
        }
    }

    private func run(_ path: String, _ arguments: [String], id: UUID, emptyDestinationsAllowed: Bool = false,
                     then: @escaping (Result<Data, Error>) -> Void) {
        guard operationID == id else { return }
        command = executor.execute(path: path, arguments: arguments, timeout: 8, outputLimit: 65_536) { [weak self] result in
            // Also isolate test executors that complete synchronously or off-main.
            DispatchQueue.main.async {
                guard let self = self, self.operationID == id else { return }
                self.command = nil
                switch result {
                case .success(let value) where value.status == 0: then(.success(value.output))
                // CUPS reports an empty scheduler as status 1, rather than an
                // empty success. Accept only this exact C-locale diagnostic,
                // and only for the complete destination listing.
                case .success(let value) where emptyDestinationsAllowed && value.status == 1 &&
                    value.output == Data("lpstat: No destinations added.\n".utf8): then(.success(Data()))
                case .success(let value) where path == Self.helperPath && value.status == 3:
                    then(.failure(PrinterSetupError.pendingJobs))
                case .success: then(.failure(PrinterSetupError.commandFailed))
                case .failure(let error): then(.failure(error))
                }
            }
        }
    }

    private func finish(_ id: UUID, _ result: Result<Void, Error>) {
        guard operationID == id else { return }
        let callback = completion
        operationID = nil
        completion = nil
        command = nil
        DispatchQueue.main.async { callback?(result) }
    }

    private static func existingURI(_ data: Data) throws -> String? {
        guard let output = String(data: data, encoding: .utf8) else { throw PrinterSetupError.invalidResponse }
        var found: String?
        for line in output.components(separatedBy: "\n") where !line.isEmpty {
            guard line.hasPrefix("device for "), let split = line.range(of: ": ") else {
                throw PrinterSetupError.invalidResponse
            }
            let name = String(line[line.index(line.startIndex, offsetBy: 11)..<split.lowerBound])
            let uri = String(line[split.upperBound...])
            guard !name.isEmpty, !uri.isEmpty, !name.contains(where: { $0.isWhitespace }),
                  !uri.contains(where: { $0.isNewline }) else { throw PrinterSetupError.invalidResponse }
            if name.caseInsensitiveCompare(queueName) == .orderedSame {
                guard found == nil else { throw PrinterSetupError.invalidResponse }
                found = uri
            }
        }
        return found
    }
}

enum PrinterSetupError: LocalizedError {
    case differentEndpoint, pendingJobs, commandFailed, invalidResponse
    case couldNotStart, timedOut, cancelled, outputLimit

    var errorDescription: String? {
        switch self {
        case .differentEndpoint:
            return "The HP LaserJet 1010 queue uses a different printer address. Use Repair Printer Setup to reconnect it to this app."
        case .pendingJobs:
            return "Finish or cancel pending HP LaserJet 1010 jobs in the print queue, then use Repair Printer Setup again."
        case .commandFailed:
            return "macOS could not inspect or configure the printer queue. Check printing permissions in System Settings, then try again."
        case .invalidResponse, .outputLimit:
            return "macOS returned an unexpected printer queue response. Check Printers & Scanners in System Settings, then try again."
        case .couldNotStart:
            return "The macOS printer setup command could not start. Try again after restarting the app."
        case .timedOut:
            return "Printer setup took too long and was stopped. Check Printers & Scanners in System Settings, then try again."
        case .cancelled:
            return "Printer setup was cancelled."
        }
    }
}

struct PrinterCommandResult {
    let status: Int32
    let output: Data
}

protocol PrinterCommandCancellation: AnyObject { func cancel() }
protocol PrinterCommandExecuting {
    @discardableResult
    func execute(path: String, arguments: [String], timeout: TimeInterval, outputLimit: Int,
                 completion: @escaping (Result<PrinterCommandResult, Error>) -> Void) -> PrinterCommandCancellation
}

// A serial worker drains a nonblocking pipe as the process runs. Output above
// the limit stops the owned child immediately. Combined diagnostics stay private.
final class PrinterProcessExecutor: PrinterCommandExecuting {
    private let worker = DispatchQueue(label: "dev.local.hp1010.printer-command")

    @discardableResult
    func execute(path: String, arguments: [String], timeout: TimeInterval, outputLimit: Int,
                 completion: @escaping (Result<PrinterCommandResult, Error>) -> Void) -> PrinterCommandCancellation {
        let task = PrinterProcessTask(worker: worker, completion: completion)
        worker.async { task.start(path: path, arguments: arguments, timeout: timeout, outputLimit: outputLimit) }
        return task
    }
}

private final class PrinterProcessTask: PrinterCommandCancellation {
    private let worker: DispatchQueue
    private var completion: ((Result<PrinterCommandResult, Error>) -> Void)?
    private var process: Process?
    private var pipe: Pipe?
    private var reader: DispatchSourceRead?
    private var deadline: DispatchWorkItem?
    private var output = Data()
    private var limit = 0

    init(worker: DispatchQueue, completion: @escaping (Result<PrinterCommandResult, Error>) -> Void) {
        self.worker = worker
        self.completion = completion
    }

    func cancel() { worker.async { self.stop(.cancelled) } }

    func start(path: String, arguments: [String], timeout: TimeInterval, outputLimit: Int) {
        guard completion != nil else { return }
        limit = max(0, outputLimit)
        let child = Process()
        let pipe = Pipe()
        self.pipe = pipe
        process = child
        child.executableURL = URL(fileURLWithPath: path)
        child.arguments = arguments
        // An explicit server argument pins production commands to local CUPS.
        child.environment = ["PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LC_ALL": "C", "LANG": "C"]
        child.standardInput = FileHandle.nullDevice
        child.standardOutput = pipe
        child.standardError = pipe
        let fd = pipe.fileHandleForReading.fileDescriptor
        guard fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != -1 else {
            finish(.failure(PrinterSetupError.couldNotStart)); return
        }
        let reader = DispatchSource.makeReadSource(fileDescriptor: fd, queue: worker)
        self.reader = reader
        reader.setEventHandler { [weak self] in self?.drain() }
        reader.resume()
        // Retain this finite task until exit/deadline even if a caller drops its token.
        child.terminationHandler = { [self] terminated in
            self.worker.async {
                guard self.completion != nil else { return }
                self.drain()
                if self.completion != nil {
                    self.finish(.success(PrinterCommandResult(status: terminated.terminationStatus, output: self.output)))
                }
            }
        }
        do { try child.run() }
        catch { finish(.failure(PrinterSetupError.couldNotStart)); return }
        // The parent must not retain the write end: EOF must follow child exit.
        try? pipe.fileHandleForWriting.close()
        let deadline = DispatchWorkItem { [weak self] in self?.stop(.timedOut) }
        self.deadline = deadline
        worker.asyncAfter(deadline: .now() + max(0, timeout), execute: deadline)
    }

    private func drain() {
        guard completion != nil, let fd = pipe?.fileHandleForReading.fileDescriptor else { return }
        var buffer = [UInt8](repeating: 0, count: 4096)
        while true {
            let count = read(fd, &buffer, buffer.count)
            if count > 0 {
                guard output.count <= limit - count else { stop(.outputLimit); return }
                output.append(contentsOf: buffer.prefix(count))
            } else if count < 0 && errno == EINTR { continue }
            else if count < 0 && errno != EAGAIN && errno != EWOULDBLOCK {
                stop(.invalidResponse); return
            } else { return }
        }
    }

    private func stop(_ error: PrinterSetupError) {
        guard completion != nil else { return }
        if let process = process, process.isRunning { kill(process.processIdentifier, SIGKILL) }
        finish(.failure(error))
    }

    private func finish(_ result: Result<PrinterCommandResult, Error>) {
        guard let callback = completion else { return }
        completion = nil
        deadline?.cancel()
        deadline = nil
        reader?.cancel()
        reader = nil
        process?.terminationHandler = nil
        process = nil
        try? pipe?.fileHandleForReading.close()
        try? pipe?.fileHandleForWriting.close()
        pipe = nil
        DispatchQueue.main.async { callback(result) }
    }
}
