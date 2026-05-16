import UIKit

private let controlKeys = "abcdefghijklmnopqrstuvwxyz@^26-=[]\\ "

@objc(TerminalView)
final class TerminalView: UIView, UIKeyInput {
    private let hostView: SwiftTermHostView
    private var allowsFirstResponder = false
    private var storedOverrideFontSize: CGFloat = 0
    private weak var storedInputAccessoryView: UIInputView?

    @objc var overrideFontSize: CGFloat {
        get {
            storedOverrideFontSize
        }
        set {
            storedOverrideFontSize = newValue
            updateStyle()
        }
    }

    @objc var effectiveFontSize: CGFloat {
        if overrideFontSize != 0 {
            return overrideFontSize
        }
        return CGFloat(truncating: UserPreferences.shared().fontSize)
    }

    @objc var overrideAppearance: Int = 0 {
        didSet {
            updateStyle()
        }
    }

    @objc var keyboardAppearance: UIKeyboardAppearance = .default {
        didSet {
            updateStyle()
        }
    }

    @objc var controlKeySelected = false

    @objc var terminal: Terminal? {
        didSet {
            oldValue?.renderer = nil
            hostView.terminal = terminal
            terminal?.renderer = hostView
            updateStyle()
        }
    }

    override var canBecomeFirstResponder: Bool {
        get {
            allowsFirstResponder
        }
        set {
            allowsFirstResponder = newValue
        }
    }

    override var inputAccessoryView: UIView? {
        get {
            storedInputAccessoryView
        }
        set {
            let accessoryView = newValue as? UIInputView
            guard storedInputAccessoryView !== accessoryView else {
                return
            }
            storedInputAccessoryView = accessoryView
            hostView.customInputAccessoryView = accessoryView
        }
    }

    override init(frame: CGRect) {
        hostView = SwiftTermHostView(frame: frame)
        super.init(frame: frame)
        setup()
    }

    required init?(coder: NSCoder) {
        hostView = SwiftTermHostView(frame: .zero)
        super.init(coder: coder)
        setup()
    }

    private func setup() {
        inputAssistantItem.leadingBarButtonGroups = []
        inputAssistantItem.trailingBarButtonGroups = []

        hostView.frame = bounds
        hostView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        addSubview(hostView)

        UserPreferences.shared().observe(
            ["colorScheme", "fontFamily", "fontSize", "theme", "cursorStyle", "blinkCursor", "optionMapping"],
            options: [],
            owner: self
        ) { [weak self] _ in
            DispatchQueue.main.async {
                self?.updateStyle()
            }
        }
    }

    private func updateStyle() {
        let preferences = UserPreferences.shared()
        if storedOverrideFontSize == CGFloat(truncating: preferences.fontSize) {
            storedOverrideFontSize = 0
        }

        let palette: AnyObject
        if overrideAppearance != 0 {
            palette = overrideAppearance == 1 ? preferences.theme.lightPalette : preferences.theme.darkPalette
        } else {
            palette = preferences.palette
        }

        hostView.applyStyle(
            fontFamily: preferences.fontFamily,
            fontSize: NSNumber(value: Double(effectiveFontSize)),
            foregroundColor: palette.value(forKey: "foregroundColor") as? String ?? "#fff",
            backgroundColor: palette.value(forKey: "backgroundColor") as? String ?? "#000",
            cursorColor: palette.value(forKey: "cursorColor") as? String,
            paletteOverrides: palette.value(forKey: "colorPaletteOverrides") as? [String],
            cursorStyle: preferences.cursorStyle.rawValue,
            blinkCursor: preferences.blinkCursor,
            optionAsMetaKey: preferences.optionMapping == OptionMapEsc,
            keyboardAppearance: keyboardAppearance
        )
    }

    override func becomeFirstResponder() -> Bool {
        reloadInputViews()
        return hostView.becomeTerminalFirstResponder()
    }

    override func resignFirstResponder() -> Bool {
        hostView.resignTerminalFirstResponder()
        return super.resignFirstResponder()
    }

    @objc func loseFocus(_ sender: Any?) {
        _ = resignFirstResponder()
    }

    func insertText(_ text: String) {
        if controlKeySelected {
            controlKeySelected = false
            guard text.count == 1, let character = text.utf8.first else {
                return
            }
            insertControlCharacter(character)
            return
        }

        hostView.sendText(text.replacingOccurrences(of: "\n", with: "\r"))
    }

    private func insertControlCharacter(_ input: UInt8) {
        guard controlKeys.utf8.contains(input) else {
            return
        }

        var character = input
        if character == UInt8(ascii: " ") {
            character = 0
        } else if character == UInt8(ascii: "2") {
            character = UInt8(ascii: "@")
        } else if character == UInt8(ascii: "6") {
            character = UInt8(ascii: "^")
        }
        if character != 0 {
            character = Character(UnicodeScalar(character)).uppercased().utf8.first! ^ 0x40
        }
        hostView.sendData(Data([character]))
    }

    func deleteBackward() {
        insertText("\u{7f}")
    }

    var hasText: Bool {
        true
    }

    override func paste(_ sender: Any?) {
        if let string = UIPasteboard.general.string {
            insertText(string)
        }
    }

    override func copy(_ sender: Any?) {
        hostView.copySelection()
    }

    @objc func clearScrollback(_ command: UIKeyCommand) {
        hostView.clearScrollback()
    }

    @objc func pressArrow(_ direction: Int) {
        hostView.sendArrow(direction)
    }
}
