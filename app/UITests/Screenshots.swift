import ObjectiveC
import UIKit
import XCTest

private enum SnapshotBridge {
    private static var snapshotClass: AnyClass? {
        [
            "ShellBox_UITests.Snapshot",
            "ShellBoxUITests.Snapshot",
            "Snapshot",
        ].compactMap(NSClassFromString).first
    }

    static func setupSnapshot(_ app: XCUIApplication, waitForAnimations: Bool) {
        guard let snapshotClass,
              snapshotClass.responds(to: Selector(("setupSnapshot:waitForAnimations:"))) else {
            XCTFail("Snapshot helper is not linked into the UI test target")
            return
        }

        typealias SetupSnapshotIMP = @convention(c) (AnyClass, Selector, XCUIApplication, Bool) -> Void
        let imp = snapshotClass.method(for: Selector(("setupSnapshot:waitForAnimations:")))
        unsafeBitCast(imp, to: SetupSnapshotIMP.self)(
            snapshotClass,
            Selector(("setupSnapshot:waitForAnimations:")),
            app,
            waitForAnimations
        )
    }

    static func snapshot(_ name: String, timeWaitingForIdle: TimeInterval) {
        guard let snapshotClass,
              snapshotClass.responds(to: Selector(("snapshot:timeWaitingForIdle:"))) else {
            XCTFail("Snapshot helper is not linked into the UI test target")
            return
        }

        typealias SnapshotIMP = @convention(c) (AnyClass, Selector, NSString, TimeInterval) -> Void
        let imp = snapshotClass.method(for: Selector(("snapshot:timeWaitingForIdle:")))
        unsafeBitCast(imp, to: SnapshotIMP.self)(
            snapshotClass,
            Selector(("snapshot:timeWaitingForIdle:")),
            name as NSString,
            timeWaitingForIdle
        )
    }
}

final class Screenshots: XCTestCase {
    private var app: XCUIApplication!

    override func setUp() {
        continueAfterFailure = false
        let app = XCUIApplication()
        self.app = app
        SnapshotBridge.setupSnapshot(app, waitForAnimations: false)

        let hostnameOverride: String
        switch UIDevice.current.userInterfaceIdiom {
        case .pad:
            hostnameOverride = "iPad"
        case .phone:
            hostnameOverride = "iPhone"
        default:
            XCTFail("unknown UI idiom")
            hostnameOverride = "ShellBox"
        }

        app.launchArguments += ["-hostnameOverride", hostnameOverride]
        app.launch()
        XCTAssert(app.webViews.staticTexts.firstMatch.waitForExistence(timeout: 10))
        chooseTheme("Solarized")
    }

    private var terminalLines: XCUIElementQuery {
        app.webViews.staticTexts
    }

    private func terminalLines(containing text: String) -> XCUIElementQuery {
        terminalLines.matching(NSPredicate(format: "label CONTAINS %@", text))
    }

    private func waitForTerminalText(_ text: String, timeout: TimeInterval) {
        XCTAssert(terminalLines(containing: text).firstMatch.waitForExistence(timeout: timeout))
    }

    private func waitForPrompt(timeout: TimeInterval) {
        let terminalLines = terminalLines
        let predicate = NSPredicate { _, _ in
            var lastLine: XCUIElement?
            while lastLine?.exists != true {
                lastLine = terminalLines.allElementsBoundByIndex.last
            }
            guard let lastLineText = lastLine?.label else {
                XCTFail("missing terminal line text")
                return false
            }
            return lastLineText.hasSuffix(":~#")
        }
        wait(for: [expectation(for: predicate, evaluatedWith: nil)], timeout: timeout)
    }

    private func runCommand(_ command: String, timeout: TimeInterval) {
        app.typeText("\(command)\n")
        waitForPrompt(timeout: timeout)
    }

    private func chooseTheme(_ name: String) {
        app.buttons["Settings"].tap()
        app.tables.staticTexts["Appearance"].tap()
        app.tables.staticTexts["Theme"].tap()
        app.tables.staticTexts[name].tap()
        app.navigationBars["Themes"].buttons["Appearance"].tap()
        app.navigationBars["Appearance"].buttons["Settings"].tap()
        app.navigationBars["Settings"].buttons["Done"].tap()
    }

    private func snapshot(_ name: String, order: UInt) {
        SnapshotBridge.snapshot(String(format: "%02u%@", order, name), timeWaitingForIdle: 10)
    }

    func testSystemInfo() {
        runCommand("uname -a", timeout: 5)
        snapshot("systeminfo", order: 1)
    }

    func testLanguages() {
        runCommand("apt-get update && apt-get install -y build-essential python3", timeout: 120)
        runCommand("printf '#include <stdio.h>\\nint main() { printf(\"Hello, ShellBox!\\\\n\"); }' > hello.c", timeout: 5)
        runCommand("gcc hello.c && ./a.out", timeout: 5)
        runCommand("python3 -c 'print(\"Hello, ShellBox!\")'", timeout: 5)
        snapshot("languages", order: 2)
    }

    func testEditorsInTmux() {
        runCommand("apt-get update && apt-get install -y vim nano tmux", timeout: 120)
        runCommand("tmux new-session -d -s foo nano", timeout: 5)
        runCommand("tmux split-window -v vim", timeout: 5)
        runCommand("tmux select-layout even-vertical", timeout: 5)
        app.typeText("tmux attach -t foo\n")
        waitForTerminalText("GNU nano", timeout: 30)
        snapshot("editorsintmux", order: 3)
    }

    func testEmacs() {
        runCommand("apt-get update && apt-get install -y emacs", timeout: 120)
        app.typeText("emacs\n")
        waitForTerminalText("Welcome to GNU Emacs", timeout: 30)
        snapshot("emacs", order: 4)
    }
}
