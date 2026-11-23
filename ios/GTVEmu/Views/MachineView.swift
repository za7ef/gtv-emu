// الشاشة الرئيسية: اختيار النظام، وتنزيله بضغطة، ثم التشغيل بزر واحد. أثناء التشغيل تُعرض الشاشة مع الريموت.
import SwiftUI

struct MachineView: View {
    @ObservedObject private var engine = EmulatorEngine.shared
    @ObservedObject private var store = ImageStore.shared
    @ObservedObject private var jit = JITManager.shared
    @AppStorage("ramMB") private var ramMB = 1024
    @AppStorage("cores") private var cores = 2
    @AppStorage("jitMode") private var jitMode = JITMode.auto.rawValue
    @AppStorage("useProfileDefaults") private var useProfileDefaults = true
    @AppStorage("profileID") private var profileID = "alpine-test"
    @State private var showRemote = true
    @State private var showConsoleOverlay = false
    @State private var showSettings = false

    private var profiles: [ImageProfile] { store.manifest?.profiles ?? [] }
    private var profile: ImageProfile? { profiles.first { $0.id == profileID } ?? profiles.first }

    var body: some View {
        Group {
            if engine.isRunning && store.preparingID == nil { running } else { home }
        }
        .sheet(isPresented: $showSettings) {
            NavigationStack { SettingsView() }
                .environment(\.layoutDirection, .rightToLeft)
                .preferredColorScheme(.dark)
        }
        // القائمة تُحدَّث عند كل تشغيل حتى تطابق بصمات الملفات المنشورة حاليًا
        .task { if store.preparingID == nil { await store.refreshManifest() } }
    }

    // MARK: - الشاشة الرئيسية (قبل التشغيل)

    private var home: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack {
                    Text("GTV Emu").font(.largeTitle.bold())
                    Spacer()
                    Button { showSettings = true } label: {
                        Image(systemName: "gearshape.fill").font(.title2)
                    }
                    .accessibilityLabel("الإعدادات")
                }

                Text("اختر النظام").font(.headline).foregroundColor(.secondary)
                if profiles.isEmpty {
                    if let err = store.lastError {
                        Label(err, systemImage: "wifi.exclamationmark").foregroundColor(.orange)
                    } else {
                        ProgressView("جارٍ جلب قائمة الأنظمة...")
                    }
                    Button("إعادة المحاولة") { Task { await store.refreshManifest() } }
                } else {
                    ForEach(profiles) { p in systemCard(p) }
                }

