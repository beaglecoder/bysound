import SwiftUI
import BySound

struct Received: Identifiable {
    let id = UUID()
    let text: String
    let band: Band
    let corrected: Int
}

/// Send text as sound and listen for it: text field, band picker, Send/Stop, Listen/Stop, level bar, received list.
final class DemoModel: ObservableObject {
    @Published var text = "Hello 🙂"
    @Published var band = Band.top
    @Published var sending = false
    @Published var listening = false
    @Published var level: Float = 0
    @Published var received: [Received] = []
    @Published var error: String?

    private let sender = SoundSender()
    private let listener = SoundListener()

    init() {
        listener.onLevel = { [weak self] rms in self?.level = min(rms * 4, 1) }
        listener.onFrame = { [weak self] h in
            self?.received.insert(Received(text: h.text, band: h.band, corrected: h.correctedBytes), at: 0)
        }
    }

    func toggleSend() {
        if sending {
            sender.stop()
            sending = false
        } else {
            do {
                try sender.start(text: text, band: band)
                sending = true
            } catch { self.error = "\(error)" }
        }
    }

    func toggleListen() {
        if listening {
            listener.stop()
            listening = false
            level = 0
            return
        }
        SoundListener.requestPermission { [self] granted in
            guard granted else { error = "Microphone access denied"; return }
            do {
                try listener.start()
                listening = true
            } catch { self.error = "\(error)" }
        }
    }
}

struct ContentView: View {
    @StateObject private var model = DemoModel()

    var body: some View {
        NavigationView {
            VStack(spacing: 12) {
                TextField("Text to send", text: $model.text).textFieldStyle(.roundedBorder)
                Picker("Band", selection: $model.band) {
                    ForEach(Band.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.menu)
                HStack {
                    Button(model.sending ? "Stop" : "Send") { model.toggleSend() }
                    Spacer()
                    Button(model.listening ? "Stop listening" : "Listen") { model.toggleListen() }
                }
                .buttonStyle(.borderedProminent)
                ProgressView(value: Double(model.level)).progressViewStyle(.linear)
                if let error = model.error { Text(error).font(.footnote).foregroundColor(.red) }
                List(model.received) { r in
                    VStack(alignment: .leading) {
                        Text(r.text)
                        Text("\(r.band.rawValue), \(r.corrected) bytes corrected").font(.caption).foregroundColor(.secondary)
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
