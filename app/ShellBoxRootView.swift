import SwiftUI
import UIKit

@objc(ShellBoxRootHostingController)
final class ShellBoxRootHostingController: UIHostingController<ShellBoxRootView> {
    @objc let terminalViewController: TerminalViewController

    @objc(controllerWithTerminalViewController:)
    static func controller(with terminalViewController: TerminalViewController) -> UIViewController {
        ShellBoxRootHostingController(terminalViewController: terminalViewController)
    }

    init(terminalViewController: TerminalViewController) {
        self.terminalViewController = terminalViewController
        super.init(rootView: ShellBoxRootView(terminalViewController: terminalViewController))
    }

    @MainActor @objc required dynamic init?(coder aDecoder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override var prefersStatusBarHidden: Bool {
        terminalViewController.prefersStatusBarHidden
    }

    override var preferredStatusBarStyle: UIStatusBarStyle {
        terminalViewController.preferredStatusBarStyle
    }
}

struct ShellBoxRootView: View {
    let terminalViewController: TerminalViewController

    var body: some View {
        VStack(spacing: 0) {
            header
            TerminalControllerView(controller: terminalViewController)
                .ignoresSafeArea(.keyboard, edges: .bottom)
        }
        .background(Color(.systemBackground))
    }

    private var header: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Shell Box")
                    .font(.headline)
                Text("Debian ARM64")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Spacer(minLength: 8)

            Button {
                presentRoots()
            } label: {
                Image(systemName: "externaldrive")
            }
            .accessibility(label: Text("Roots"))

            Button {
                presentSettings()
            } label: {
                Image(systemName: "gearshape")
            }
            .accessibility(label: Text("Settings"))
        }
        .buttonStyle(.borderless)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.regularMaterial)
    }

    private func presentSettings() {
        let storyboard = UIStoryboard(name: "About", bundle: nil)
        if let controller = storyboard.instantiateInitialViewController() {
            terminalViewController.present(controller, animated: true)
        }
    }

    private func presentRoots() {
        let storyboard = UIStoryboard(name: "Roots", bundle: nil)
        if let controller = storyboard.instantiateInitialViewController() {
            terminalViewController.present(controller, animated: true)
        }
    }
}

private struct TerminalControllerView: UIViewControllerRepresentable {
    let controller: TerminalViewController

    func makeUIViewController(context: Context) -> TerminalViewController {
        controller
    }

    func updateUIViewController(_ uiViewController: TerminalViewController, context: Context) {
    }
}
