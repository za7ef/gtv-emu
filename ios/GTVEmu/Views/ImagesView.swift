// إدارة الصور: ملفات التعريف من manifest.json، وتنزيل ملفاتها أو استيرادها من تطبيق الملفات، وحذفها.
import SwiftUI
import UniformTypeIdentifiers

struct ImagesView: View {
    @ObservedObject private var store = ImageStore.shared
    @State private var importing: (ImageProfile, ImageFile)?
    @State private var showImporter = false
    @State private var refreshing = false

    var body: some View {
        List {
            if let err = store.lastError {
                Section { Label(err, systemImage: "exclamationmark.triangle").foregroundColor(.orange) }
            }
            if let m = store.manifest {
                ForEach(m.profiles) { p in profileSection(p) }
            } else {
                Section { Text("لا توجد قائمة صور بعد. اسحب للتحديث أو اضغط زر التحديث.") }
            }
        }
        .navigationTitle("الصور")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    Task { refreshing = true; await store.refreshManifest(); refreshing = false }
                } label: {
                    if refreshing { ProgressView() } else { Image(systemName: "arrow.clockwise") }
                }
            }
        }
        .refreshable { await store.refreshManifest() }
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.data, .item]) { result in
            if case .success(let url) = result, let pf = importing { store.importFile(url, into: pf.0, pf.1) }
            importing = nil
        }
    }

    @ViewBuilder
    private func profileSection(_ p: ImageProfile) -> some View {
        Section {
            if let d = p.description { Text(d).font(.footnote).foregroundColor(.secondary) }
            if let n = p.notice { Text(n).font(.caption2).foregroundColor(.secondary) }
            ForEach(p.files) { f in fileRow(p, f) }
            HStack {
                Button { store.download(p) } label: { Label("تنزيل الكل", systemImage: "arrow.down.circle") }
                    .disabled(store.isReady(p) || store.readyToPrepare(p))
                Spacer()
                Button(role: .destructive) { store.delete(p) } label: { Label("حذف", systemImage: "trash") }
            }
            .buttonStyle(.borderless)
        } header: {
            HStack {
                Text(p.name)
                Spacer()
                if store.isReady(p) { Label("جاهز", systemImage: "checkmark.circle.fill").foregroundColor(.green) }
            }
        } footer: {
            Text(ByteCountFormatter.string(fromByteCount: store.sizeOnDisk(p), countStyle: .file) + " على الجهاز")
        }
    }

    private func fileRow(_ p: ImageProfile, _ f: ImageFile) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Image(systemName: store.isPresent(p, f) ? "checkmark.circle.fill" : "circle")
                    .foregroundColor(store.isPresent(p, f) ? .green : .secondary)
                VStack(alignment: .leading) {
                    Text(roleTitle(f.role) + (f.optional == true ? " (اختياري)" : "")).font(.subheadline)
                    Text(f.name).font(.caption.monospaced()).foregroundColor(.secondary)
                }
                Spacer()
                if let s = f.size { Text(ByteCountFormatter.string(fromByteCount: s, countStyle: .file)).font(.caption) }
                if f.generated == true { Text("يُنشأ على الجهاز").font(.caption2).foregroundColor(.secondary) } else {
                Menu {
                    Button { store.download(p, f) } label: { Label("تنزيل", systemImage: "arrow.down") }
                    Button { importing = (p, f); showImporter = true } label: { Label("استيراد من الملفات", systemImage: "folder") }
                } label: { Image(systemName: "ellipsis.circle") }
                }
            }
            if let prog = store.progress[f.id] { ProgressView(value: prog) }
            if let st = store.status[f.id] { Text(st).font(.caption2).foregroundColor(.secondary) }
        }
    }

    private func roleTitle(_ r: String) -> String {
        switch r {
        case "kernel": return "النواة"
        case "initrd": return "القرص الأولي"
        case "boot": return "boot.img (نواة GKI)"
        case "vendor_boot": return "vendor_boot.img"
        case "init_boot": return "init_boot.img"
        case "disk": return "صورة النظام / القرص"
        case "source": return "ملف Google الأصلي (يُحذف بعد التجهيز)"
        case "prep_kernel": return "نواة آلة التجهيز"
        case "prep_initrd": return "أدوات التجهيز"
        default: return r
        }
    }
}
