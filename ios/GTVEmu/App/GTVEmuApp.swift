// نقطة دخول التطبيق: شاشة رئيسية واحدة، والإعدادات والأدوات المتقدمة من زر الترس، بواجهة عربية من اليمين إلى اليسار.
import SwiftUI

@main
struct GTVEmuApp: App {
    @StateObject private var updater = UpdateChecker.shared

    init() {
        _ = EmulatorEngine.shared
        UsageStats.start()
    }

    var body: some Scene {
        WindowGroup {
            NavigationStack { MachineView().toolbar(.hidden, for: .navigationBar) }
            .environment(\.layoutDirection, .rightToLeft)
            .preferredColorScheme(.dark)
            .task { await updater.check() }
            .sheet(item: $updater.available) { release in
                UpdateSheet(release: release, onLater: { updater.later() }, onUpdate: { updater.dismiss() })
                    .environment(\.layoutDirection, .rightToLeft)
                    .preferredColorScheme(.dark)
            }
        }
    }
}
