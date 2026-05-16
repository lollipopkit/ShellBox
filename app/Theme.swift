import UIKit

private let themeVersion = 1

@objc(Palette)
final class Palette: NSObject {
    @objc let foregroundColor: String
    @objc let backgroundColor: String
    @objc let cursorColor: String?
    @objc let colorPaletteOverrides: [String]?

    @objc(initWithForegroundColor:backgroundColor:cursorColor:colorPaletteOverrides:)
    init(foregroundColor: String, backgroundColor: String, cursorColor: String?, colorPaletteOverrides: [String]?) {
        self.foregroundColor = foregroundColor
        self.backgroundColor = backgroundColor
        self.cursorColor = cursorColor
        self.colorPaletteOverrides = colorPaletteOverrides
        super.init()
    }

    fileprivate convenience init?(serializedRepresentation: [String: Any]) {
        func validColor(_ value: Any?) -> String? {
            guard let color = value as? String, UIColor(shellBoxHexString: color) != nil else {
                return nil
            }
            return color
        }

        guard let foregroundColor = validColor(serializedRepresentation["foregroundColor"]),
              let backgroundColor = validColor(serializedRepresentation["backgroundColor"]) else {
            return nil
        }

        let cursorColorValue = serializedRepresentation["cursorColor"]
        let cursorColor = cursorColorValue == nil ? nil : validColor(cursorColorValue)
        guard cursorColorValue == nil || cursorColor != nil else {
            return nil
        }

        let overridesValue = serializedRepresentation["colorPaletteOverrides"]
        let overrides: [String]?
        if let paletteOverrides = overridesValue as? [String] {
            guard paletteOverrides.allSatisfy({ UIColor(shellBoxHexString: $0) != nil }) else {
                return nil
            }
            overrides = paletteOverrides
        } else if overridesValue == nil {
            overrides = nil
        } else {
            return nil
        }

        self.init(
            foregroundColor: foregroundColor,
            backgroundColor: backgroundColor,
            cursorColor: cursorColor,
            colorPaletteOverrides: overrides
        )
    }

    fileprivate var serializedRepresentation: [String: Any] {
        var representation: [String: Any] = [
            "foregroundColor": foregroundColor,
            "backgroundColor": backgroundColor,
        ]
        if let cursorColor {
            representation["cursorColor"] = cursorColor
        }
        if let colorPaletteOverrides {
            representation["colorPaletteOverrides"] = colorPaletteOverrides
        }
        return representation
    }
}

@objc(ThemeAppearance)
final class ThemeAppearance: NSObject {
    @objc let lightOverride: Bool
    @objc let darkOverride: Bool

    @objc(initWithLightOverride:darkOverride:)
    init(lightOverride: Bool, darkOverride: Bool) {
        self.lightOverride = lightOverride
        self.darkOverride = darkOverride
        super.init()
    }

    fileprivate convenience init?(serializedRepresentation: [String: Any]) {
        guard let lightOverride = serializedRepresentation["lightOverride"] as? Bool,
              let darkOverride = serializedRepresentation["darkOverride"] as? Bool else {
            return nil
        }
        self.init(lightOverride: lightOverride, darkOverride: darkOverride)
    }

    @objc class var alwaysLight: ThemeAppearance {
        ThemeAppearance(lightOverride: false, darkOverride: true)
    }

    @objc class var alwaysDark: ThemeAppearance {
        ThemeAppearance(lightOverride: true, darkOverride: false)
    }

    fileprivate var serializedRepresentation: [String: Any] {
        [
            "lightOverride": lightOverride,
            "darkOverride": darkOverride,
        ]
    }
}

private final class DirectoryWatcher: NSObject, NSFilePresenter {
    let presentedItemURL: URL?
    private let handler: () -> Void

    init(url: URL, handler: @escaping () -> Void) {
        presentedItemURL = url
        self.handler = handler
        super.init()
    }

    var presentedItemOperationQueue: OperationQueue {
        .main
    }

    func presentedItemDidChange() {
        handler()
    }
}

@objc(Theme)
final class Theme: NSObject {
    private static var directoryWatcher: DirectoryWatcher?
    private static var didInstallSupport = false

    @objc static let themesUpdatedNotification = "ThemesUpdatedNotification"
    @objc static let themeUpdatedNotification = "ThemeUpdatedNotification"

