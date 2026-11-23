// محرك المحاكاة: يربط الواجهة البرمجية للمحاكي (gtv.h) بالتطبيق.
// يشغّل الآلة في خيط مخصص، ويستقبل مخرجات الطرفية والصوت والإطارات عبر خطافات المضيف.
import Foundation
import Combine

/// أوضاع المترجم الفوري كما في gtv.h
enum JITMode: Int, CaseIterable, Identifiable {
    case auto = 0, txm = 1, legacy = 2, off = 3
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .auto: return "تلقائي"
        case .txm: return "TXM (iOS 26 وما بعده)"
        case .legacy: return "المسار القديم"
        case .off: return "معطّل (المفسّر فقط)"
        }
    }
}

/// إعدادات تشغيل آلة واحدة
struct BootRequest {
    var kernel: URL?
    var initrd: URL?
    var bootImage: URL?
    var vendorBoot: URL?
    var initBoot: URL?
    var disk: URL?
    var cmdline: String?
    var androidboot: [String] = []
    var ramMB: Int
    var cores: Int
    var jitMode: JITMode
}

final class EmulatorEngine: ObservableObject {
    static let shared = EmulatorEngine()

    enum RunState: Equatable { case idle, running, stopped(String) }

    @Published private(set) var state: RunState = .idle
    @Published private(set) var jitStatus: String = ""
    @Published private(set) var jitActive = false
    @Published private(set) var mips: Double = 0
    @Published private(set) var instructions: UInt64 = 0

    let console = ConsoleBuffer()
    let audio = AudioOutput()

    private var machine: OpaquePointer?
    // بنية الخطافات في ذاكرة ثابتة العنوان، لأن المحاكي يحتفظ بمؤشر إليها
    private let host: UnsafeMutablePointer<GtvHost> = {
        let p = UnsafeMutablePointer<GtvHost>.allocate(capacity: 1)
        p.initialize(to: GtvHost())
        return p
    }()
    private var thread: Thread?
    private var statsTimer: Timer?
    private var lastCount: UInt64 = 0
    private var lastTime = Date()

    // طابور إدخال الطرفية (من الواجهة إلى المنفذ التسلسلي)
    private let inputLock = NSLock()
    private var inputBytes: [UInt8] = []
    // نسخ C للسلاسل؛ تبقى حية طوال عمر الآلة
    private var cStrings: [UnsafeMutablePointer<CChar>] = []

    private init() {
        console.captureStderr()
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        host.pointee.ctx = ctx
        host.pointee.console_out = { ctx, s, n in
            guard let ctx = ctx, let s = s else { return }
            let e = Unmanaged<EmulatorEngine>.fromOpaque(ctx).takeUnretainedValue()
            e.console.append(UnsafeRawBufferPointer(start: s, count: n))
        }
        host.pointee.console_in = { ctx, buf, max in
            guard let ctx = ctx, let buf = buf else { return 0 }
            let e = Unmanaged<EmulatorEngine>.fromOpaque(ctx).takeUnretainedValue()
            return e.takeInput(into: buf, max: max)
        }
        host.pointee.console_in_ready = { ctx in
            guard let ctx = ctx else { return 0 }
            let e = Unmanaged<EmulatorEngine>.fromOpaque(ctx).takeUnretainedValue()
            return e.hasInput ? 1 : 0
        }
        host.pointee.audio_out = { ctx, pcm, frames, rate, channels in
            guard let ctx = ctx, let pcm = pcm else { return }
            let e = Unmanaged<EmulatorEngine>.fromOpaque(ctx).takeUnretainedValue()
            e.audio.enqueue(pcm, frames: Int(frames), rate: Double(rate), channels: Int(channels))
        }
        host.pointee.frame_ready = nil // العرض يسحب الإطارات بنفسه مع كل تحديث للشاشة
        jitStatus = String(cString: gtv_jit_status())
    }

    var isRunning: Bool { state == .running }

    /// تحديث حالة JIT المعروضة بعد التجهيز المبكر أو محاولة تفعيل
    func refreshJITStatus() {
        jitStatus = String(cString: gtv_jit_status())
        if !isRunning { jitActive = gtv_jit_ready() != 0 }
    }

    // MARK: - التشغيل والإيقاف

