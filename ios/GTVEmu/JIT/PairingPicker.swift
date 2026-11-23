// منتقي ملف الاقتران مبني على UIDocumentPickerViewController مباشرة بدل fileImporter في SwiftUI.
// السبب: بعض أدوات التثبيت الجانبي تحقن مكتبة (مثل FilePickerFix.dylib) تعدّل المنتقي، فظهر ملف
// ‎rp_pairing_file.plist باهتًا لا يمكن اختياره. هنا تُذكر الأنواع صراحة، ويُفتح المنتقي بنسخ الملف
// (asCopy) دون الحاجة إلى صلاحية الوصول في مكانه، ويُتحقق من المحتوى بعد الاختيار لا من النوع.
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct PairingPicker: UIViewControllerRepresentable {
    var onPick: (URL) -> Void
    var onCancel: () -> Void = {}

    static var types: [UTType] {
        var t: [UTType] = [.propertyList, .xmlPropertyList, .binaryPropertyList, .xml, .data, .item, .content, .text]
        for ext in ["plist", "mobiledevicepairing"] {
            if let u = UTType(filenameExtension: ext) { t.append(u) }
        }
        if let u = UTType("com.apple.mobiledevicepairing") { t.append(u) }
        var seen = Set<String>()
        return t.filter { seen.insert($0.identifier).inserted }
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let vc = UIDocumentPickerViewController(forOpeningContentTypes: Self.types, asCopy: true)
        vc.allowsMultipleSelection = false
        vc.shouldShowFileExtensions = true
        vc.delegate = context.coordinator
        return vc
    }

    func updateUIViewController(_ vc: UIDocumentPickerViewController, context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let parent: PairingPicker
        init(_ p: PairingPicker) { parent = p }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            if let u = urls.first { parent.onPick(u) } else { parent.onCancel() }
        }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { parent.onCancel() }
    }
}
