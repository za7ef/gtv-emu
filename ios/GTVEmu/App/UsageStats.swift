//
//  UsageStats.swift
//  GTV Emu
//  Anonymous usage statistics (TelemetryDeck ingest API v2)
//
//  Copyright © 2026 za7ef
//
//  Sends one small "session started" signal when the app becomes active
//  (at most once every 30 minutes). No personal data, no device model,
//  no content. The user identifier is a random install ID that is hashed
//  (SHA-256) before it leaves the device, and TelemetryDeck salts and hashes
//  it again on its server. Disabled entirely when the user turns the
//  setting off, or when no App ID is configured.
//

import Foundation
import CryptoKit
#if canImport(UIKit)
import UIKit
#endif

enum UsageStats {
    /// TelemetryDeck App ID. Replace the placeholder with the real ID from
    /// dashboard.telemetrydeck.com (Set Up App). While it is the placeholder,
    /// nothing is sent.
    static let appID = "TELEMETRYDECK_APP_ID_GTV_EMU"

    /// UserDefaults key for the on/off setting (default: on).
    static let enabledKey = "usageStats.enabled"

    private static let installKey = "usageStats.installID"
    private static let lastSentKey = "usageStats.lastSent"
    private static let endpoint = URL(string: "https://nom.telemetrydeck.com/v2/")!
    private static let minInterval: TimeInterval = 30 * 60
    private static var observer: NSObjectProtocol?

    static var isEnabled: Bool {
        get { UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true }
        set {
            UserDefaults.standard.set(newValue, forKey: enabledKey)
            if !newValue { UserDefaults.standard.removeObject(forKey: lastSentKey) }
        }
    }

    private static var isConfigured: Bool {
        !appID.isEmpty && !appID.hasPrefix("TELEMETRYDECK_APP_ID")
    }

    /// Call once at app launch. Sends a signal now and whenever the app
    /// returns to the foreground (throttled).
    static func start() {
        sessionStarted()
        #if canImport(UIKit)
        if observer == nil {
            observer = NotificationCenter.default.addObserver(
                forName: UIApplication.didBecomeActiveNotification,
                object: nil, queue: .main
            ) { _ in sessionStarted() }
        }
        #endif
    }

    static func sessionStarted() {
        guard isEnabled, isConfigured else { return }
        let defaults = UserDefaults.standard
        let now = Date()
        if let last = defaults.object(forKey: lastSentKey) as? Date,
           now.timeIntervalSince(last) < minInterval { return }
        defaults.set(now, forKey: lastSentKey)
        send(type: "App.sessionStarted")
    }

    private static func installID() -> String {
        let defaults = UserDefaults.standard
        if let id = defaults.string(forKey: installKey) { return id }
        let id = UUID().uuidString
        defaults.set(id, forKey: installKey)
        return id
    }

    private static func sha256(_ s: String) -> String {
        SHA256.hash(data: Data(s.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    private static func send(type: String) {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        let os = ProcessInfo.processInfo.operatingSystemVersion
        let osVersion = "\(os.majorVersion).\(os.minorVersion)"
        let region = Locale.current.region?.identifier ?? "?"
        let language = Locale.preferredLanguages.first.map { String($0.prefix(2)) } ?? "?"
        #if DEBUG
        let testMode = "true"
        #else
        let testMode = "false"
        #endif

        let payload: [String: String] = [
            "TelemetryDeck.AppInfo.version": version,
            "TelemetryDeck.AppInfo.buildNumber": build,
            "TelemetryDeck.AppInfo.versionAndBuildNumber": "\(version) (build \(build))",
            "TelemetryDeck.Device.platform": "iOS",
            "TelemetryDeck.Device.operatingSystem": "iOS",
            "TelemetryDeck.Device.systemMajorMinorVersion": osVersion,
            "TelemetryDeck.Device.systemMajorVersion": "\(os.majorVersion)",
            "TelemetryDeck.UserPreference.region": region,
            "TelemetryDeck.UserPreference.language": language,
            "appVersion": version,
            "region": region,
        ]
        let body: [[String: Any]] = [[
            "receivedAt": ISO8601DateFormatter().string(from: Date()),
            "appID": appID,
            "clientUser": sha256(installID()),
            "sessionID": UUID().uuidString,
            "type": type,
            "payload": payload,
            "isTestMode": testMode,
        ]]
        guard let data = try? JSONSerialization.data(withJSONObject: body) else { return }
        var request = URLRequest(url: endpoint, timeoutInterval: 15)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = data
        URLSession.shared.dataTask(with: request) { _, _, _ in }.resume()
    }
}