    func start(_ req: BootRequest) {
        guard !isRunning else { return }
        gtv_set_host(host)
        gtv_set_jit_mode(Int32(req.jitMode.rawValue))

        var cfg = GtvConfig()
        cfg.kernel = cstr(req.kernel?.path)
        cfg.initrd = cstr(req.initrd?.path)
        cfg.bootimg = cstr(req.bootImage?.path)
        cfg.vendor_boot = cstr(req.vendorBoot?.path)
        cfg.init_boot = cstr(req.initBoot?.path)
        cfg.disk = cstr(req.disk?.path)
        cfg.cmdline = cstr(req.cmdline)
        cfg.ram_mb = UInt32(req.ramMB)
        cfg.ncpu = Int32(req.cores)
        cfg.use_jit = req.jitMode == .off ? 0 : 1
        let props = Array(req.androidboot.prefix(32))
        withUnsafeMutableBytes(of: &cfg.androidboot) { raw in
            let slots = raw.bindMemory(to: UnsafePointer<CChar>?.self)
            for (i, p) in props.enumerated() { slots[i] = cstr(p) }
        }
        cfg.nandroidboot = Int32(props.count)

        console.appendLine("[تطبيق] بدء الآلة: ذاكرة \(req.ramMB) ميغابايت، \(req.cores) معالج، JIT: \(req.jitMode.title)")
        var err = [CChar](repeating: 0, count: 256)
        guard let m = gtv_create(&cfg, &err, err.count) else {
            let msg = String(cString: err)
            console.appendLine("[تطبيق] فشل الإنشاء: \(msg)")
            state = .stopped(msg)
            return
        }
        machine = m
        jitActive = gtv_jit_active(m) != 0
        jitStatus = String(cString: gtv_jit_status())
        console.appendLine("[تطبيق] المترجم الفوري: \(jitStatus)")
        audio.start()
        state = .running
        lastCount = 0; lastTime = Date()

        let t = Thread { [weak self] in
            let rc = gtv_run(m, 0)
            DispatchQueue.main.async { self?.finished(rc: Int(rc)) }
        }
        t.name = "gtv-emu"
        t.stackSize = 16 << 20
        t.qualityOfService = .userInteractive
        thread = t
        t.start()
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.sampleStats() }
    }

    func stop() {
        guard let m = machine, isRunning else { return }
        gtv_stop(m)
    }

    private func finished(rc: Int) {
        statsTimer?.invalidate(); statsTimer = nil
        audio.stop()
        if let m = machine {
            instructions = gtv_icount(m)
            gtv_destroy(m)
        }
        machine = nil
        thread = nil
        cStrings.forEach { free($0) }
        cStrings.removeAll()
        console.appendLine("\n[تطبيق] توقفت الآلة بعد \(instructions) تعليمة")
        state = .stopped("توقفت الآلة")
        mips = 0
    }

    private func sampleStats() {
        guard let m = machine else { return }
        let n = gtv_icount(m)
        let now = Date()
        let dt = now.timeIntervalSince(lastTime)
        if dt > 0 { mips = Double(n &- lastCount) / dt / 1e6 }
        lastCount = n; lastTime = now
        instructions = n
    }

    private func cstr(_ s: String?) -> UnsafePointer<CChar>? {
        guard let s = s else { return nil }
        let p = strdup(s)!
        cStrings.append(p)
        return UnsafePointer(p)
    }

    // MARK: - الإدخال

    /// يرسل نصًا إلى طرفية الضيف (المنفذ التسلسلي)
    func sendConsole(_ text: String) {
        let bytes = Array(text.replacingOccurrences(of: "\n", with: "\r").utf8)
        inputLock.lock(); inputBytes.append(contentsOf: bytes); inputLock.unlock()
    }

    func sendConsoleBytes(_ bytes: [UInt8]) {
        inputLock.lock(); inputBytes.append(contentsOf: bytes); inputLock.unlock()
    }

    fileprivate var hasInput: Bool {
        inputLock.lock(); defer { inputLock.unlock() }
        return !inputBytes.isEmpty
    }

    fileprivate func takeInput(into buf: UnsafeMutablePointer<CChar>, max: Int) -> Int {
        inputLock.lock(); defer { inputLock.unlock() }
        let n = min(max, inputBytes.count)
        for i in 0..<n { buf[i] = CChar(bitPattern: inputBytes[i]) }
        inputBytes.removeFirst(n)
        return n
    }

    /// زر ريموت أو لوحة مفاتيح (رمز مفتاح لينكس)
    func key(_ code: Int32, down: Bool) { gtv_post_key(code, down ? 1 : 0) }

    func tap(_ code: Int32) { key(code, down: true); key(code, down: false) }

    /// لمس بإحداثيات شاشة الضيف (1280×720)؛ phase: 0 بدء، 1 تحريك، 2 رفع
    func touch(x: Int32, y: Int32, phase: Int32) { gtv_post_touch(x, y, phase) }
}

/// رموز مفاتيح لينكس المستخدمة في الريموت
enum TVKey {
    static let home: Int32 = 172, back: Int32 = 158, menu: Int32 = 139
    static let up: Int32 = 103, down: Int32 = 108, left: Int32 = 105, right: Int32 = 106
    static let ok: Int32 = 353, enter: Int32 = 28
    static let playPause: Int32 = 164, next: Int32 = 163, prev: Int32 = 165
    static let volUp: Int32 = 115, volDown: Int32 = 114, mute: Int32 = 113
    static let power: Int32 = 116, guide: Int32 = 362
}
