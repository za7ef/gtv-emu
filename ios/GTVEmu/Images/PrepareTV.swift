// تجهيز نظام Android TV على الجهاز:
// 1) استخراج أقسام النظام من ملف Google الأصلي إلى قرص واحد بأقسام GPT (core/atvprep.c)
// 2) تشغيل آلة Alpine صغيرة على القرص تعدّل الأقسام بأدوات e2fsprogs (android/tv/prep/prep.sh)
// لا يُعاد توزيع أي ملف من Google؛ الملف يُنزَّل من dl.google.com ويُجهَّز هنا فقط.
import Foundation
import UIKit

/// صندوق يمرَّر إلى دالة C لاستقبال التقدم
private final class ProgressBox {
    let report: (Double, String) -> Void
    init(_ r: @escaping (Double, String) -> Void) { report = r }
}

extension ImageStore {
    /// يبدأ التجهيز؛ الحالة تُنشر في preparingID وprepareProgress وprepareStatus
    @MainActor func prepare(_ p: ImageProfile, jitMode: JITMode) {
        guard preparingID == nil, !EmulatorEngine.shared.isRunning else { return }
        guard let zip = p.files.first(where: { $0.role == "source" }), isPresent(p, zip),
              let diskFile = p.files.first(where: { $0.role == "disk" && $0.generated == true }),
              let kernel = url(p, role: "prep_kernel"), let initrd = url(p, role: "prep_initrd") else {
            prepareStatus = "فشل التجهيز: ملفات ناقصة، نزّل النظام أولًا"
            return
        }
        // المساحة: القرص يشغل نحو 1.7 غيغابايت فعليًا، ويكبر مع استخدام النظام
        let need: Int64 = 2_500_000_000
        if let v = try? Self.root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
           let free = v.volumeAvailableCapacityForImportantUsage, free < need {
            prepareStatus = "فشل التجهيز: المساحة الفارغة غير كافية (يلزم نحو 2.5 غيغابايت)"
            return
        }
        let zipURL = localURL(p, zip)
        let dst = localURL(p, diskFile)
        let part = dst.appendingPathExtension("part")
        preparingID = p.id
        prepareProgress = 0
        prepareStatus = "استخراج الأقسام..."
        UIApplication.shared.isIdleTimerDisabled = true

        Task.detached(priority: .userInitiated) {
            try? FileManager.default.removeItem(at: part)
            let box = ProgressBox { f, stage in
                Task { @MainActor in
                    ImageStore.shared.prepareProgress = f * 0.3
                    ImageStore.shared.prepareStatus = "استخراج الأقسام: \(stage) \(Int(f * 100))٪"
                }
            }
            let ctx = Unmanaged.passRetained(box).toOpaque()
            var err = [CChar](repeating: 0, count: 256)
            let rc = gtv_atv_extract(zipURL.path, part.path, nil, { ctx, f, stage in
                guard let ctx = ctx else { return }
                let b = Unmanaged<ProgressBox>.fromOpaque(ctx).takeUnretainedValue()
                b.report(f, stage.map { String(cString: $0) } ?? "")
            }, ctx, &err, err.count)
            Unmanaged<ProgressBox>.fromOpaque(ctx).release()
            if rc != 0 {
                let msg = String(cString: err)
                await MainActor.run { ImageStore.shared.finishPrepare("فشل الاستخراج: \(msg)") }
                return
            }
            await MainActor.run { ImageStore.shared.runPrepGuest(p, kernel: kernel, initrd: initrd, part: part, dst: dst, zip: zipURL, jitMode: jitMode) }
        }
    }

    @MainActor private func runPrepGuest(_ p: ImageProfile, kernel: URL, initrd: URL, part: URL, dst: URL, zip: URL, jitMode: JITMode) {
        let engine = EmulatorEngine.shared
        prepareProgress = 0.3
        prepareStatus = "تعديل الأقسام داخل آلة التجهيز..."
        engine.console.clear()
        var r = BootRequest(ramMB: 512, cores: 2, jitMode: jitMode)
        r.kernel = kernel
        r.initrd = initrd
        r.disk = part
        engine.start(r)
        guard engine.isRunning else {
            finishPrepare("فشل تشغيل آلة التجهيز")
            return
        }
        let steps = ["vendor: resize": 0.4, "vendor: unshare blocks": 0.5, "vendor: config files": 0.7,
                     "vendor: graphics stack": 0.75, "system: selinux": 0.9]
        Task { @MainActor in
            var ok = false, failure: String?
            while true {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                let log = engine.console.fullLog
                for (k, v) in steps where log.contains("GTVPREP-STEP: " + k) && self.prepareProgress < v {
                    self.prepareProgress = v
                    self.prepareStatus = "تعديل الأقسام: " + Self.stepTitle(k)
                }
                if log.contains("GTVPREP-OK") { ok = true }
                if let range = log.range(of: "GTVPREP-FAIL: ") {
                    failure = String(log[range.upperBound...].prefix { $0 != "\n" })
                }
                if !engine.isRunning { break }
                // بعد النتيجة تطفئ الآلة نفسها؛ إن لم تفعل خلال مدة قصيرة تُوقف يدويًا
                if ok || failure != nil {
                    for _ in 0..<20 where engine.isRunning { try? await Task.sleep(nanoseconds: 500_000_000) }
                    if engine.isRunning { engine.stop() }
                    for _ in 0..<20 where engine.isRunning { try? await Task.sleep(nanoseconds: 500_000_000) }
                    break
                }
            }
            if ok {
                do {
                    try? FileManager.default.removeItem(at: dst)
                    try FileManager.default.moveItem(at: part, to: dst)
                    try? FileManager.default.removeItem(at: zip)   // ملف Google لم يعد لازمًا
                    self.finishPrepare(nil)
                } catch {
                    self.finishPrepare("فشل حفظ القرص: \(error.localizedDescription)")
                }
            } else {
                try? FileManager.default.removeItem(at: part)
                self.finishPrepare("فشل التجهيز: " + (failure ?? "توقفت آلة التجهيز قبل الانتهاء"))
            }
        }
    }

    @MainActor fileprivate func finishPrepare(_ error: String?) {
        UIApplication.shared.isIdleTimerDisabled = false
        preparingID = nil
        prepareProgress = error == nil ? 1 : 0
        prepareStatus = error ?? "اكتمل التجهيز، النظام جاهز للتشغيل"
        objectWillChange.send()
    }

    private static func stepTitle(_ k: String) -> String {
        switch k {
        case "vendor: resize": return "توسيع قسم vendor"
        case "vendor: unshare blocks": return "فك الكتل المشتركة"
        case "vendor: config files": return "ملفات الإعداد"
        case "vendor: graphics stack": return "مكدّس الرسوميات"
        case "system: selinux": return "سياسة SELinux"
        default: return k
        }
    }
}
