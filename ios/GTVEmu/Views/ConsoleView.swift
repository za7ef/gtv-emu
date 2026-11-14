// عرض الطرفية: مخرجات المنفذ التسلسلي ورسائل المحاكي، مع سطر إدخال يُرسل إلى صدفة الضيف.
import SwiftUI

struct ConsoleView: View {
    @ObservedObject var console = EmulatorEngine.shared.console
    @State private var line = ""
    @State private var follow = true
    @FocusState private var focused: Bool

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    Text(console.text.isEmpty ? "لا مخرجات بعد." : console.text)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.green)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                        .padding(8)
                        .environment(\.layoutDirection, .leftToRight)
                    Color.clear.frame(height: 1).id("end")
                }
                .background(Color.black)
                .onChange(of: console.text) { _ in if follow { proxy.scrollTo("end", anchor: .bottom) } }
            }
            HStack(spacing: 8) {
                TextField("أمر للصدفة", text: $line)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .font(.system(.body, design: .monospaced))
                    .environment(\.layoutDirection, .leftToRight)
                    .focused($focused)
                    .submitLabel(.send)
                    .onSubmit(send)
                Button("Ctrl-C") { EmulatorEngine.shared.sendConsoleBytes([3]) }
                    .buttonStyle(.bordered)
                Button { send() } label: { Image(systemName: "return") }
                    .buttonStyle(.borderedProminent)
            }
            .padding(8)
            .background(.bar)
        }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Toggle(isOn: $follow) { Image(systemName: "arrow.down.to.line") }
                ShareLink(item: console.fullLog) { Image(systemName: "square.and.arrow.up") }
                Button { console.clear() } label: { Image(systemName: "trash") }
            }
        }
    }

    private func send() {
        EmulatorEngine.shared.sendConsole(line + "\n")
        line = ""
        focused = true
    }
}
