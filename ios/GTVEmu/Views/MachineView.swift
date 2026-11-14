// شاشة الآلة: اختيار ملف التعريف، والتشغيل والإيقاف، وعرض الشاشة مع الريموت وشريط الحالة.
import SwiftUI

struct MachineView: View {
    @ObservedObject private var engine = EmulatorEngine.shared
    @ObservedObject private var store = ImageStore.shared
    @AppStorage("ramMB") private var ramMB = 1024
    @AppStorage("cores") private var cores = 2
    @AppStorage("jitMode") private var jitMode = JITMode.auto.rawValue
    @AppStorage("useProfileDefaults") private var useProfileDefaults = true
    @AppStorage("profileID") private var profileID = "alpine-test"
    @State private var showRemote = true
    @State private var showConsoleOverlay = false

    private var profiles: [ImageProfile] { store.manifest?.profiles ?? [] }
    private var profile: ImageProfile? { profiles.first { $0.id == profileID } ?? profiles.first }

    var body: some View {
        GeometryReader { geo in
            let landscape = geo.size.width > geo.size.height
            ZStack {
                Color.black.ignoresSafeArea()
                if landscape {
                    HStack(spacing: 0) {
                        screen
                        if showRemote { RemoteView(compact: true).padding(8) }
                    }
                } else {
                    VStack(spacing: 12) {
                        screen.aspectRatio(16.0 / 9.0, contentMode: .fit)
                        if showRemote { RemoteView() }
                        Spacer(minLength: 0)
                    }
                }
            }
        }
        .safeAreaInset(edge: .top) { controls }
    }

    private var screen: some View {
        ZStack(alignment: .bottomLeading) {
            DisplayView()
            if showConsoleOverlay {
                ConsoleTail().allowsHitTesting(false)
            }
            if engine.state == .idle {
                Text(profile.map { store.isReady($0) ? "اضغط تشغيل لبدء \($0.name)" : "نزّل صور \($0.name) من تبويب الصور أولًا" } ?? "لا توجد ملفات تعريف")
                    .foregroundColor(.white.opacity(0.7)).padding()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }

    private var controls: some View {
        HStack(spacing: 10) {
            Menu {
                ForEach(profiles) { p in
                    Button { profileID = p.id } label: {
                        Label(p.name, systemImage: store.isReady(p) ? "checkmark.circle" : "arrow.down.circle")
                    }
                }
            } label: {
                Label(profile?.name ?? "ملف التعريف", systemImage: "shippingbox").lineLimit(1)
            }
            .disabled(engine.isRunning)
            Spacer()
            if engine.isRunning {
                Text(String(format: "%.0f م ت/ث", engine.mips)).font(.caption.monospacedDigit())
                Image(systemName: engine.jitActive ? "bolt.fill" : "tortoise.fill")
                    .foregroundColor(engine.jitActive ? .yellow : .secondary)
            }
            Button { showConsoleOverlay.toggle() } label: { Image(systemName: "terminal") }
            Button { showRemote.toggle() } label: { Image(systemName: "appletvremote.gen4") }
            if engine.isRunning {
                Button(role: .destructive) { engine.stop() } label: { Image(systemName: "stop.fill") }
            } else {
                Button { boot() } label: { Image(systemName: "play.fill") }
                    .disabled(profile.map { !store.isReady($0) } ?? true)
            }
        }
        .padding(.horizontal).padding(.vertical, 8)
        .background(.bar)
    }

    private func boot() {
        guard let p = profile else { return }
        let ram = useProfileDefaults ? (p.ramMB ?? ramMB) : ramMB
        let ncpu = useProfileDefaults ? (p.cores ?? cores) : cores
        var r = BootRequest(ramMB: ram, cores: min(4, max(1, ncpu)), jitMode: JITMode(rawValue: jitMode) ?? .auto)
        r.cmdline = p.cmdline
        r.disk = store.url(p, role: "disk")
        if p.kind == "android" {
            r.bootImage = store.url(p, role: "boot")
            r.vendorBoot = store.url(p, role: "vendor_boot")
            r.initBoot = store.url(p, role: "init_boot")
            r.androidboot = p.androidboot ?? []
        } else {
            r.kernel = store.url(p, role: "kernel")
            r.initrd = store.url(p, role: "initrd")
        }
        engine.start(r)
    }
}

/// آخر أسطر الطرفية فوق الشاشة
private struct ConsoleTail: View {
    @ObservedObject var console = EmulatorEngine.shared.console
    var body: some View {
        Text(console.text.split(separator: "\n", omittingEmptySubsequences: false).suffix(14).joined(separator: "\n"))
            .font(.system(size: 10, design: .monospaced))
            .foregroundColor(.green)
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.black.opacity(0.6))
            .environment(\.layoutDirection, .leftToRight)
    }
}
