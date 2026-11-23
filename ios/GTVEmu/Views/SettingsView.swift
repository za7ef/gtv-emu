// الإعدادات: تفعيل JIT (المدمج بملف الاقتران أو عبر StikDebug) وحالته، وخلف «الخيارات المتقدمة»: الذاكرة والمعالجات ووضع JIT ومصدر الصور والطرفية وإدارة الملفات.
import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {
    @AppStorage("ramMB") private var ramMB = 1024
    @AppStorage("cores") private var cores = 2
    @AppStorage("jitMode") private var jitMode = JITMode.auto.rawValue
    @AppStorage("useProfileDefaults") private var useProfileDefaults = true
    @AppStorage("repo") private var repo = "za7ef/gtv-emu"
    @AppStorage("releaseTag") private var releaseTag = "images-v1"
    @AppStorage("githubToken") private var token = ""
    @AppStorage("manifestURL") private var manifestURL = ""
    @AppStorage(UsageStats.enabledKey) private var usageStats = true
    @ObservedObject private var engine = EmulatorEngine.shared
    @ObservedObject private var jit = JITManager.shared
    @AppStorage(JITManager.methodKey) private var jitMethod = JITManager.Method.builtIn.rawValue
    @AppStorage(JITManager.autoKey) private var jitAuto = true
    @State private var showImporter = false
    @State private var importError: String?

    private var physicalGB: Double { Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824 }

    @Environment(\.dismiss) private var dismiss
    @State private var showAdvanced = false

    private var isWorking: Bool { if case .working = jit.phase { return true }; return false }

    private var pairingText: String {
        guard jit.hasPairing else { return "غير موجود" }
        let kind = jit.pairingKind?.title ?? "غير معروف"
        if let d = jit.pairingDate { return kind + "، " + d.formatted(date: .abbreviated, time: .omitted) }
        return kind
    }

    var body: some View {
        Form {
            Section {
                LabeledContent("حالة JIT") {
                    Text(jit.summary).multilineTextAlignment(.trailing)
                        .foregroundStyle(jit.isReady ? Color.yellow : Color.secondary)
                }
                if !jit.detail.isEmpty {
                    Text(jit.detail).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                }
                Picker("طريقة التفعيل", selection: $jitMethod) {
                    ForEach(JITManager.Method.allCases) { Text($0.title).tag($0.rawValue) }
                }
                if jitMethod == JITManager.Method.builtIn.rawValue {
                    Button { showImporter = true } label: {
                        Label(jit.hasPairing ? "استبدال ملف الاقتران" : "استيراد ملف الاقتران", systemImage: "doc.badge.plus")
                    }
                    Button {
                        do { try jit.importPairingFromPasteboard(); importError = nil }
                        catch { importError = error.localizedDescription }
                    } label: { Label("لصق ملف الاقتران من الحافظة", systemImage: "doc.on.clipboard") }
                    LabeledContent("ملف الاقتران", value: pairingText)
                    if let importError { Text(importError).font(.footnote).foregroundStyle(.red) }
                    Toggle("التفعيل تلقائيًا عند فتح التطبيق", isOn: $jitAuto)
                    if let r = HelperLauncher.unavailableReason { Text(r).font(.footnote).foregroundStyle(.orange) }
                }
                Button { jit.enable() } label: { Label("تفعيل JIT الآن", systemImage: "bolt") }
                    .disabled(jit.isReady || isWorking)
                if jitMethod == JITManager.Method.builtIn.rawValue && jit.hasPairing {
                    Button { jit.checkConnection() } label: { Label("فحص الاتصال وصورة المطوّر", systemImage: "antenna.radiowaves.left.and.right") }
                        .disabled(isWorking)
                }
                NavigationLink { JITGuideView() } label: { Label("طريقة تفعيل JIT خطوة بخطوة", systemImage: "list.number") }
            } header: { Text("السرعة (JIT)") } footer: {
                Text("المترجم الفوري يسرّع المحاكاة كثيرًا. بدونه يعمل التطبيق بالمفسّر، وهو أبطأ لكنه يعمل دائمًا. التفعيل المدمج يحتاج تشغيل LocalDevVPN أو StosVPN وملف اقتران RemotePairing لهذا الجهاز (مثل rp_pairing_file.plist من StikDebug). إن تعذّر اختيار الملف: انسخه إلى «ملفات ← على جهازي ← GTV Emu»، أو شاركه من تطبيق الملفات إلى GTV Emu، فيُستورد تلقائيًا.")
            }
            .sheet(isPresented: $showImporter) {
                PairingPicker(onPick: { url in
                    showImporter = false
                    do { try jit.importPairing(from: url); importError = nil }
                    catch { importError = error.localizedDescription }
                }, onCancel: { showImporter = false })
                .ignoresSafeArea()
            }

            Section {
                Toggle("مشاركة إحصائيات الاستخدام المجهولة", isOn: $usageStats)
                    .onChange(of: usageStats) { _, v in UsageStats.isEnabled = v }
            } header: { Text("الإحصائيات") } footer: {
                Text("تُرسل إشارة مجهولة عند فتح التطبيق لمعرفة عدد المستخدمين، تتضمن رقم الإصدار وإصدار النظام والمنطقة واللغة فقط، مع معرّف عشوائي مشفّر لا يكشف هوية المستخدم. ولا تُرسل أي بيانات شخصية أو ملفات.")
            }

            Section {
                Toggle("إظهار الخيارات المتقدمة", isOn: $showAdvanced.animation())
            }

            if showAdvanced {
            Section {
                Stepper(value: $ramMB, in: 256...6144, step: 256) {
                    LabeledContent("ذاكرة الضيف", value: "\(ramMB) ميغابايت")
                }
                Stepper(value: $cores, in: 1...4) {
                    LabeledContent("عدد المعالجات", value: "\(cores)")
                }
                Toggle("استخدم قيم ملف التعريف إن وُجدت", isOn: $useProfileDefaults)
            } header: { Text("الآلة") } footer: {
                Text(String(format: "ذاكرة الجهاز %.1f غيغابايت. أندرويد يحتاج 768 ميغابايت على الأقل. تُنفَّذ المعالجات الافتراضية بالتناوب على خيط واحد، فزيادتها لا تسرّع المحاكاة.", physicalGB))
            }

            Section {
                Picker("المترجم الفوري (JIT)", selection: $jitMode) {
                    ForEach(JITMode.allCases) { Text($0.title).tag($0.rawValue) }
                }
                LabeledContent("آخر نتيجة", value: engine.jitStatus)
                LabeledContent("TXM", value: jit.txmPresent.map { $0 ? "موجود" : "غير موجود" } ?? "غير معروف")
                if jit.hasPairing {
                    Button("حذف نسخة صورة المطوّر (DDI)") { jit.resetDDI() }
                    Button("حذف ملف الاقتران", role: .destructive) { jit.removePairing() }
                }
            } header: { Text("المترجم الفوري") } footer: {
                Text("يُطلب الآن تجهيز منطقة التنفيذ فور إرفاق المصحّح عند فتح التطبيق، ثم يُفصل المصحّح، فلا يلزم إبقاء StikDebug مفتوحًا حتى تشغيل النظام.")
            }

            Section {
                TextField("المستودع", text: $repo).textInputAutocapitalization(.never).autocorrectionDisabled()
                TextField("وسم الإصدار (أو latest)", text: $releaseTag).textInputAutocapitalization(.never).autocorrectionDisabled()
                SecureField("رمز وصول GitHub (اختياري)", text: $token)
                TextField("رابط manifest.json بديل (اختياري)", text: $manifestURL)
                    .textInputAutocapitalization(.never).autocorrectionDisabled().keyboardType(.URL)
            } header: { Text("مصدر الصور") } footer: {
                Text("تُقرأ قائمة الصور من ملف manifest.json في إصدار المستودع. رمز الوصول مطلوب فقط عند استخدام مستودع خاص.")
            }

            Section("أدوات") {
                NavigationLink { ConsoleView().navigationTitle("الطرفية").navigationBarTitleDisplayMode(.inline) } label: {
                    Label("الطرفية", systemImage: "terminal")
                }
                NavigationLink { ImagesView() } label: {
                    Label("إدارة ملفات الأنظمة", systemImage: "externaldrive")
                }
            }
            }

            Section("حول") {
                NavigationLink {
                    AboutView()
                } label: {
                    LabeledContent("حول التطبيق", value: "\(UpdateChecker.currentVersion) (\(UpdateChecker.currentBuild))")
                }
                LabeledContent("مجلد الصور", value: "ملفات ← على جهازي ← GTV Emu ← images")
            }
        }
        .navigationTitle("الإعدادات")
        .onChange(of: jitMethod) { _, _ in jit.objectWillChange.send() }
        .toolbar { ToolbarItem(placement: .confirmationAction) { Button("تم") { dismiss() } } }
    }
}
