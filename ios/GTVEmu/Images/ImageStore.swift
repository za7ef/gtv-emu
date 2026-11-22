// إدارة الصور: قراءة ملف manifest.json من إصدار على GitHub، وتنزيل ملفات كل ملف تعريف
// (النواة، init_boot، vendor_boot، صورة النظام...) مع التقدم، وفك ضغط gzip عند الحاجة،
// واستيراد الملفات يدويًا من تطبيق الملفات.
import Foundation
import Compression
import CryptoKit

// MARK: - صيغة الملف

struct ImageManifest: Codable {
    var version: Int
    var profiles: [ImageProfile]
}

struct ImageProfile: Codable, Identifiable, Hashable {
    var id: String
    var name: String
    var kind: String              // "linux" أو "android"
    var description: String?
    var cmdline: String?
    var androidboot: [String]?
    var ramMB: Int?               // الذاكرة المقترحة
    var cores: Int?
    var experimental: Bool?       // نظام تجريبي لا يكتمل إقلاعه بعد
    var note: String?             // سطر قصير يظهر تحت الاسم
    var files: [ImageFile]
    var downloadSize: Int64 { files.reduce(0) { $0 + ($1.size ?? 0) } }
}

struct ImageFile: Codable, Identifiable, Hashable {
    /// kernel | initrd | boot | vendor_boot | init_boot | disk
    var role: String
    var name: String              // اسم الملف المحلي بعد التنزيل
    var asset: String?            // اسم ملف الإصدار على GitHub (افتراضيًا name)
    var url: String?              // رابط مباشر بديل
    var size: Int64?              // الحجم بعد فك الضغط إن وُجد
    var sha256: String?           // بصمة الملف كما يُنزَّل
    var gunzip: Bool?             // فك ضغط gzip بعد التنزيل
    var optional: Bool?
    var id: String { role + "/" + name }
}

// MARK: - المخزن

// تُعدَّل الحالة المنشورة على الخيط الرئيسي فقط (الدوال المعلَّمة @MainActor).
final class ImageStore: NSObject, ObservableObject {
    static let shared = ImageStore()

    @Published private(set) var manifest: ImageManifest?
    @Published private(set) var progress: [String: Double] = [:]   // معرّف الملف ← نسبة التنزيل
    @Published private(set) var status: [String: String] = [:]     // معرّف الملف ← رسالة
    @Published var lastError: String?

    private var tasks: [Int: (profile: ImageProfile, file: ImageFile)] = [:]
    private var resumeData: [String: Data] = [:]   // استئناف التنزيل المنقطع لكل ملف

    /// هل يُنزَّل أي ملف من هذا النظام الآن؟
    func isDownloading(_ p: ImageProfile) -> Bool { p.files.contains { progress[$0.id] != nil } }
    /// نسبة تنزيل النظام كاملًا (الملفات الموجودة تُحسب مكتملة)
    func overallProgress(_ p: ImageProfile) -> Double {
        let total = Double(max(p.downloadSize, 1))
        let done = p.files.reduce(0.0) { acc, f in
            let sz = Double(f.size ?? 0)
            if isPresent(p, f) { return acc + sz }
            return acc + sz * (progress[f.id] ?? 0)
        }
        return min(1, done / total)
    }
    /// آخر رسالة خطأ لملفات النظام إن وُجدت
    func failure(_ p: ImageProfile) -> String? {
        p.files.compactMap { status[$0.id] }.first { $0.hasPrefix("فشل") }
    }
    private lazy var session: URLSession = {
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForResource = 6 * 3600
        cfg.waitsForConnectivity = true
        return URLSession(configuration: cfg, delegate: self, delegateQueue: nil)
    }()

