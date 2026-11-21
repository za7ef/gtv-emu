// الإعدادات: الذاكرة وعدد المعالجات ووضع المترجم الفوري ومصدر الصور.
import SwiftUI

struct SettingsView: View {
    @AppStorage("ramMB") private var ramMB = 1024
    @AppStorage("cores") private var cores = 2
    @AppStorage("jitMode") private var jitMode = JITMode.auto.rawValue
    @AppStorage("useProfileDefaults") private var useProfileDefaults = true
    @AppStorage("repo") private var repo = "za7ef/gtv-emu"
    @AppStorage("releaseTag") private var releaseTag = "images-v1"
    @AppStorage("githubToken") private var token = ""
    @AppStorage("manifestURL") private var manifestURL = ""
    @ObservedObject private var engine = EmulatorEngine.shared

    private var physicalGB: Double { Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824 }

    var body: some View {
        Form {
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
            } header: { Text("المترجم الفوري") } footer: {
                Text("لتفعيل JIT على iOS 26 وما بعده: شغّل التطبيق من StikDebug مع سكربت universal.js. بدون مصحّح مرفق يعمل المفسّر وحده، وهو أبطأ بكثير لكنه يعمل دائمًا.")
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
    }
}
