// منسّق تفعيل المترجم الفوري (JIT): التجهيز المبكر عند فتح التطبيق، والتفعيل المدمج بملف الاقتران،
// والتفعيل عبر StikDebug بديلًا، ورسائل سبب واضحة للإعدادات.
//
// لماذا التجهيز المبكر؟ على iOS 26 (أجهزة TXM) لا يكفي إرفاق المصحّح؛ يجب أن يجهّز المصحّح منطقة التنفيذ
// وهو ما يزال مرفقًا. كان التطبيق يطلبها عند ضغط «تشغيل» فقط، وفي ذلك الوقت قد يكون StikDebug أُغلق
// في الخلفية فانقطع الاتصال. الآن تُطلب المنطقة فور إرفاق المصحّح، ثم يُفصل، وتُحفظ للجلسة كلها.
import Foundation
import SwiftUI
import UIKit

/// تُعدَّل خصائصه المنشورة على الخيط الرئيسي فقط.
final class JITManager: ObservableObject {
    static let shared = JITManager()

    enum Method: String, CaseIterable, Identifiable {
        case builtIn, stikDebug
        var id: String { rawValue }
        var title: String {
            switch self {
            case .builtIn: return "مدمج (ملف الاقتران)"
            case .stikDebug: return "عبر StikDebug"
            }
        }
    }

    enum Phase: Equatable {
        case idle
        case working(String)
        case ready
        case failed(String)
    }

    static let methodKey = "jitMethod"
    static let autoKey = "jitAutoEnable"

    @Published private(set) var phase: Phase = .idle
    /// النص الأصلي من أداة التفعيل، يُعرض للتشخيص
    @Published private(set) var detail = ""
    @Published private(set) var hasPairing = false
    @Published private(set) var pairingDate: Date?
    @Published private(set) var txmPresent: Bool?

    /// علم إلغاء الانتظار؛ يُقرأ من الخيط الخلفي
    private let cancelLock = NSLock()
    private var _cancelWait = false
    private var cancelWait: Bool {
        get { cancelLock.lock(); defer { cancelLock.unlock() }; return _cancelWait }
        set { cancelLock.lock(); _cancelWait = newValue; cancelLock.unlock() }
    }

    private init() { refreshPairing() }

    var method: Method {
        Method(rawValue: UserDefaults.standard.string(forKey: Self.methodKey) ?? "") ?? .builtIn
    }
    var autoEnable: Bool {
        UserDefaults.standard.object(forKey: Self.autoKey) as? Bool ?? true
    }
    private var jitMode: JITMode {
        JITMode(rawValue: UserDefaults.standard.integer(forKey: "jitMode")) ?? .auto
    }
    var isReady: Bool { gtv_jit_ready() != 0 }

    // MARK: - ملف الاقتران

