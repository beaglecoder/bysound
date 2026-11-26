import SwiftUI
import BySound

struct Row: Identifiable {
    let id = UUID()
    let text: String
    let detail: String
}

/// "Auto" runs a session (pre-listen, channel ladder, ack, confirm gate); a band plays that band in a loop, like before.
enum BandChoice: Hashable {
    case auto
    case band(Band)
    var label: String { if case .band(let b) = self { return b.rawValue } else { return "auto" } }
}

/// Send text as sound and listen for it: text field, band picker, Send/Stop, Listen/Stop, level bar, received list.
final class DemoModel: ObservableObject {
    @Published var text = "Hello 🙂"
    @Published var choice = BandChoice.auto
    @Published var sending = false
    @Published var listening = false
    @Published var level: Float = 0
    @Published var received: [Row] = []
    @Published var error: String?
    /// Sender: "✓ Received on <band> by the other device".
    @Published var sendStatus: String?
    /// Receiver: "⚠ Two senders at once" or the text of the alert.
    @Published var alert: String?

    private let sender = SoundSender()
    private let listener = SoundListener()
    private var sendSession: SendSession?
    private var receiveTask: Task<Void, Never>?
    private var receiveSession: ReceiveSession?

    init() {
        listener.onLevel = { [weak self] rms in self?.level = min(rms * 4, 1) }
        listener.onFrame = { [weak self] h in
            self?.received.insert(Row(text: h.text, detail: "\(h.band.rawValue), \(h.correctedBytes) bytes corrected"), at: 0)
        }
    }

    func toggleSend() {
        if sending {
            sender.stop()
            sendSession?.stop()
            sending = false
            return
        }
        error = nil
        sendStatus = nil
        guard case .band(let band) = choice else { return sendWithSession() }
        do {
            try sender.start(text: text, band: band)
            sending = true
        } catch { self.error = "\(error)" }
    }

    private func sendWithSession() {
        let text = text
        sending = true
        Task { @MainActor in
            do {
                let session = try await BySoundSession.startSending(text: text)
                sendSession = session
                sendStatus = "Listening, then sending…"
                let d = try await session.delivered
                sendStatus = "✓ Received on \(d.band.rawValue) by the other device"
            } catch let e as SessionError {
                sendStatus = Self.describe(e)
            } catch { self.error = "\(error)" }
            sending = false
            sendSession = nil
        }
    }

    func toggleListen() {
        if listening {
            receiveSession?.cancel()
            receiveTask?.cancel()
            listener.stop()
            listening = false
            level = 0
            return
        }
        error = nil
        alert = nil
        if choice == .auto { return listenWithSession() }
        SoundListener.requestPermission { [self] granted in
            guard granted else { error = "Microphone access denied"; return }
            do {
                try listener.start()
                listening = true
            } catch { self.error = "\(error)" }
        }
    }

    private func listenWithSession() {
        listening = true
        receiveTask = Task { @MainActor in
            do {
                var options = ReceiverOptions()
                options.onAlert = { [weak self] a in self?.alert = "⚠ Risk signal \(a.direction == .heard ? "heard" : "sent"): \(a.reason)" }
                let session = try await BySoundSession.startReceiving(options: options) { [weak self] rms in self?.level = min(rms * 4, 1) }
                receiveSession = session
                let r = try await session.result
                let c = try await session.confirmed() // before acting on the data
                received.insert(Row(text: r.text, detail: "\(r.band.rawValue), confirmed (\(c.watch.rawValue))"), at: 0)
            } catch let e as SessionError {
                alert = Self.describe(e)
            } catch { self.error = "\(error)" }
            listening = false
            level = 0
            receiveSession = nil
        }
    }

    private static func describe(_ e: SessionError) -> String {
        switch e.code {
        case .multipleTransmissions: return "⚠ Two senders at once"
        case .otherTransmission, .otherEmitter, .clonedTransmission: return "⚠ Another transmission is sounding"
        case .riskSignal: return "⚠ A nearby device raised an alert"
        case .timeout: return "No answer"
        case .stopped, .cancelled: return "Stopped"
        default: return "\(e)"
        }
    }
}

struct ContentView: View {
    @StateObject private var model = DemoModel()
    private let choices: [BandChoice] = [.auto] + Band.allCases.map { .band($0) }

    var body: some View {
        NavigationView {
            VStack(spacing: 12) {
                TextField("Text to send", text: $model.text).textFieldStyle(.roundedBorder)
                Picker("Band", selection: $model.choice) {
                    ForEach(choices, id: \.self) { Text($0.label).tag($0) }
                }
                .pickerStyle(.menu)
                HStack {
                    Button(model.sending ? "Stop" : "Send") { model.toggleSend() }
                    Spacer()
                    Button(model.listening ? "Stop listening" : "Listen") { model.toggleListen() }
                }
                .buttonStyle(.borderedProminent)
                ProgressView(value: Double(model.level)).progressViewStyle(.linear)
                if let status = model.sendStatus { Text(status).font(.footnote) }
                if let alert = model.alert { Text(alert).font(.footnote).foregroundColor(.orange) }
                if let error = model.error { Text(error).font(.footnote).foregroundColor(.red) }
                List(model.received) { r in
                    VStack(alignment: .leading) {
                        Text(r.text)
                        Text(r.detail).font(.caption).foregroundColor(.secondary)
                    }
                }
                .listStyle(.plain)
            }
            .padding()
            .navigationTitle("BySound")
        }
        .navigationViewStyle(.stack)
    }
}
