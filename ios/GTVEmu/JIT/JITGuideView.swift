// دليل تفعيل JIT خطوة بخطوة، ويظهر أيضًا عند أول فتح للتطبيق.
import SwiftUI

struct JITGuideView: View {
    var body: some View {
        List {
            Section {
                step(1, "ثبّت تطبيق LocalDevVPN (أو StosVPN) من متجر التطبيقات، وشغّله. يجب أن يبقى الاتصال مفعّلًا أثناء التفعيل.")
                step(2, "جهّز ملف اقتران RemotePairing لهذا الجهاز، وهو الملف الذي يستخدمه StikDebug 3 (اسمه عادة rp_pairing_file.plist). ملفات الاقتران التقليدية (lockdown) لا تعمل مع الطريقة المدمجة.")
                step(3, "من الإعدادات ← السرعة (JIT) اختر «استيراد ملف الاقتران». وإن ظهر الملف باهتًا في المنتقي: شاركه من تطبيق الملفات إلى GTV Emu، أو انسخه إلى «ملفات ← على جهازي ← GTV Emu»، أو انسخ محتواه واختر «لصق ملف الاقتران من الحافظة». يتعرّف التطبيق على الملف من محتواه ويعرض نوعه.")
                step(4, "اضغط «تفعيل JIT الآن». في المرة الأولى تُنزَّل صورة المطوّر (DDI) وتُركَّب، وقد يستغرق ذلك دقيقة. بعدها يتم التفعيل تلقائيًا عند كل فتح للتطبيق.")
            } header: { Text("الطريقة المدمجة (موصى بها)") }

            Section {
                step(1, "ثبّت StikDebug الإصدار 3 أو أحدث، وفعّل فيه ملف الاقتران وLocalDevVPN.")
                step(2, "في إعدادات GTV Emu اختر «عبر StikDebug» ثم اضغط «تفعيل JIT الآن»؛ يفتح StikDebug ويرسل سكربت universal.js تلقائيًا ثم يعود إلى التطبيق.")
                step(3, "أو افتح GTV Emu من قائمة التطبيقات في StikDebug بعد اختيار سكربت universal.js له.")
            } header: { Text("الطريقة البديلة: StikDebug") }

            Section {
                row("تعذّر الوصول إلى الجهاز", "شغّل LocalDevVPN أو StosVPN وتأكد من اتصال Wi‑Fi.")
                row("ملف الاقتران غير صالح", "يتغير الملف عند إعادة ضبط الجهاز أو إلغاء الثقة بالحاسوب؛ أنشئ ملفًا جديدًا. والملف التقليدي (lockdown) لا يعمل هنا؛ استخدم ملف RemotePairing أو طريقة StikDebug.")
                row("تعذّر تجهيز صورة المطوّر", "تحقق من الإنترنت، أو احذف نسخة الصورة من الخيارات المتقدمة وأعد المحاولة.")
                row("أُرفق مصحّح ثم فُصل دون سكربت", "شُغّل التطبيق من StikDebug دون سكربت universal.js؛ استخدم زر التفعيل بدلًا من ذلك.")
                row("المترجم غير مفعّل بعد كل هذا", "يعمل التطبيق بالمفسّر، وهو أبطأ لكنه يعمل. أرسل نص الحالة الظاهر في الإعدادات عند الإبلاغ عن المشكلة.")
            } header: { Text("حل المشكلات") }
        }
        .navigationTitle("تفعيل JIT")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func step(_ n: Int, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Text("\(n)").font(.headline).frame(width: 24, height: 24)
                .background(Circle().fill(Color.accentColor.opacity(0.25)))
            Text(text).font(.callout)
        }
        .padding(.vertical, 2)
    }

    private func row(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.subheadline.bold())
            Text(text).font(.footnote).foregroundStyle(.secondary)
        }
    }
}

/// يُعرض مرة واحدة عند أول فتح للتطبيق
struct JITFirstRunSheet: View {
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            JITGuideView()
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("تم") { dismiss() } } }
        }
    }
}
