import SwiftUI
import BySound

/// Accepts only http(s) URLs with a host. Sound is not authenticated: anyone in earshot can send anything,
/// so the receiver never opens what it heard without checking it and showing it to the person first.
func openableURL(_ text: String) -> URL? {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let url = URL(string: t), let scheme = url.scheme?.lowercased(),
          scheme == "http" || scheme == "https",
          let host = url.host, !host.isEmpty else { return nil }
    return url
}

struct ContentView: View {
    @State private var link = "https://example.com/hello"
    @State private var status = ""
    @State private var sending: SendSession?
    @State private var receiving: ReceiveSession?
    @State private var heard: (text: String, band: Band)?
    @Environment(\.openURL) private var openURL

    var body: some View {
        NavigationView {
            Form {
                Section("Send") {
                    TextField("https://...", text: $link)
                        .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button(sending == nil ? "Send by sound" : "Stop") { sending == nil ? send() : stopSending() }
                        .disabled(sending == nil && openableURL(link) == nil)
                }
                Section("Receive") {
                    Button(receiving == nil ? "Listen" : "Cancel") { receiving == nil ? listen() : cancelListening() }
                }
                if let heard {
                    Section("Heard on \(heard.band.rawValue)") {
                        Text(heard.text).font(.callout.monospaced()).textSelection(.enabled)
                        if let url = openableURL(heard.text) {
                            Text("Host: \(url.host ?? "")").bold()
                            Button("Open") { openURL(url) }
                        } else {
                            Text("Not an http(s) link: not offering to open it.").foregroundColor(.red)
                        }
                    }
                }
                if !status.isEmpty { Section { Text(status).font(.footnote) } }
            }
            .navigationTitle("Share Link")
        }
        .navigationViewStyle(.stack)
    }

    private func send() {
        guard openableURL(link) != nil else { return }
        let text = link.trimmingCharacters(in: .whitespacesAndNewlines)
        status = "Sending..."
        Task {
            do {
                let s = try await BySoundSession.startSending(text: text)   // band automatic: the ladder picks it
                sending = s
                let d = try await s.delivered
                status = "Delivered on \(d.band.rawValue) in \(Int(d.seconds)) s"
            } catch {
                status = "Send failed: \(error)"
            }
            sending = nil
        }
    }

    private func stopSending() { sending?.stop() }

    private func listen() {
        heard = nil
        status = "Listening..."
        Task {
            do {
                let r = try await BySoundSession.startReceiving()
                receiving = r
                let got = try await r.result
                _ = try await r.confirmed()          // the confirm gate: throws if another emitter or a risk frame appeared
                heard = (got.text, got.band)
                status = "Received. Check the link before opening it."
            } catch {
                status = "Receive failed: \(error)"
            }
            receiving = nil
        }
    }

    private func cancelListening() { receiving?.cancel() }
}