    /// يُحفظ داخل حاوية التطبيق في مجلد لا يظهر في تطبيق الملفات، ولا يُنسخ احتياطيًا
    static var pairingURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("JIT", isDirectory: true)
        return dir.appendingPathComponent("pairingFile.plist")
    }

    func refreshPairing() {
        let attrs = try? FileManager.default.attributesOfItem(atPath: Self.pairingURL.path)
        hasPairing = attrs != nil
        pairingDate = attrs?[.modificationDate] as? Date
    }

    enum PairingError: LocalizedError {
        case unreadable, notPlist, notPairing
        var errorDescription: String? {
            switch self {
            case .unreadable: return "تعذّرت قراءة الملف."
            case .notPlist: return "الملف ليس بصيغة plist؛ اختر ملف ‎.mobiledevicepairing أو ‎.plist."
            case .notPairing: return "الملف لا يحتوي مفاتيح الاقتران (HostID وHostPrivateKey)؛ أنشئ ملف اقتران جديدًا لهذا الجهاز."
            }
        }
    }

    func importPairing(from url: URL) throws {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url) else { throw PairingError.unreadable }
        guard let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else {
            throw PairingError.notPlist
        }
        guard plist["HostID"] != nil, plist["HostPrivateKey"] != nil else { throw PairingError.notPairing }
        let dest = Self.pairingURL
        var dir = dest.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var values = URLResourceValues(); values.isExcludedFromBackup = true
        try? dir.setResourceValues(values)
        try data.write(to: dest, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        refreshPairing()
        if case .failed = phase { phase = .idle; detail = "" }
    }

    func removePairing() {
        try? FileManager.default.removeItem(at: Self.pairingURL)
        refreshPairing()
    }

    // MARK: - دورة حياة التطبيق

    /// يُستدعى مرة واحدة عند فتح التطبيق
    func onLaunch() {
        gtv_set_jit_mode(Int32(jitMode.rawValue))
        guard jitMode != .off else { phase = .idle; syncEngine(); return }
        if gtv_jit_debug_state() & 2 != 0 {
            // فُتح التطبيق من StikDebug والمصحّح ما يزال مرفقًا: تُجهَّز المنطقة فورًا قبل أي شيء آخر
            phase = .working("تجهيز منطقة التنفيذ…")
            waitAndPrewarm(seconds: 5)
        } else if method == .builtIn, autoEnable, hasPairing, HelperLauncher.unavailableReason == nil {
            enable()
        } else {
            syncEngine()
        }
    }

    /// عند العودة إلى التطبيق (مثلًا بعد StikDebug)
    func onBecameActive() {
        guard !isReady, jitMode != .off else { return }
        if case .working = phase { return }
        if gtv_jit_debug_state() & 2 != 0 {
            phase = .working("تجهيز منطقة التنفيذ…")
            waitAndPrewarm(seconds: 5)
        }
    }

    // MARK: - التفعيل

    func enable() {
        guard !isReady else { phase = .ready; syncEngine(); return }
        gtv_set_jit_mode(Int32(jitMode.rawValue))
        guard jitMode != .off else { fail("المترجم الفوري معطّل من الخيارات المتقدمة."); return }
        guard Self.hasGetTaskAllow else {
            fail("هذا التثبيت لا يحمل صلاحية get-task-allow، فلا يمكن إرفاق مصحّح به. أعد التثبيت بأداة تحافظ على هذه الصلاحية.")
            return
        }
        switch method {
        case .builtIn: enableBuiltIn()
        case .stikDebug: openStikDebug()
        }
    }

    private func enableBuiltIn() {
        if let r = HelperLauncher.unavailableReason { fail(r); return }
        guard let pairing = try? Data(contentsOf: Self.pairingURL) else {
            fail("لا يوجد ملف اقتران؛ استورده من الإعدادات أولًا.")
            return
        }
        phase = .working("الاتصال بالجهاز عبر VPN وتجهيز صورة المطوّر…")
        detail = ""
        // ينتظر التطبيق إرفاق المصحّح ثم يطلب المنطقة؛ والمساعد يعالج الطلب ويعود بعد الفصل
        waitAndPrewarm(seconds: 180)
        let req = JITHelperRequest(operation: .enable, targetPID: getpid(), pairingData: pairing)
        HelperLauncher.send(req) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let r):
                self.txmPresent = r.txmPresent ?? self.txmPresent
                if r.success { self.finishIfReady() }
                else { self.cancelWait = true; self.fail(Self.message(for: r.kind), detail: r.detail) }
            case .failure(let e):
                self.cancelWait = true
                self.fail(e.localizedDescription)
            }
        }
    }

    private func openStikDebug() {
        var c = URLComponents()
        c.scheme = "stikdebug"
        c.host = "enable-jit"
        c.queryItems = [
            URLQueryItem(name: "bundle-id", value: Bundle.main.bundleIdentifier ?? "com.za7ef.gtvemu"),
            URLQueryItem(name: "pid", value: String(getpid())),
            URLQueryItem(name: "script-name", value: "universal.js"),
        ]
        guard let url = c.url else { fail("تعذّر إنشاء رابط StikDebug."); return }
        phase = .working("بانتظار StikDebug…")
        UIApplication.shared.open(url) { [weak self] ok in
            guard let self else { return }
            if !ok { self.fail("StikDebug غير مثبّت، أو لا يدعم فتح الروابط. ثبّت الإصدار 3 أو أحدث، أو استخدم التفعيل المدمج.") }
            else { self.waitAndPrewarm(seconds: 90) }
        }
    }

    /// ينتظر إرفاق المصحّح في خيط خلفي، ثم يجهّز منطقة التنفيذ ويفصل المصحّح
    private func waitAndPrewarm(seconds: Double) {
        cancelWait = false
        Thread.detachNewThread { [weak self] in
            let deadline = Date().addingTimeInterval(seconds)
            var traced = false
            while Date() < deadline {
                if gtv_jit_debug_state() & 2 != 0 { traced = true; break }
                if self?.cancelWait ?? true { return }
                Thread.sleep(forTimeInterval: 0.1)
            }
            if traced { _ = gtv_jit_prewarm(0) }
            DispatchQueue.main.async {
                guard let self else { return }
                if self.isReady { self.phase = .ready; self.syncEngine() }
                else if traced || self.method == .stikDebug || gtv_jit_debug_state() != 0 {
                    self.fail(String(cString: gtv_jit_status()))
                } else if case .working = self.phase, !traced, Date() >= deadline {
                    self.fail("انتهت مهلة انتظار المصحّح.")
                }
            }
        }
    }

    /// المساعد فصل المصحّح بنجاح. على الأجهزة دون TXM يكفي الإرفاق والفصل، فقد ينتهي قبل أن يلحظ
    /// خيط الانتظار الإرفاق؛ فتُجرَّب المنطقة مباشرة (المسار القديم لا يحتاج مصحّحًا مرفقًا).
    private func finishIfReady() {
        cancelWait = true
        if isReady { phase = .ready; syncEngine(); return }
        DispatchQueue.global().async { [weak self] in
            _ = gtv_jit_prewarm(0)
            DispatchQueue.main.async {
                guard let self else { return }
                if self.isReady { self.phase = .ready; self.syncEngine() }
                else { self.fail(String(cString: gtv_jit_status())) }
            }
        }
    }

    // MARK: - فحص وصيانة

    /// فحص الاتصال وصورة المطوّر دون تفعيل
    func checkConnection() {
        guard let pairing = try? Data(contentsOf: Self.pairingURL) else { fail("لا يوجد ملف اقتران."); return }
        phase = .working("فحص الاتصال وصورة المطوّر…")
        HelperLauncher.send(JITHelperRequest(operation: .prepare, targetPID: nil, pairingData: pairing)) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let r):
                self.txmPresent = r.txmPresent
                if r.success { self.phase = self.isReady ? .ready : .idle; self.detail = "الاتصال سليم وصورة المطوّر مركّبة." }
                else { self.fail(Self.message(for: r.kind), detail: r.detail) }
            case .failure(let e): self.fail(e.localizedDescription)
            }
        }
    }

    func resetDDI() {
        HelperLauncher.send(JITHelperRequest(operation: .resetDDI, targetPID: nil, pairingData: nil)) { [weak self] result in
            if case .failure(let e) = result { self?.detail = e.localizedDescription }
            else { self?.detail = "حُذفت نسخة صورة المطوّر؛ ستُنزَّل من جديد عند التفعيل." }
        }
    }

    // MARK: - الرسائل

    private func fail(_ message: String, detail: String = "") {
        phase = .failed(message)
        if !detail.isEmpty { self.detail = detail }
        syncEngine()
    }

    private func syncEngine() { EmulatorEngine.shared.refreshJITStatus() }

    static func message(for kind: JITHelperRequest.Response.Kind) -> String {
        switch kind {
        case .ok: return "تم."
        case .vpnUnreachable: return "تعذّر الوصول إلى الجهاز: شغّل LocalDevVPN أو StosVPN، وتأكد من اتصال Wi‑Fi، ثم أعد المحاولة."
        case .pairingInvalid: return "ملف الاقتران غير صالح أو انتهت صلاحيته: أنشئ ملفًا جديدًا لهذا الجهاز ثم استورده."
        case .ddiFailed: return "تعذّر تجهيز صورة المطوّر (DDI): تحقق من الإنترنت، أو احذف نسخة الصورة من الإعدادات وأعد المحاولة."
        case .attachFailed: return "تعذّر إرفاق المصحّح بالتطبيق: تأكد أن التثبيت يحتفظ بصلاحية get-task-allow."
        case .scriptFailed: return "أُرفق المصحّح لكن سكربت التفعيل فشل."
        case .other: return "تعذّر التفعيل."
        }
    }

    /// وصف حالة الجلسة الحالية لصفحة الإعدادات
    var summary: String {
        switch phase {
        case .ready: return String(cString: gtv_jit_status())
        case .working(let s): return s
        case .failed(let s): return s
        case .idle:
            if isReady { return String(cString: gtv_jit_status()) }
            if jitMode == .off { return "معطّل من الخيارات المتقدمة" }
            if method == .builtIn && !hasPairing { return "غير مفعّل: استورد ملف الاقتران" }
            return "غير مفعّل"
        }
    }

    // MARK: - get-task-allow

    /// فحص صلاحية get-task-allow للتطبيق نفسه (واجهة SecTask الخاصة، تُحمَّل ديناميكيًا)
    static var hasGetTaskAllow: Bool {
        typealias CreateFn = @convention(c) (UnsafeRawPointer?) -> UnsafeMutableRawPointer?
        typealias CopyFn = @convention(c) (UnsafeMutableRawPointer, CFString, UnsafeMutableRawPointer?) -> Unmanaged<CFTypeRef>?
        guard let h = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW),
              let c = dlsym(h, "SecTaskCreateFromSelf"), let v = dlsym(h, "SecTaskCopyValueForEntitlement") else { return true }
        let create = unsafeBitCast(c, to: CreateFn.self)
        let copy = unsafeBitCast(v, to: CopyFn.self)
        guard let task = create(nil) else { return true }
        defer { Unmanaged<AnyObject>.fromOpaque(task).release() }
        let value = copy(task, "get-task-allow" as CFString, nil)?.takeRetainedValue()
        return (value as? Bool) == true
    }
}