    static var root: URL {
        let base = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let u = base.appendingPathComponent("images", isDirectory: true)
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    override init() {
        super.init()
        loadCachedManifest()
    }

    func dir(for p: ImageProfile) -> URL {
        let u = Self.root.appendingPathComponent(p.id, isDirectory: true)
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    func localURL(_ p: ImageProfile, _ f: ImageFile) -> URL { dir(for: p).appendingPathComponent(f.name) }

    func isPresent(_ p: ImageProfile, _ f: ImageFile) -> Bool { FileManager.default.fileExists(atPath: localURL(p, f).path) }

    func isReady(_ p: ImageProfile) -> Bool { p.files.allSatisfy { $0.optional == true || isPresent(p, $0) } }

    func url(_ p: ImageProfile, role: String) -> URL? {
        guard let f = p.files.first(where: { $0.role == role }), isPresent(p, f) else { return nil }
        return localURL(p, f)
    }

    func sizeOnDisk(_ p: ImageProfile) -> Int64 {
        p.files.reduce(0) { acc, f in
            let a = try? FileManager.default.attributesOfItem(atPath: localURL(p, f).path)
            return acc + ((a?[.size] as? NSNumber)?.int64Value ?? 0)
        }
    }

    // MARK: إعدادات المصدر

    private var settings: (repo: String, tag: String, token: String, manifestURL: String) {
        let d = UserDefaults.standard
        return (d.string(forKey: "repo") ?? "za7ef/gtv-emu",
                d.string(forKey: "releaseTag") ?? "images-v1",
                d.string(forKey: "githubToken") ?? "",
                d.string(forKey: "manifestURL") ?? "")
    }

    private func authorized(_ r: inout URLRequest) {
        let t = settings.token.trimmingCharacters(in: .whitespaces)
        if !t.isEmpty { r.setValue("Bearer \(t)", forHTTPHeaderField: "Authorization") }
        r.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
    }

    // MARK: قراءة القائمة

    private var cachedManifestURL: URL { Self.root.appendingPathComponent("manifest.json") }

    private func loadCachedManifest() {
        if let d = try? Data(contentsOf: cachedManifestURL), let m = try? JSONDecoder().decode(ImageManifest.self, from: d) {
            manifest = m
        } else if let u = Bundle.main.url(forResource: "default-manifest", withExtension: "json"),
                  let d = try? Data(contentsOf: u), let m = try? JSONDecoder().decode(ImageManifest.self, from: d) {
            manifest = m
        }
    }

    /// معلومات الإصدار من واجهة GitHub (تعمل مع المستودعات الخاصة عند وجود رمز وصول)
    private struct Release: Decodable { struct Asset: Decodable { let id: Int; let name: String; let url: String; let browser_download_url: String }; let tag_name: String; let assets: [Asset] }
    private var release: Release?

    @MainActor private func fetchRelease() async throws -> Release {
        if let r = release { return r }
        let s = settings
        let path = s.tag == "latest" ? "releases/latest" : "releases/tags/\(s.tag)"
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(s.repo)/\(path)")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        authorized(&req)
        let (d, resp) = try await session.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard code == 200 else {
            throw NSError(domain: "gtv", code: code, userInfo: [NSLocalizedDescriptionKey:
                code == 404 ? "لم يُعثر على الإصدار (المستودع خاص؟ أضف رمز وصول في الإعدادات)" : "خطأ من GitHub: \(code)"])
        }
        let r = try JSONDecoder().decode(Release.self, from: d)
        release = r
        return r
    }

    @MainActor func refreshManifest() async {
        lastError = nil
        release = nil
        do {
            let s = settings
            var req: URLRequest
            if !s.manifestURL.isEmpty, let u = URL(string: s.manifestURL) {
                req = URLRequest(url: u)
            } else {
                let r = try await fetchRelease()
                guard let a = r.assets.first(where: { $0.name == "manifest.json" }) else {
                    throw NSError(domain: "gtv", code: 1, userInfo: [NSLocalizedDescriptionKey: "الإصدار \(r.tag_name) لا يحتوي manifest.json"])
                }
                req = URLRequest(url: URL(string: a.url)!)
                req.setValue("application/octet-stream", forHTTPHeaderField: "Accept")
                authorized(&req)
            }
            let (d, _) = try await session.data(for: req)
            let m = try JSONDecoder().decode(ImageManifest.self, from: d)
            try d.write(to: cachedManifestURL)
            manifest = m
        } catch {
            lastError = error.localizedDescription
        }
    }

    // MARK: التنزيل

    func download(_ p: ImageProfile) {
        for f in p.files where !isPresent(p, f) { download(p, f) }
    }

    func download(_ p: ImageProfile, _ f: ImageFile) {
        Task { @MainActor in
            do {
                if let data = resumeData.removeValue(forKey: f.id) {
                    let t = session.downloadTask(withResumeData: data)
                    tasks[t.taskIdentifier] = (p, f)
                    progress[f.id] = progress[f.id] ?? 0
                    status[f.id] = "يستأنف التنزيل..."
                    t.resume()
                    return
                }
                var req: URLRequest
                if let s = f.url, let u = URL(string: s) {
                    req = URLRequest(url: u)
                } else {
                    let r = try await fetchRelease()
                    let name = f.asset ?? f.name
                    guard let a = r.assets.first(where: { $0.name == name }) else {
                        throw NSError(domain: "gtv", code: 2, userInfo: [NSLocalizedDescriptionKey: "الملف \(name) غير موجود في الإصدار \(r.tag_name)"])
                    }
                    req = URLRequest(url: URL(string: a.url)!)
                    req.setValue("application/octet-stream", forHTTPHeaderField: "Accept")
                    authorized(&req)
                }
                let t = session.downloadTask(with: req)
                tasks[t.taskIdentifier] = (p, f)
                progress[f.id] = 0
                status[f.id] = "يُنزَّل..."
                t.resume()
            } catch {
                status[f.id] = error.localizedDescription
            }
        }
    }

    /// إيقاف التنزيل مع حفظ ما نُزِّل لاستئنافه لاحقًا
    func cancelAll() {
        session.getAllTasks { all in
            for t in all {
                if let d = t as? URLSessionDownloadTask { d.cancel(byProducingResumeData: { _ in }) } else { t.cancel() }
            }
        }
    }

    func delete(_ p: ImageProfile) {
        try? FileManager.default.removeItem(at: dir(for: p))
        objectWillChange.send()
    }

    /// استيراد ملف من تطبيق الملفات بدل تنزيله
    func importFile(_ src: URL, into p: ImageProfile, _ f: ImageFile) {
        let scoped = src.startAccessingSecurityScopedResource()
        defer { if scoped { src.stopAccessingSecurityScopedResource() } }
        let dst = localURL(p, f)
        do {
            try? FileManager.default.removeItem(at: dst)
            if f.gunzip == true && src.pathExtension == "gz" {
                try Self.gunzipFile(src, to: dst)
            } else {
                try FileManager.default.copyItem(at: src, to: dst)
            }
            status[f.id] = "استُورد"
            objectWillChange.send()
        } catch {
            status[f.id] = "فشل الاستيراد: \(error.localizedDescription)"
        }
    }

    @MainActor fileprivate func finish(task: Int, location: URL?, error: Error?) {
        guard let entry = tasks.removeValue(forKey: task) else { return }
        let p = entry.profile, f = entry.file
        progress[f.id] = nil
        if let error = error {
            if let data = (error as NSError).userInfo[NSURLSessionDownloadTaskResumeData] as? Data { resumeData[f.id] = data }
            status[f.id] = "فشل: \(error.localizedDescription)"; return
        }
        guard let loc = location else { return }
        let dst = localURL(p, f)
        status[f.id] = f.gunzip == true ? "يتحقق ويفك الضغط..." : "يتحقق..."
        // التحقق وفك الضغط قد يستغرقان دقائق لصورة النظام، فيجريان خارج الخيط الرئيسي
        Task.detached(priority: .utility) {
            var msg = "جاهز"
            do {
                if let want = f.sha256?.lowercased(), !want.isEmpty {
                    let got = try ImageStore.sha256(of: loc)
                    guard got == want else { throw NSError(domain: "gtv", code: 3, userInfo: [NSLocalizedDescriptionKey: "البصمة لا تطابق"]) }
                }
                try? FileManager.default.removeItem(at: dst)
                if f.gunzip == true {
                    try ImageStore.gunzipFile(loc, to: dst)
                    try? FileManager.default.removeItem(at: loc)
                } else {
                    try FileManager.default.moveItem(at: loc, to: dst)
                }
            } catch {
                try? FileManager.default.removeItem(at: dst)
                msg = "فشل: \(error.localizedDescription)"
            }
            await MainActor.run {
                self.status[f.id] = msg
                self.objectWillChange.send()
            }
        }
    }

    // MARK: أدوات

    static func sha256(of url: URL) throws -> String {
        let h = try FileHandle(forReadingFrom: url)
        defer { try? h.close() }
        var hasher = SHA256()
        while let d = try h.read(upToCount: 4 << 20), !d.isEmpty { hasher.update(data: d) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// فك ضغط gzip تدفقيًا دون تحميل الملف كاملًا في الذاكرة
    static func gunzipFile(_ src: URL, to dst: URL) throws {
        let input = try FileHandle(forReadingFrom: src)
        defer { try? input.close() }
        FileManager.default.createFile(atPath: dst.path, contents: nil)
        let output = try FileHandle(forWritingTo: dst)
        defer { try? output.close() }

        // ترويسة gzip (RFC 1952)
        guard let hdr = try input.read(upToCount: 10), hdr.count == 10, hdr[0] == 0x1f, hdr[1] == 0x8b, hdr[2] == 8 else {
            throw NSError(domain: "gtv", code: 4, userInfo: [NSLocalizedDescriptionKey: "ليس ملف gzip"])
        }
        let flags = hdr[3]
        var skip: UInt64 = 10
        if flags & 4 != 0, let x = try input.read(upToCount: 2), x.count == 2 { skip += 2 + UInt64(x[0]) + UInt64(x[1]) << 8 }
        try input.seek(toOffset: skip)
        func skipCString() throws { while let b = try input.read(upToCount: 1), !b.isEmpty, b[0] != 0 {} }
        if flags & 8 != 0 { try skipCString() }
        if flags & 16 != 0 { try skipCString() }
        if flags & 2 != 0 { _ = try input.read(upToCount: 2) }

        // تدفق deflate خام عبر إطار Compression (COMPRESSION_ZLIB هو deflate الخام)
        let bufSize = 1 << 20
        let dstBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: bufSize)
        defer { dstBuf.deallocate() }
        let streamPtr = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer { streamPtr.deallocate() }
        guard compression_stream_init(streamPtr, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            throw NSError(domain: "gtv", code: 5, userInfo: [NSLocalizedDescriptionKey: "تعذّر تهيئة فك الضغط"])
        }
        defer { compression_stream_destroy(streamPtr) }
        var done = false
        while !done {
            let chunk = try input.read(upToCount: bufSize) ?? Data()
            let last = chunk.isEmpty
            try chunk.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                streamPtr.pointee.src_ptr = raw.bindMemory(to: UInt8.self).baseAddress ?? UnsafePointer(dstBuf)
                streamPtr.pointee.src_size = chunk.count
                repeat {
                    streamPtr.pointee.dst_ptr = dstBuf
                    streamPtr.pointee.dst_size = bufSize
                    let st = compression_stream_process(streamPtr, last ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0)
                    let produced = bufSize - streamPtr.pointee.dst_size
                    if produced > 0 { output.write(Data(bytes: dstBuf, count: produced)) }
                    if st == COMPRESSION_STATUS_END { done = true; return }
                    if st == COMPRESSION_STATUS_ERROR { throw NSError(domain: "gtv", code: 6, userInfo: [NSLocalizedDescriptionKey: "ملف gzip تالف"]) }
                } while streamPtr.pointee.src_size > 0 || streamPtr.pointee.dst_size == 0
            }
            if last { done = true }
        }
    }
}

extension ImageStore: URLSessionDownloadDelegate {
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                                totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        let id = downloadTask.taskIdentifier
        let frac = totalBytesExpectedToWrite > 0 ? Double(totalBytesWritten) / Double(totalBytesExpectedToWrite) : 0
        let mb = Double(totalBytesWritten) / 1_048_576
        Task { @MainActor in
            guard let f = self.tasks[id]?.file else { return }
            self.progress[f.id] = frac
            self.status[f.id] = String(format: "يُنزَّل %.1f ميغابايت", mb)
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // يجب نقل الملف قبل العودة من هذه الدالة، لأن النظام يحذفه بعدها
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try? FileManager.default.moveItem(at: location, to: tmp)
        let code = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 200
        let id = downloadTask.taskIdentifier
        Task { @MainActor in
            if code >= 400 {
                try? FileManager.default.removeItem(at: tmp)
                self.finish(task: id, location: nil, error: NSError(domain: "gtv", code: code, userInfo: [NSLocalizedDescriptionKey: "رمز HTTP \(code)"]))
            } else {
                self.finish(task: id, location: tmp, error: nil)
            }
        }
    }

    /// روابط ملفات الإصدار تحوّل إلى خادم تخزين آخر يرفض ترويسة Authorization، فنحذفها عند تغيّر المضيف
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        var r = request
        if r.url?.host != task.originalRequest?.url?.host { r.setValue(nil, forHTTPHeaderField: "Authorization") }
        completionHandler(r)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error = error else { return }
        let id = task.taskIdentifier
        Task { @MainActor in self.finish(task: id, location: nil, error: error) }
    }
}
