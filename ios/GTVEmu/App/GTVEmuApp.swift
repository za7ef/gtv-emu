// نقطة دخول التطبيق: شاشة رئيسية واحدة، والإعدادات والأدوات المتقدمة من زر الترس، بواجهة عربية من اليمين إلى اليسار.
import SwiftUI

@main
struct GTVEmuApp: App {
    @StateObject private var updater = UpdateChecker.shared
    @Environment(\.scenePhase) private var scenePhase
    @AppStorage("jitGuideShown") private var jitGuideShown = false
    @State private var showGuide = false

    init() {
        _ = EmulatorEngine.shared
        // يجب أن يسبق التجهيزُ المبكر لـ JIT أيَّ عمل آخر، ما دام المصحّح مرفقًا
        JITManager.shared.onLaunch()
        UsageStats.start()
    }

    var body: some Scene {
        WindowGroup {
            NavigationStack { MachineView().toolbar(.hidden, for: .navigationBar) }
            .environment(\.layoutDirection, .rightToLeft)
            .preferredColorScheme(.dark)
            .task {
                if !jitGuideShown && !JITManager.shared.isReady { jitGuideShown = true; showGuide = true }
                await updater.check()
            }
            .fullScreenCover(isPresented: $showGuide) {
                JITFirstRunSheet()
                    .environment(\.layoutDirection, .rightToLeft)
                    .preferredColorScheme(.dark)
            }
            .onChange(of: scenePhase) { _, phase in
                if phase == .active { JITManager.shared.onBecameActive() }
            }
            .sheet(item: $updater.available) { release in
                UpdateSheet(release: release, onLater: { updater.later() }, onUpdate: { updater.dismiss() })
                    .environment(\.layoutDirection, .rightToLeft)
                    .preferredColorScheme(.dark)
            }
        }
    }
}
