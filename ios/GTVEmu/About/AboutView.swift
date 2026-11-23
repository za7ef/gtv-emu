// شاشة «حول التطبيق»: الإصدار ورقم البناء والمطوّر والسياسات والشكر والتراخيص.
import SwiftUI

struct AboutView: View {
    @ObservedObject private var updater = UpdateChecker.shared
    @State private var checking = false
    @State private var upToDate = false

    var body: some View {
        Form {
            Section {
                VStack(spacing: 8) {
                    Image(systemName: "tv").font(.system(size: 44)).foregroundStyle(.tint)
                    Text("GTV Emu").font(.title2.bold())
                    Text("محاكي لمعمارية AArch64 لتشغيل الأنظمة على آيفون")
                        .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
            }

            Section("الإصدار") {
                LabeledContent("رقم الإصدار", value: UpdateChecker.currentVersion)
                LabeledContent("رقم البناء", value: "\(UpdateChecker.currentBuild)")
                Button {
                    Task {
                        checking = true
                        upToDate = false
                        UserDefaults.standard.removeObject(forKey: "skippedUpdateTag")
                        await updater.check()
                        checking = false
                        upToDate = updater.available == nil
                    }
                } label: {
                    HStack {
                        Text("التحقق من وجود تحديث")
                        Spacer()
                        if checking { ProgressView() }
                        else if upToDate { Text("الإصدار الحالي هو الأحدث").font(.footnote).foregroundStyle(.secondary) }
                    }
                }
                .disabled(checking)
            }

            Section("المطوّر") {
                Link(destination: URL(string: "https://github.com/za7ef")!) {
                    LabeledContent("GitHub", value: "za7ef")
                }
            }

            Section("السياسات") {
                NavigationLink("سياسة الاستخدام") { PolicyView(policy: .terms) }
                NavigationLink("سياسة الخصوصية") { PolicyView(policy: .privacy) }
            }

            Section {
                ForEach(Credit.all) { c in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(c.name).font(.subheadline.bold())
                        Text(c.detail).font(.footnote).foregroundStyle(.secondary)
                    }
                }
            } header: { Text("الشكر والتراخيص") } footer: {
                Text("لا يتضمن التطبيق أي نظام تشغيل؛ تُنزَّل ملفات الأنظمة بشكل منفصل وتخضع لتراخيص أصحابها.")
            }
        }
        .navigationTitle("حول التطبيق")
    }
}

struct Credit: Identifiable {
    let id = UUID()
    let name: String
    let detail: String

    static let all: [Credit] = [
        Credit(name: "Alpine Linux", detail: "نواة linux-virt وملف initramfs المستخدمان في ملف التعريف التجريبي. النواة مرخّصة بـ GPL-2.0، وحزم Alpine بتراخيص أصحابها."),
        Credit(name: "Android Open Source Project و GKI", detail: "نواة Android العامة وصور الإقلاع من Google، مرخّصة بـ GPL-2.0 للنواة و Apache-2.0 لبقية المشروع."),
        Credit(name: "StikJIT", detail: "إطار التفعيل المدمج للمترجم الفوري، ويتضمن سكربت universal.js. مرخّص بـ MPL-2.0، ومصدره في مستودع StikDebug/StikJIT. عُدّل ملف واجهته البرمجية فقط ليُقرأ بإصدار أقدم من Xcode."),
        Credit(name: "idevice", detail: "مكتبة الاتصال بخدمات الجهاز، المضمّنة في StikJIT، من تطوير Jackson Coxson، ومرخّصة بـ MIT."),
        Credit(name: "StikDebug", detail: "الطريقة البديلة لتفعيل المترجم الفوري عبر رابط stikdebug://. بروتوكول نقاط التوقف في التطبيق مبنيّ وفق سكربت universal.js (رخصة AGPL-3.0)، دون تضمين شيء من شيفرته."),
        Credit(name: "Linux virtio", detail: "مواصفات أجهزة virtio من OASIS، المستخدمة في محاكاة القرص والشبكة والرسوميات والإدخال والصوت."),
    ]
}
