// ريموت Google TV على الشاشة: أسهم واختيار ورجوع ورئيسية وتشغيل ومستوى الصوت.
// كل زر يرسل ضغطًا ثم إفلاتًا لرمز مفتاح لينكس عبر جهاز الإدخال virtio.
import SwiftUI

struct RemoteView: View {
    var compact = false
    private let engine = EmulatorEngine.shared

    var body: some View {
        VStack(spacing: compact ? 8 : 14) {
            HStack(spacing: 18) {
                key("power", TVKey.power)
                key("tv", TVKey.guide)
                key("line.3.horizontal", TVKey.menu)
            }
            dpad
            HStack(spacing: 18) {
                key("arrow.uturn.backward", TVKey.back)
                key("house.fill", TVKey.home)
                key("playpause.fill", TVKey.playPause)
            }
            HStack(spacing: 18) {
                key("speaker.minus.fill", TVKey.volDown)
                key("speaker.slash.fill", TVKey.mute)
                key("speaker.plus.fill", TVKey.volUp)
            }
        }
        .padding(compact ? 10 : 16)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 24))
        .environment(\.layoutDirection, .leftToRight) // الأسهم تتبع اتجاه الشاشة لا اتجاه اللغة
    }

    private var dpad: some View {
        let s: CGFloat = compact ? 44 : 54
        return ZStack {
            Circle().fill(Color.secondary.opacity(0.2)).frame(width: s * 3, height: s * 3)
            VStack(spacing: 0) {
                arrow("chevron.up", TVKey.up, s)
                HStack(spacing: 0) {
                    arrow("chevron.left", TVKey.left, s)
                    Button { tap(TVKey.ok) } label: {
                        Circle().fill(Color.accentColor).frame(width: s, height: s)
                            .overlay(Text("OK").font(.caption.bold()).foregroundColor(.white))
                    }
                    arrow("chevron.right", TVKey.right, s)
                }
                arrow("chevron.down", TVKey.down, s)
            }
        }
    }

    private func arrow(_ icon: String, _ code: Int32, _ s: CGFloat) -> some View {
        Button { tap(code) } label: {
            Image(systemName: icon).font(.title3.bold()).frame(width: s, height: s).contentShape(Rectangle())
        }
    }

    private func key(_ icon: String, _ code: Int32) -> some View {
        Button { tap(code) } label: {
            Image(systemName: icon).font(.title3).frame(width: 44, height: 44)
                .background(Circle().fill(Color.secondary.opacity(0.2)))
        }
    }

    private func tap(_ code: Int32) {
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
        engine.tap(code)
    }
}
