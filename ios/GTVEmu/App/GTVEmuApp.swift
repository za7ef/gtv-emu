// نقطة دخول التطبيق: أربعة تبويبات (الآلة، الطرفية، الصور، الإعدادات) بواجهة عربية من اليمين إلى اليسار.
import SwiftUI

@main
struct GTVEmuApp: App {
    @StateObject private var updater = UpdateChecker.shared

    init() { _ = EmulatorEngine.shared }

    var body: some Scene {
        WindowGroup {
            TabView {
                NavigationStack { MachineView().toolbar(.hidden, for: .navigationBar) }
                    .tabItem { Label("الآلة", systemImage: "tv") }
                NavigationStack { ConsoleView().navigationTitle("الطرفية").navigationBarTitleDisplayMode(.inline) }
                    .tabItem { Label("الطرفية", systemImage: "terminal") }
                NavigationStack { ImagesView() }
                    .tabItem { Label("الصور", systemImage: "externaldrive") }
                NavigationStack { SettingsView() }
                    .tabItem { Label("الإعدادات", systemImage: "gearshape") }
            }
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