    @objc let name: String
    @objc let lightPalette: Palette
    @objc let darkPalette: Palette
    @objc let appearance: ThemeAppearance?

    @objc(initWithName:palette:appearance:)
    convenience init(name: String, palette: Palette, appearance: ThemeAppearance?) {
        self.init(name: name, lightPalette: palette, darkPalette: palette, appearance: appearance)
    }

    @objc(initWithName:lightPalette:darkPalette:appearance:)
    init(name: String, lightPalette: Palette, darkPalette: Palette, appearance: ThemeAppearance?) {
        self.name = name
        self.lightPalette = lightPalette
        self.darkPalette = darkPalette
        self.appearance = appearance
        super.init()
        Self.ensureSupportInstalled()
    }

    @objc(initWithName:data:)
    convenience init?(name: String, data: Data) {
        guard let json = try? JSONSerialization.jsonObject(with: data),
              let representation = json as? [String: Any] else {
            return nil
        }

        guard let version = representation["version"] as? NSNumber,
              version.intValue > 0,
              version.intValue <= themeVersion else {
            NSLog("Rejecting theme %@ with invalid version number", name)
            return nil
        }

        let appearance: ThemeAppearance?
        if let appearanceRepresentation = representation["appearance"] as? [String: Any] {
            appearance = ThemeAppearance(serializedRepresentation: appearanceRepresentation)
        } else {
            appearance = nil
        }

        if let shared = representation["shared"] as? [String: Any] {
            guard let palette = Palette(serializedRepresentation: shared) else {
                return nil
            }
            self.init(name: name, palette: palette, appearance: appearance)
        } else if let light = representation["light"] as? [String: Any],
                  let dark = representation["dark"] as? [String: Any],
                  let lightPalette = Palette(serializedRepresentation: light),
                  let darkPalette = Palette(serializedRepresentation: dark) {
            self.init(name: name, lightPalette: lightPalette, darkPalette: darkPalette, appearance: appearance)
        } else {
            NSLog("Rejecting theme %@ with invalid palette(s)", name)
            return nil
        }
    }

    @objc class var defaultThemes: [Theme] {
        ensureSupportInstalled()
        return cachedDefaultThemes
    }

    private static let cachedDefaultThemes: [Theme] = [
        Theme(
            name: "Default",
            lightPalette: Palette(foregroundColor: "#000", backgroundColor: "#fff", cursorColor: nil, colorPaletteOverrides: nil),
            darkPalette: Palette(foregroundColor: "#fff", backgroundColor: "#000", cursorColor: nil, colorPaletteOverrides: nil),
            appearance: nil
        ),
        Theme(
            name: "1337",
            palette: Palette(foregroundColor: "#0f0", backgroundColor: "#000", cursorColor: nil, colorPaletteOverrides: nil),
            appearance: ThemeAppearance.alwaysDark
        ),
        Theme(
            name: "Solarized",
            lightPalette: Palette(
                foregroundColor: "#657b83",
                backgroundColor: "#fdf6e3",
                cursorColor: nil,
                colorPaletteOverrides: solarizedPalette
            ),
            darkPalette: Palette(
                foregroundColor: "#839496",
                backgroundColor: "#002b36",
                cursorColor: nil,
                colorPaletteOverrides: solarizedPalette
            ),
            appearance: nil
        ),
        Theme(
            name: "Hot Dog Stand",
            palette: Palette(foregroundColor: "#ff0", backgroundColor: "#f00", cursorColor: nil, colorPaletteOverrides: nil),
            appearance: nil
        ),
    ]

    private static let solarizedPalette = [
        "#073642",
        "#dc322f",
        "#859900",
        "#b58900",
        "#268bd2",
        "#d33682",
        "#2aa198",
        "#eee8d5",
        "#002b36",
        "#cb4b16",
        "#586e75",
        "#657b83",
        "#839496",
        "#6c71c4",
        "#93a1a1",
        "#fdf6e3",
    ]

    @objc class var userThemes: [Theme] {
        ensureSupportInstalled()
        let files = (try? FileManager.default.contentsOfDirectory(at: themesDirectory, includingPropertiesForKeys: nil)) ?? []
        return files.compactMap { file in
            guard let data = try? Data(contentsOf: file) else {
                return nil
            }
            return Theme(name: file.deletingPathExtension().lastPathComponent, data: data)
        }.sorted {
            $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
    }

