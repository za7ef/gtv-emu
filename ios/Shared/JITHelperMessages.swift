// رسائل التطبيق ومساعد JIT المدمج؛ يُترجم هذا الملف في الهدفين معًا.
// تنتقل الرسالة نصَّ JSON داخل عنصر طلب الامتداد، ذهابًا وإيابًا.
import Foundation

struct JITHelperRequest: Codable {
    static let itemKey = "gtv-jit-request"

    enum Operation: String, Codable {
        /// فحص الاتصال عبر VPN، وتنزيل صورة المطوّر (DDI) وتركيبها عند الحاجة
        case prepare
        /// الإرفاق بالعملية targetPID وتشغيل السكربت حتى يفصل التطبيق المصحّح
        case enable
        /// حذف نسخة صورة المطوّر المخزنة
        case resetDDI
    }

    var operation: Operation
    var targetPID: Int32?
    var pairingData: Data?

    struct Response: Codable {
        static let itemKey = "gtv-jit-response"
        /// رموز ثابتة يترجمها التطبيق إلى رسالة عربية واضحة
        enum Kind: String, Codable {
            case ok, vpnUnreachable, pairingInvalid, ddiFailed, attachFailed, scriptFailed, other
        }
        var success: Bool
        var kind: Kind
        /// النص الأصلي من StikJIT، يُعرض تحت الرسالة العربية للتشخيص
        var detail: String
        var txmPresent: Bool?
    }
}
