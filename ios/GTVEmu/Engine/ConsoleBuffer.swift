// مخزن الطرفية: يجمع مخرجات المنفذ التسلسلي ورسائل المحاكي (stderr) ويعرضها في الواجهة.
// الإضافة تحدث من خيط المحاكي بايتًا بايتًا، فتُجمع في مخزن محمي بقفل وتُنشر عشر مرات في الثانية.
import Foundation
import Combine

final class ConsoleBuffer: ObservableObject {
    @Published private(set) var text: String = ""

    private let lock = NSLock()
    private var pending = Data()
    private var all = Data()
    private var timer: Timer?
    private var stderrPipe: Pipe?
    private let maxBytes = 256 * 1024

    init() {
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in self?.publish() }
    }

    func append(_ bytes: UnsafeRawBufferPointer) {
        lock.lock(); pending.append(contentsOf: bytes); lock.unlock()
    }

    func appendLine(_ s: String) {
        lock.lock(); pending.append(contentsOf: Array((s + "\n").utf8)); lock.unlock()
    }

    func clear() {
        lock.lock(); pending.removeAll(); all.removeAll(); lock.unlock()
        text = ""
    }

    /// السجل الكامل كنص، للمشاركة أو النسخ
    var fullLog: String {
        lock.lock(); defer { lock.unlock() }
        return Self.clean(all + pending)
    }

    /// يحوّل stderr إلى الطرفية حتى تظهر رسائل المحاكي ([gtv] ...) داخل التطبيق
    func captureStderr() {
        let p = Pipe()
        stderrPipe = p
        setvbuf(stderr, nil, _IONBF, 0)
        dup2(p.fileHandleForWriting.fileDescriptor, STDERR_FILENO)
        p.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty, let self = self else { return }
            self.lock.lock(); self.pending.append(d); self.lock.unlock()
        }
    }

    private func publish() {
        lock.lock()
        guard !pending.isEmpty else { lock.unlock(); return }
        all.append(pending)
        pending.removeAll(keepingCapacity: true)
        if all.count > maxBytes { all.removeFirst(all.count - maxBytes) }
        let snapshot = all
        lock.unlock()
        text = Self.clean(Data(snapshot.suffix(64 * 1024))) // نعرض آخر 64 كيلوبايت فقط للحفاظ على سرعة الواجهة
    }

    /// يزيل تسلسلات ANSI ومحارف الإرجاع، ويتسامح مع UTF-8 المقطوع
    static func clean(_ d: Data) -> String {
        var out = [UInt8](); out.reserveCapacity(d.count)
        var i = d.startIndex
        while i < d.endIndex {
            let b = d[i]
            if b == 0x1B { // ESC [ ... حرف
                var j = d.index(after: i)
                if j < d.endIndex && d[j] == 0x5B {
                    j = d.index(after: j)
                    while j < d.endIndex && !(0x40...0x7E).contains(d[j]) { j = d.index(after: j) }
                    if j < d.endIndex { j = d.index(after: j) }
                }
                i = j; continue
            }
            if b != 0x0D && b != 0x07 { out.append(b) }
            i = d.index(after: i)
        }
        return String(decoding: out, as: UTF8.self)
    }
}
