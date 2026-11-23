// مساعد JIT المدمج: امتداد مستقل يعمل في عملية منفصلة، لأن العملية لا تستطيع تصحيح نفسها.
// يستقبل من التطبيق رقم عمليته وملف الاقتران، ثم يتصل بالجهاز نفسه عبر LocalDevVPN بمكتبة StikJIT
// (المبنية على idevice)، فيركّب صورة المطوّر عند الحاجة، ويُرفق debugserver بالتطبيق، ويشغّل سكربت
// universal.js المضمّن في StikJIT حتى يجهّز التطبيق منطقة التنفيذ ويفصل المصحّح.
import Foundation
import StikJIT

private enum HelperWork {
    /// طلب واحد في كل مرة: الإرفاق يحجز الطابور حتى يفصل التطبيق المصحّح
    static let queue = DispatchQueue(label: "com.za7ef.gtvemu.jit-helper")

    static func handle(_ request: JITHelperRequest) -> JITHelperRequest.Response {
        let fm = FileManager.default
        let root = fm.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
        let paths = DDIPaths.default(in: root)
        func reply(_ ok: Bool, _ kind: JITHelperRequest.Response.Kind, _ detail: String) -> JITHelperRequest.Response {
            .init(success: ok, kind: kind, detail: detail, txmPresent: StikJIT.isTXMPresent)
        }
        do {
            try fm.createDirectory(at: root, withIntermediateDirectories: true)
            if request.operation == .resetDDI {
                try StikJIT.resetCachedDDI(at: paths)
                return reply(true, .ok, "DDI cache removed")
            }
            guard let data = request.pairingData, !data.isEmpty else {
                return reply(false, .pairingInvalid, "no pairing data")
            }
            // نسخة مؤقتة داخل الامتداد تُحذف فور انتهاء العملية
            let pairingURL = root.appendingPathComponent("pairing-\(UUID().uuidString).plist")
            try data.write(to: pairingURL, options: .atomic)
            defer { try? fm.removeItem(at: pairingURL) }

            switch request.operation {
            case .prepare:
                switch StikJIT.prepareDevice(pairingFile: pairingURL, paths: paths) {
                case .ready:
                    return reply(true, .ok, "ready")
                case .unreachable(let reason):
                    return reply(false, .vpnUnreachable, reason)
                case .preparationFailed(let reason):
                    return reply(false, classify(reason), reason)
                @unknown default:
                    return reply(false, .other, "unknown readiness state")
                }
            case .enable:
                guard let pid = request.targetPID else { return reply(false, .other, "no target pid") }
                try StikJIT.enableJIT(targetPID: pid, pairingFile: pairingURL, ddiPaths: paths,
                                      script: .universal, forceScript: false,
                                      progress: { NSLog("[GTVJIT] %@", $0) })
                return reply(true, .ok, "detached")
            case .resetDDI:
                return reply(true, .ok, "")
            }
        } catch let e as StikJITError {
            let text = e.localizedDescription
            switch e {
            case .pairingFile: return reply(false, .pairingInvalid, text)
            case .ddiDownload, .ddiFilesMissing, .ddiNotMounted: return reply(false, .ddiFailed, text)
            case .debugProxyUnavailable: return reply(false, .attachFailed, text)
            case .scriptExecution, .scriptUnavailable, .customScript: return reply(false, .scriptFailed, text)
            case .deviceNotReady(let r):
                let k = classify(r)
                return reply(false, k == .other ? .vpnUnreachable : k, text)
            default: return reply(false, classify(text), text)
            }
        } catch {
            return reply(false, classify(error.localizedDescription), error.localizedDescription)
        }
    }

    /// تصنيف تقريبي للنصوص الحرة التي تعيدها المكتبة
    static func classify(_ s: String) -> JITHelperRequest.Response.Kind {
        let t = s.lowercased()
        if t.contains("pairing") || t.contains("certificate") || t.contains("invalidhostid") || t.contains("password") { return .pairingInvalid }
        if t.contains("developer disk image") || t.contains("ddi") || t.contains("mount") { return .ddiFailed }
        if t.contains("timed out") || t.contains("timeout") || t.contains("connect") || t.contains("unreachable") || t.contains("refused") { return .vpnUnreachable }
        return .other
    }
}

/// الصنف الرئيسي للامتداد (NSExtensionPrincipalClass): يصل الطلب في أول عنصر،
/// ويُختم الطلب بالرد؛ طلب التفعيل لا يُختم إلا بعد فصل المصحّح.
@objc(GTVJITHelperHandler)
final class GTVJITHelperHandler: NSObject, NSExtensionRequestHandling {
    func beginRequest(with context: NSExtensionContext) {
        let info = (context.inputItems.first as? NSExtensionItem)?.userInfo
        let data = info?[JITHelperRequest.itemKey] as? Data
        HelperWork.queue.async {
            let response: JITHelperRequest.Response
            if let data, let req = try? JSONDecoder().decode(JITHelperRequest.self, from: data) {
                response = HelperWork.handle(req)
            } else {
                response = .init(success: false, kind: .other, detail: "empty request", txmPresent: nil)
            }
            let item = NSExtensionItem()
            item.userInfo = [JITHelperRequest.Response.itemKey: (try? JSONEncoder().encode(response)) ?? Data()]
            context.completeRequest(returningItems: [item], completionHandler: nil)
        }
    }
}
