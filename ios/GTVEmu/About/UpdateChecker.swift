// التحقق من وجود إصدار أحدث في GitHub عند تشغيل التطبيق.
import Foundation
import SwiftUI

struct AppRelease: Identifiable, Equatable {
    let id: String          // وسم الإصدار، مثل v0.2.0-b12
    let version: String     // رقم الإصدار التسويقي
    let build: Int          // رقم البناء
    let notes: String
    let url: URL
}

@MainActor
final class UpdateChecker: ObservableObject {
    static let shared = UpdateChecker()
    static let repo = "za7ef/gtv-emu"

    @Published var available: AppRelease?

    static var currentVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
    }
    static var currentBuild: Int {
        Int(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "") ?? 0
    }

    private struct GHRelease: Decodable {
        let tag_name: String
        let body: String?
        let html_url: String
        let draft: Bool
        let prerelease: Bool
    }

    /// يحلّل وسمًا بصيغة v<الإصدار>-b<البناء>، ويُرجع nil لأي وسم آخر (مثل images-v1).
    nonisolated static func parse(tag: String) -> (version: String, build: Int)? {
        guard tag.hasPrefix("v"), let r = tag.range(of: "-b", options: .backwards) else { return nil }
        let version = String(tag[tag.index(after: tag.startIndex)..<r.lowerBound])
        guard !version.isEmpty, let build = Int(tag[r.upperBound...]) else { return nil }
        return (version, build)
    }

    func check() async {
        guard let url = URL(string: "https://api.github.com/repos/\(Self.repo)/releases?per_page=20") else { return }
        var req = URLRequest(url: url)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.timeoutInterval = 20
        guard let (data, resp) = try? await URLSession.shared.data(for: req),
              (resp as? HTTPURLResponse)?.statusCode == 200,
              let list = try? JSONDecoder().decode([GHRelease].self, from: data) else { return }

        let newest = list
            .filter { !$0.draft && !$0.prerelease }
            .compactMap { r -> AppRelease? in
                guard let p = Self.parse(tag: r.tag_name), let u = URL(string: r.html_url) else { return nil }
                return AppRelease(id: r.tag_name, version: p.version, build: p.build,
                                  notes: (r.body ?? "").trimmingCharacters(in: .whitespacesAndNewlines), url: u)
            }
            .max { $0.build < $1.build }

        guard let newest, newest.build > Self.currentBuild else { return }
        if UserDefaults.standard.string(forKey: "skippedUpdateTag") == newest.id { return }
        available = newest
    }

    func dismiss() { available = nil }

    func later() {
        if let a = available { UserDefaults.standard.set(a.id, forKey: "skippedUpdateTag") }
        available = nil
    }
}

struct UpdateSheet: View {
    let release: AppRelease
    let onLater: () -> Void
    let onUpdate: () -> Void
    @Environment(\.openURL) private var openURL

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text("تحديث جديد متوفر").font(.title2.bold())
                    Text("الإصدار \(release.version) (بناء \(release.build))")
                        .foregroundStyle(.secondary)
                    Text("ما الجديد").font(.headline)
                    Text(release.notes.isEmpty ? "لا توجد ملاحظات لهذا الإصدار." : release.notes)
                        .font(.body)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
            }
            .safeAreaInset(edge: .bottom) {
                HStack(spacing: 12) {
                    Button("لاحقًا") { onLater() }
                        .buttonStyle(.bordered)
                        .frame(maxWidth: .infinity)
                    Button("تحديث") {
                        openURL(release.url)
                        onUpdate()
                    }
                    .buttonStyle(.borderedProminent)
                    .frame(maxWidth: .infinity)
                }
                .padding()
            }
        }
        .presentationDetents([.medium, .large])
    }
}