                if let p = profile { mainButton(p) }
                statusLine
                if case .stopped(let why) = engine.state {
                    Text(why).font(.footnote).foregroundColor(.secondary)
                }
            }
            .padding()
        }
        .background(Color.black.ignoresSafeArea())
    }

    private func systemCard(_ p: ImageProfile) -> some View {
        let selected = p.id == profile?.id
        return Button { profileID = p.id } label: {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: p.kind == "android" ? "tv" : "terminal")
                    .font(.title2).frame(width: 36)
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Text(p.name).font(.headline)
                        if p.experimental == true {
                            Text("تجريبي").font(.caption2.bold())
                                .padding(.horizontal, 6).padding(.vertical, 2)
                                .background(Capsule().fill(Color.orange.opacity(0.25)))
                                .foregroundColor(.orange)
                        }
                    }
                    if let n = p.note { Text(n).font(.caption).foregroundColor(p.experimental == true ? .orange : .secondary) }
                    Text(stateText(p)).font(.caption).foregroundColor(.secondary)
                    if store.isDownloading(p) { ProgressView(value: store.overallProgress(p)) }
                    if store.preparingID == p.id { ProgressView(value: store.prepareProgress) }
                    if selected, let n = p.notice, !store.isReady(p) {
                        Label(n, systemImage: "doc.text.magnifyingglass").font(.caption2).foregroundColor(.secondary)
                    }
                }
                Spacer()
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .foregroundColor(selected ? .green : .secondary)
            }
            .padding()
            .background(RoundedRectangle(cornerRadius: 16).fill(Color.white.opacity(selected ? 0.12 : 0.06)))
        }
        .buttonStyle(.plain)
        .disabled(engine.isRunning || store.preparingID != nil)
    }

    private func stateText(_ p: ImageProfile) -> String {
        let size = ByteCountFormatter.string(fromByteCount: p.downloadSize, countStyle: .file)
        if store.isReady(p) { return "جاهز للتشغيل" }
        if store.preparingID == p.id { return (store.prepareStatus ?? "يُجهَّز...") + " (أبقِ التطبيق مفتوحًا)" }
        if store.readyToPrepare(p) {
            if let st = store.prepareStatus, st.hasPrefix("فشل") { return st }
            return "اكتمل التنزيل، بقي تجهيز النظام (عدة دقائق)"
        }
        if store.isDownloading(p) { return String(format: "جاري التنزيل %.0f٪ من %@", store.overallProgress(p) * 100, size) }
        if let f = store.failure(p) { return f + " (اضغط تنزيل للاستئناف)" }
        return "يحتاج تنزيل \(size)"
    }

    @ViewBuilder
    private func mainButton(_ p: ImageProfile) -> some View {
        if store.isReady(p) {
            Button { boot() } label: {
                Label("تشغيل", systemImage: "play.fill").font(.title2.bold()).frame(maxWidth: .infinity).padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent).tint(.green)
        } else if store.preparingID == p.id {
            Label("يُجهَّز النظام...", systemImage: "gearshape.2.fill").frame(maxWidth: .infinity).padding(.vertical, 8)
        } else if store.readyToPrepare(p) {
            Button { store.prepare(p, jitMode: JITMode(rawValue: jitMode) ?? .auto) } label: {
                Label("تجهيز النظام", systemImage: "gearshape.2.fill")
                    .font(.title3.bold()).frame(maxWidth: .infinity).padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent).tint(.orange)
        } else if store.isDownloading(p) {
            Button(role: .cancel) { store.cancelAll() } label: {
                Label("إيقاف التنزيل مؤقتًا", systemImage: "pause.fill").frame(maxWidth: .infinity).padding(.vertical, 8)
            }
            .buttonStyle(.bordered)
        } else {
            Button { store.download(p) } label: {
                Label(p.needsPrepare ? "تنزيل نظام Android TV (تجريبي)" : (p.kind == "android" ? "تنزيل نظام Android TV" : "تنزيل \(p.name)"), systemImage: "arrow.down.circle.fill")
                    .font(.title3.bold()).frame(maxWidth: .infinity).padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
        }
    }

    /// سطر حالة واضح للمترجم الفوري
    private var statusLine: some View {
        let on = jit.isReady || engine.jitStatus.hasPrefix("مفعّل")
        var working = false
        if case .working = jit.phase { working = true }
        return VStack(spacing: 6) {
            Label(on ? "JIT مفعّل" : "JIT غير مفعّل: " + jit.summary,
                  systemImage: on ? "bolt.fill" : (working ? "hourglass" : "tortoise.fill"))
                .font(.footnote)
                .foregroundColor(on ? .yellow : .secondary)
            if !on && !working {
                Button("تفعيل JIT") { jit.enable() }
                    .font(.footnote.bold())
                    .buttonStyle(.bordered)
            }
        }
    }

    // MARK: - أثناء التشغيل

    private var running: some View {
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
        .safeAreaInset(edge: .top) { runControls }
    }

    private var screen: some View {
        ZStack(alignment: .bottomLeading) {
            DisplayView()
            if showConsoleOverlay { ConsoleTail().allowsHitTesting(false) }
        }
    }

    private var runControls: some View {
        HStack(spacing: 14) {
            Text(profile?.name ?? "").font(.subheadline.bold()).lineLimit(1)
            Spacer()
            Image(systemName: engine.jitActive ? "bolt.fill" : "tortoise.fill")
                .foregroundColor(engine.jitActive ? .yellow : .secondary)
            Button { showRemote.toggle() } label: { Image(systemName: "appletvremote.gen4") }
            Menu {
                Toggle(isOn: $showConsoleOverlay) { Label("إظهار سجل الإقلاع", systemImage: "terminal") }
                Text(String(format: "السرعة: %.0f مليون تعليمة/ث", engine.mips))
            } label: { Image(systemName: "ellipsis.circle") }
            Button(role: .destructive) { engine.stop() } label: { Image(systemName: "stop.fill") }
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
