import class SwiftTerm.Color
import class SwiftTerm.TerminalView
import protocol SwiftTerm.TerminalViewDelegate
import UIKit

@objc(SwiftTermHostView)
final class SwiftTermHostView: UIView, TerminalViewDelegate, TerminalRenderer {
    @objc weak var terminal: Terminal? {
        didSet {
            reportSize()
        }
    }

    private let terminalView: TerminalView

    @objc override init(frame: CGRect) {
        terminalView = TerminalView(frame: frame, font: UIFont.monospacedSystemFont(ofSize: 12, weight: .regular))
        super.init(frame: frame)
        commonInit()
    }

    @objc required init?(coder: NSCoder) {
        terminalView = TerminalView(frame: .zero, font: UIFont.monospacedSystemFont(ofSize: 12, weight: .regular))
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        backgroundColor = .clear
        terminalView.translatesAutoresizingMaskIntoConstraints = false
        terminalView.terminalDelegate = self
        terminalView.allowMouseReporting = true
        terminalView.linkReporting = .implicit
        terminalView.linkHighlightMode = .hover
        addSubview(terminalView)
        NSLayoutConstraint.activate([
            terminalView.leadingAnchor.constraint(equalTo: leadingAnchor),
            terminalView.trailingAnchor.constraint(equalTo: trailingAnchor),
            terminalView.topAnchor.constraint(equalTo: topAnchor),
            terminalView.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        reportSize()
    }

    @objc var customInputAccessoryView: UIInputView? {
        didSet {
            terminalView.inputAccessoryView = customInputAccessoryView
            if terminalView.isFirstResponder {
                terminalView.reloadInputViews()
            }
        }
    }

    @objc func becomeTerminalFirstResponder() -> Bool {
        terminalView.becomeFirstResponder()
    }

    @objc func resignTerminalFirstResponder() {
        _ = terminalView.resignFirstResponder()
    }

    @objc func feedData(_ data: Data) {
        guard !data.isEmpty else { return }
        terminalView.feed(byteArray: [UInt8](data)[...])
    }

    @objc func terminalDidReceiveOutput(_ data: Data) {
        feedData(data)
    }

    @objc func scrollToBottom() {
        terminalView.scroll(toPosition: 1)
    }

    @objc func terminalScrollToBottom() {
        scrollToBottom()
    }

    @objc func copySelection() {
        terminalView.copy(nil)
    }

    @objc func clearScrollback() {
        terminalView.feed(text: "\u{1b}[3J")
    }

    @objc func sendText(_ text: String) {
        terminalView.send(txt: text)
    }

    @objc func sendData(_ data: Data) {
        terminalView.send(data: [UInt8](data)[...])
    }

    @objc func sendArrow(_ direction: Int) {
        let appCursor = terminalView.getTerminal().applicationCursor
        let suffix: UInt8
        switch direction {
        case 1: suffix = UInt8(ascii: "A")
        case 2: suffix = UInt8(ascii: "B")
        case 3: suffix = UInt8(ascii: "D")
        case 4: suffix = UInt8(ascii: "C")
        default: return
        }
        terminalView.send(data: [0x1b, appCursor ? UInt8(ascii: "O") : UInt8(ascii: "["), suffix][...])
    }

    @objc(applyFontFamily:fontSize:foregroundColor:backgroundColor:cursorColor:paletteOverrides:cursorStyle:blinkCursor:optionAsMetaKey:keyboardAppearance:)
    func applyStyle(fontFamily: String,
                    fontSize: NSNumber,
                    foregroundColor: String,
                    backgroundColor: String,
                    cursorColor: String?,
                    paletteOverrides: [String]?,
                    cursorStyle: Int,
                    blinkCursor: Bool,
                    optionAsMetaKey: Bool,
                    keyboardAppearance: UIKeyboardAppearance) {
        let size = CGFloat(truncating: fontSize)
        terminalView.font = UIFont(name: fontFamily, size: size) ?? UIFont.monospacedSystemFont(ofSize: size, weight: .regular)
        terminalView.nativeForegroundColor = UIColor(hexString: foregroundColor) ?? .white
        terminalView.nativeBackgroundColor = UIColor(hexString: backgroundColor) ?? .black
        terminalView.caretColor = cursorColor.flatMap { UIColor(hexString: $0) } ?? terminalView.nativeForegroundColor
        terminalView.optionAsMetaKey = optionAsMetaKey
        terminalView.keyboardAppearance = keyboardAppearance

        if let paletteOverrides, paletteOverrides.count >= 16 {
            let colors = paletteOverrides.prefix(16).compactMap { Color(hexString: $0) }
            if colors.count == 16 {
                terminalView.installColors(colors)
            }
        }

        switch (cursorStyle, blinkCursor) {
        case (0, true): terminalView.getTerminal().options.cursorStyle = .blinkBlock
        case (0, false): terminalView.getTerminal().options.cursorStyle = .steadyBlock
        case (1, true): terminalView.getTerminal().options.cursorStyle = .blinkBar
        case (1, false): terminalView.getTerminal().options.cursorStyle = .steadyBar
        case (2, true): terminalView.getTerminal().options.cursorStyle = .blinkUnderline
        case (2, false): terminalView.getTerminal().options.cursorStyle = .steadyUnderline
        default: terminalView.getTerminal().options.cursorStyle = .steadyBlock
        }

        setNeedsLayout()
        terminalView.setNeedsDisplay()
        reportSize()
    }

    private func reportSize() {
        guard bounds.width > 0, bounds.height > 0 else { return }
        let term = terminalView.getTerminal()
        terminal?.setWindowSizeWithColumns(CInt(term.cols), rows: CInt(term.rows))
    }

    func sizeChanged(source: TerminalView, newCols: Int, newRows: Int) {
        terminal?.setWindowSizeWithColumns(CInt(newCols), rows: CInt(newRows))
    }

    func setTerminalTitle(source: TerminalView, title: String) {}
    func hostCurrentDirectoryUpdate(source: TerminalView, directory: String?) {}

    func send(source: TerminalView, data: ArraySlice<UInt8>) {
        terminal?.sendInput(Data(data))
    }

    func scrolled(source: TerminalView, position: Double) {}

    func requestOpenLink(source: TerminalView, link: String, params: [String: String]) {
        guard let url = URL(string: link) else { return }
        UIApplication.shared.open(url)
    }

    func bell(source: TerminalView) {}

    func clipboardCopy(source: TerminalView, content: Data) {
        UIPasteboard.general.string = String(data: content, encoding: .utf8)
    }

    func iTermContent(source: TerminalView, content: ArraySlice<UInt8>) {}
    func rangeChanged(source: TerminalView, startY: Int, endY: Int) {}
}

private extension UIColor {
    convenience init?(hexString: String) {
        guard hexString.hasPrefix("#") else { return nil }
        let raw = String(hexString.dropFirst())
        guard let value = UInt32(raw, radix: 16) else { return nil }

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

        self.init(red: CGFloat(red) / 255.0,
                  green: CGFloat(green) / 255.0,
                  blue: CGFloat(blue) / 255.0,
                  alpha: CGFloat(alpha) / 255.0)
    }
}

private extension Color {
    convenience init?(hexString: String) {
        guard let color = UIColor(hexString: hexString) else { return nil }
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        guard color.getRed(&red, green: &green, blue: &blue, alpha: &alpha) else { return nil }
        self.init(red: UInt16(red * 65535), green: UInt16(green * 65535), blue: UInt16(blue * 65535))
    }
}
