# المكتبات المضمّنة

## StikJIT.xcframework

- المصدر: https://github.com/StikDebug/StikJIT، الإصدار 2.0.1 (الإيداع b3e01a30ded4cd0b27cf1b6bd42f123bbdf49c3a).
- الملف الأصلي: ‏StikJIT.xcframework.zip، وبصمته SHA-256:
  `c376687326a785f5ed1da8caca08adc709b56919019dd751bca059b92b175d95`
- الترخيص: MPL-2.0 (انظر LICENSE-StikJIT-MPL-2.0.txt). ويتضمن مكتبة idevice بترخيص MIT (انظر LICENSE-idevice-MIT.txt).
- التعديلات على النسخة الأصلية:
  1. حُذفت نسخة tvOS.
  2. حُذف الملفان arm64-apple-ios.private.swiftinterface وarm64-apple-ios.abi.json.
  3. عُدّل الملف arm64-apple-ios.swiftinterface بإزالة محددات الوحدات (`Swift::` وما يشبهها)، وإزالة خيارات المترجم الخاصة بالإصدار 6.4، ليقرأه مترجم Swift في Xcode 16 المستخدم في البناء. لم تتغير التصريحات ولا الملف الثنائي.
