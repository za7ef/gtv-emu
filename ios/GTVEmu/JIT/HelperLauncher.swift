// تشغيل مساعد JIT المدمج (PlugIns/GTVJITHelper.appex) عبر واجهة NSExtension الخاصة،
// وهي الطريقة نفسها التي تستخدمها LiveContainer لتشغيل امتداداتها. طلب واحد لكل عملية.
import Foundation

@objc private protocol ExtensionClassShim {
    @objc(extensionWithIdentifier:error:)
    func `extension`(withIdentifier identifier: String) throws -> AnyObject
}

@objc private protocol ExtensionShim {
    @objc(beginExtensionRequestWithInputItems:completion:)
    func beginExtensionRequest(withInputItems items: [Any], completion: @escaping (NSUUID?) -> Void)
    @objc(setRequestCompletionBlock:)
    func setRequestCompletionBlock(_ block: @escaping (NSUUID?, [Any]?) -> Void)
    @objc(setRequestCancellationBlock:)
    func setRequestCancellationBlock(_ block: @escaping (NSUUID?, NSError?) -> Void)
    @objc(setRequestInterruptionBlock:)
    func setRequestInterruptionBlock(_ block: @escaping (NSUUID?) -> Void)
}

enum HelperLauncher {
    static let helperFile = "GTVJITHelper.appex"

    /// سبب عدم توفر المسار المدمج، أو nil إن كان متاحًا
    static var unavailableReason: String? {
#if targetEnvironment(simulator)
        return "التفعيل المدمج يعمل على جهاز حقيقي فقط."
#else
        guard #available(iOS 17.4, *) else { return "التفعيل المدمج يحتاج iOS 17.4 أو أحدث." }
        if getenv("LC_HOME_PATH") != nil { return "التفعيل المدمج غير متاح داخل LiveContainer؛ استخدم StikDebug." }
        if helperIdentifier == nil { return "مساعد JIT غير موجود في هذا التثبيت؛ أعد التثبيت مع الإبقاء على الامتدادات (App Extensions)." }
        return nil
#endif
    }

    /// معرّف المساعد في هذا التثبيت؛ أدوات التثبيت الجانبي قد تغيّره، فيُقرأ من ملفه
    static var helperIdentifier: String? {
        guard let url = Bundle.main.builtInPlugInsURL?.appendingPathComponent(helperFile) else { return nil }
        return Bundle(url: url)?.bundleIdentifier
    }

    /// الطلبات الجارية تبقى محفوظة حتى تنتهي
    private static var running: [Request] = []
    private static let lock = NSLock()

    static func send(_ request: JITHelperRequest, completion: @escaping (Result<JITHelperRequest.Response, Error>) -> Void) {
        func fail(_ msg: String) { completion(.failure(NSError(domain: "GTVJIT", code: 1, userInfo: [NSLocalizedDescriptionKey: msg]))) }
        if let r = unavailableReason { fail(r); return }
        guard let identifier = helperIdentifier else { fail("مساعد JIT غير موجود."); return }
        if NSClassFromString("NSExtension") == nil {
            dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW)
        }
        guard let cls = NSClassFromString("NSExtension") else { fail("لم يوفّر النظام واجهة الامتدادات اللازمة."); return }
        let factory = unsafeBitCast(cls as AnyObject, to: ExtensionClassShim.self)
        let ext: AnyObject
        do { ext = try factory.extension(withIdentifier: identifier) } catch {
            fail("تعذّر العثور على مساعد JIT (\(identifier)): \(error.localizedDescription)"); return
        }
        guard let data = try? JSONEncoder().encode(request) else { fail("تعذّر ترميز الطلب."); return }
        let r = Request(ext: ext, completion: completion)
        lock.lock(); running.append(r); lock.unlock()
        r.begin(data)
    }

    fileprivate static func finished(_ r: Request) {
        lock.lock(); running.removeAll { $0 === r }; lock.unlock()
    }

    fileprivate final class Request {
        let ext: AnyObject
        let shim: ExtensionShim
        var completion: ((Result<JITHelperRequest.Response, Error>) -> Void)?
        let lock = NSLock()

        init(ext: AnyObject, completion: @escaping (Result<JITHelperRequest.Response, Error>) -> Void) {
            self.ext = ext
            self.shim = unsafeBitCast(ext, to: ExtensionShim.self)
            self.completion = completion
        }

        func begin(_ data: Data) {
            shim.setRequestCompletionBlock { [self] _, items in
                let payload = (items?.first as? NSExtensionItem)?.userInfo?[JITHelperRequest.Response.itemKey] as? Data
                if let payload, let resp = try? JSONDecoder().decode(JITHelperRequest.Response.self, from: payload) {
                    end(.success(resp))
                } else {
                    end(.failure(err("انتهى مساعد JIT دون رد.")))
                }
            }
            shim.setRequestCancellationBlock { [self] _, e in
                end(.failure(err("توقف مساعد JIT: \(e?.localizedDescription ?? "أُلغي الطلب")")))
            }
            shim.setRequestInterruptionBlock { [self] _ in
                end(.failure(err("توقف مساعد JIT فجأة.")))
            }
            let item = NSExtensionItem()
            item.userInfo = [JITHelperRequest.itemKey: data]
            shim.beginExtensionRequest(withInputItems: [item]) { [self] uuid in
                if uuid == nil { end(.failure(err("لم يبدأ مساعد JIT؛ أعد التثبيت مع الإبقاء على الامتدادات."))) }
            }
        }

        private func err(_ m: String) -> NSError { NSError(domain: "GTVJIT", code: 2, userInfo: [NSLocalizedDescriptionKey: m]) }

        private func end(_ result: Result<JITHelperRequest.Response, Error>) {
            lock.lock()
            let c = completion; completion = nil
            lock.unlock()
            guard let c else { return }
            HelperLauncher.finished(self)
            DispatchQueue.main.async { c(result) }
        }
    }
}