    private class var themesDirectory: URL {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        return documents.appendingPathComponent("themes")
    }

    private var data: Data {
        var representation: [String: Any] = [
            "version": themeVersion,
        ]
        if lightPalette === darkPalette {
            representation["shared"] = lightPalette.serializedRepresentation
        } else {
            representation["light"] = lightPalette.serializedRepresentation
            representation["dark"] = darkPalette.serializedRepresentation
        }
        if let appearance {
            representation["appearance"] = appearance.serializedRepresentation
        }
        return (try? JSONSerialization.data(withJSONObject: representation, options: [.prettyPrinted, .sortedKeys])) ?? Data()
    }

    @objc(themeForName:includingDefaultThemes:)
    class func theme(forName name: String, includingDefaultThemes: Bool) -> Theme? {
        var themes = userThemes
        if includingDefaultThemes {
            themes.append(contentsOf: defaultThemes)
        }
        return themes.first { $0.name == name }
    }

    @objc func duplicateAsUserTheme() {
        var suffix = 1
        while true {
            let candidateName = "\(name)-\(suffix)"
            if Self.theme(forName: candidateName, includingDefaultThemes: false) == nil {
                writeTheme(named: candidateName)
                return
            }
            suffix += 1
        }
    }

    @objc func addUserTheme() -> Bool {
        guard Self.theme(forName: name, includingDefaultThemes: false) == nil else {
            return false
        }
        writeTheme(named: name)
        return true
    }

    @objc func deleteUserTheme() {
        try? FileManager.default.removeItem(at: Self.themesDirectory.appendingPathComponent("\(name).json"))
    }

    @objc(replaceWithUserTheme:)
    func replace(withUserTheme theme: Theme) {
        try? theme.data.write(to: Self.themesDirectory.appendingPathComponent("\(theme.name).json"), options: .atomic)
        if name != theme.name {
            deleteUserTheme()
            NotificationCenter.default.post(name: Notification.Name(Theme.themeUpdatedNotification), object: theme.name)
        }
    }

    private func writeTheme(named name: String) {
        try? data.write(to: Self.themesDirectory.appendingPathComponent("\(name).json"), options: .atomic)
    }

    private class func ensureSupportInstalled() {
        guard !didInstallSupport else {
            return
        }
        didInstallSupport = true
        let watcher = DirectoryWatcher(url: themesDirectory) {
            NotificationCenter.default.post(name: Notification.Name(themesUpdatedNotification), object: nil)
        }
        directoryWatcher = watcher
        NSFileCoordinator.addFilePresenter(watcher)
        try? FileManager.default.createDirectory(at: themesDirectory, withIntermediateDirectories: true)
    }
}

private extension UIColor {
    convenience init?(shellBoxHexString string: String) {
        guard string.hasPrefix("#") else {
            return nil
        }
        let raw = String(string.dropFirst())
        guard let value = UInt32(raw, radix: 16) else {
            return nil
        }

        let red: UInt32
        let green: UInt32
        let blue: UInt32
        let alpha: UInt32
        switch raw.count {
        case 3:
            red = ((value & 0xf00) >> 8) * 0x11
            green = ((value & 0x0f0) >> 4) * 0x11
            blue = (value & 0x00f) * 0x11
            alpha = 0xff
        case 4:
            red = ((value & 0xf000) >> 12) * 0x11
            green = ((value & 0x0f00) >> 8) * 0x11
            blue = ((value & 0x00f0) >> 4) * 0x11
            alpha = (value & 0x000f) * 0x11
        case 6:
            red = (value & 0xff0000) >> 16
            green = (value & 0x00ff00) >> 8
            blue = value & 0x0000ff
            alpha = 0xff
        case 8:
            red = (value & 0xff000000) >> 24
            green = (value & 0x00ff0000) >> 16
            blue = (value & 0x0000ff00) >> 8
            alpha = value & 0x000000ff
        default:
            return nil
        }

        self.init(
            red: CGFloat(red) / 0xff,
            green: CGFloat(green) / 0xff,
            blue: CGFloat(blue) / 0xff,
            alpha: CGFloat(alpha) / 0xff
        )
    }
}
