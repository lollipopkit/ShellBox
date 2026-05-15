import SwiftUI
import UIKit
import FileProvider
import Combine
import UniformTypeIdentifiers

private let shellBoxAlertNotification = Notification.Name("ShellBoxAlertNotification")
private let terminalUUIDActivityKey = "TerminalUUID"
private let shellBoxENODEV: Int32 = -19
private let shellBoxECANCELED: Int32 = -125

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
    @State private var isShowingSettings = false
    @State private var isShowingRoots = false
    @State private var shellBoxAlert: ShellBoxAlert?

    var body: some View {
        VStack(spacing: 0) {
            header
            TerminalControllerView(controller: terminalViewController)
                .ignoresSafeArea(.keyboard, edges: .bottom)
        }
        .background(Color(.systemBackground))
        .sheet(isPresented: $isShowingSettings) {
            NavigationView {
                ShellBoxSettingsView(recoveryMode: false)
            }
        }
        .sheet(isPresented: $isShowingRoots) {
            NavigationView {
                ShellBoxRootsView()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: shellBoxAlertNotification)) { notification in
            shellBoxAlert = ShellBoxAlert(notification: notification)
        }
        .alert(item: $shellBoxAlert) { alert in
            if alert.kind == .installRootfs {
                return Alert(
                    title: Text(alert.title),
                    message: Text(alert.message),
                    primaryButton: .cancel(Text("Exit")),
                    secondaryButton: .default(Text("Install")) {
                        (UIApplication.shared.delegate as? AppDelegate)?.exitApp()
                    }
                )
            }

            return Alert(
                title: Text(alert.title),
                message: Text(alert.message),
                dismissButton: .default(Text("OK"))
            )
        }
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
                isShowingRoots = true
            } label: {
                Image(systemName: "externaldrive")
            }
            .accessibility(label: Text("Roots"))

            Button {
                isShowingSettings = true
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
}

@objc(SceneDelegate)
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private var terminalUUID: String?
    private var terminalViewController: TerminalViewController?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options connectionOptions: UIScene.ConnectionOptions) {
        if window == nil, let windowScene = scene as? UIWindowScene {
            window = UIWindow(windowScene: windowScene)
        }

        if UserDefaults.standard.bool(forKey: "recovery") {
            window?.rootViewController = ShellBoxSettingsHostingController.controller(recoveryMode: true)
            window?.makeKeyAndVisible()
            return
        }

        let terminalController = terminalViewController(from: window?.rootViewController) ?? TerminalViewController()
        terminalViewController = terminalController
        window?.rootViewController = ShellBoxRootHostingController.controller(with: terminalController)
        window?.makeKeyAndVisible()

        terminalController.sceneSession = session
        if let restoredUUID = session.stateRestorationActivity?.userInfo?[terminalUUIDActivityKey] as? String {
            terminalUUID = restoredUUID
            terminalController.reconnectSession(fromTerminalUUID: UUID(uuidString: restoredUUID))
        } else {
            terminalController.startNewSession()
        }
    }

    func stateRestorationActivity(for scene: UIScene) -> NSUserActivity? {
        let activity = NSUserActivity(activityType: "app.ish.scene")
        guard let terminalController = terminalViewController ?? terminalViewController(from: window?.rootViewController),
              let uuidString = terminalController.sessionTerminalUUID?.uuidString else {
            return activity
        }
        terminalUUID = uuidString
        activity.addUserInfoEntries(from: [terminalUUIDActivityKey: uuidString])
        return activity
    }

    func sceneDidBecomeActive(_ scene: UIScene) {
        currentTerminalViewController = terminalViewController ?? terminalViewController(from: window?.rootViewController)
    }

    func sceneWillResignActive(_ scene: UIScene) {
        let terminalController = terminalViewController ?? terminalViewController(from: window?.rootViewController)
        if currentTerminalViewController === terminalController {
            currentTerminalViewController = nil
        }
    }

    private func terminalViewController(from rootViewController: UIViewController?) -> TerminalViewController? {
        if let terminalController = rootViewController as? TerminalViewController {
            return terminalController
        }
        if let rootController = rootViewController as? ShellBoxRootHostingController {
            return rootController.terminalViewController
        }
        return rootViewController?.value(forKey: "terminalViewController") as? TerminalViewController
    }
}

@objc(ShellBoxDirectoryPicker)
final class ShellBoxDirectoryPicker: NSObject, UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate {
    private let condition = NSCondition()
    private var urls: [URL]?

    @objc(askForURL:)
    func askForURL(_ url: AutoreleasingUnsafeMutablePointer<NSURL?>) -> Int32 {
        guard let terminalViewController = currentTerminalViewController else {
            return shellBoxENODEV
        }

        DispatchQueue.main.async {
            let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder], asCopy: false)
            picker.delegate = self
            picker.allowsMultipleSelection = false
            picker.presentationController?.delegate = self
            terminalViewController.present(picker, animated: true)
        }

        condition.lock()
        while urls == nil {
            condition.wait()
        }
        let selectedURLs = urls ?? []
        urls = nil
        condition.unlock()

        guard let selectedURL = selectedURLs.first else {
            return shellBoxECANCELED
        }
        url.pointee = selectedURL as NSURL
        return 0
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        finish(with: [])
    }

    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
        finish(with: [])
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        finish(with: urls)
    }

    private func finish(with urls: [URL]) {
        condition.lock()
        self.urls = urls
        condition.signal()
        condition.unlock()
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

@objc(ShellBoxAccessoryBarFactory)
final class ShellBoxAccessoryBarFactory: NSObject {
    @objc(inputViewWithController:)
    static func inputView(controller: TerminalViewController) -> UIInputView {
        ShellBoxAccessoryInputView(controller: controller)
    }
}

private final class ShellBoxAccessoryInputView: UIInputView {
    private var hostingController: UIHostingController<ShellBoxAccessoryBar>?

    init(controller: TerminalViewController) {
        super.init(frame: CGRect(x: 0, y: 0, width: 500, height: 54), inputViewStyle: .keyboard)
        allowsSelfSizing = true

        let hostingController = UIHostingController(rootView: ShellBoxAccessoryBar(handle: TerminalControllerHandle(controller)))
        hostingController.view.backgroundColor = .clear
        hostingController.view.translatesAutoresizingMaskIntoConstraints = false
        addSubview(hostingController.view)
        NSLayoutConstraint.activate([
            hostingController.view.topAnchor.constraint(equalTo: safeAreaLayoutGuide.topAnchor),
            hostingController.view.bottomAnchor.constraint(equalTo: safeAreaLayoutGuide.bottomAnchor),
            hostingController.view.leadingAnchor.constraint(equalTo: safeAreaLayoutGuide.leadingAnchor),
            hostingController.view.trailingAnchor.constraint(equalTo: safeAreaLayoutGuide.trailingAnchor),
        ])
        self.hostingController = hostingController
    }

    @MainActor @objc required dynamic init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override var intrinsicContentSize: CGSize {
        CGSize(width: UIView.noIntrinsicMetric, height: UIDevice.current.userInterfaceIdiom == .phone ? 48 : 56)
    }
}

private final class TerminalControllerHandle {
    weak var controller: TerminalViewController?

    init(_ controller: TerminalViewController) {
        self.controller = controller
    }
}

private struct ShellBoxAccessoryBar: View {
    let handle: TerminalControllerHandle

    @State private var controlActive = false
    @State private var showUpdateBadge = false

    private var isPhone: Bool {
        UIDevice.current.userInterfaceIdiom == .phone
    }

    var body: some View {
        HStack(spacing: 6) {
            accessoryButton("arrow.right.to.line.alt", label: "Tab") {
                sendText("\t")
            }

            Button {
                handle.controller?.accessoryToggleControl()
                syncState()
            } label: {
                Image(systemName: "control")
            }
            .accessibilityLabel("Control")
            .buttonStyle(AccessoryKeyButtonStyle(isActive: controlActive))

            accessoryButton("escape", label: "Escape") {
                sendText("\u{1B}")
            }

            accessoryButton("arrow.up", label: "Arrow Up") {
                sendArrow(1)
            }

            accessoryButton("arrow.down", label: "Arrow Down") {
                sendArrow(2)
            }

            accessoryButton("arrow.left", label: "Arrow Left") {
                sendArrow(3)
            }

            accessoryButton("arrow.right", label: "Arrow Right") {
                sendArrow(4)
            }

            Spacer(minLength: 4)

            Button {
                handle.controller?.accessoryShowSettings()
            } label: {
                Image(systemName: "gearshape")
                    .overlay(alignment: .topTrailing) {
                        if showUpdateBadge {
                            Circle()
                                .fill(Color.red)
                                .frame(width: 8, height: 8)
                                .offset(x: 3, y: -3)
                        }
                    }
            }
            .accessibilityLabel("Settings")
            .buttonStyle(AccessoryKeyButtonStyle())

            accessoryButton("doc.on.clipboard", label: "Paste") {
                handle.controller?.accessoryPaste()
                syncState()
            }

            if isPhone {
                accessoryButton("keyboard.chevron.compact.down", label: "Hide Keyboard") {
                    handle.controller?.accessoryHideKeyboard()
                }
            }
        }
        .padding(.horizontal, isPhone ? 6 : 12)
        .padding(.vertical, isPhone ? 6 : 8)
        .onAppear(perform: syncState)
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name(rawValue: "FsUpdatedNotification"))) { _ in
            syncState()
        }
    }

    private func accessoryButton(_ systemName: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: systemName)
        }
        .accessibilityLabel(label)
        .buttonStyle(AccessoryKeyButtonStyle())
    }

    private func sendText(_ text: String) {
        handle.controller?.accessoryInsertText(text)
        syncState()
    }

    private func sendArrow(_ direction: Int) {
        handle.controller?.accessoryPressArrow(direction)
        syncState()
    }

    private func syncState() {
        controlActive = handle.controller?.accessoryControlActive() ?? false
        showUpdateBadge = handle.controller?.accessoryShouldShowUpdateBadge() ?? false
    }
}

private struct AccessoryKeyButtonStyle: ButtonStyle {
    var isActive = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .medium))
            .frame(width: 34, height: 34)
            .foregroundColor(isActive ? .white : .primary)
            .background(backgroundColor(isPressed: configuration.isPressed))
            .clipShape(RoundedRectangle(cornerRadius: 5, style: .continuous))
            .shadow(color: Color.black.opacity(0.22), radius: 0, x: 0, y: 1)
    }

    private func backgroundColor(isPressed: Bool) -> Color {
        if isActive {
            return Color.accentColor
        }
        return isPressed ? Color(.tertiarySystemFill) : Color(.secondarySystemBackground)
    }
}

@objc(ShellBoxSettingsHostingController)
final class ShellBoxSettingsHostingController: UIHostingController<ShellBoxSettingsView> {
    @objc(controllerWithRecoveryMode:)
    static func controller(recoveryMode: Bool) -> UIViewController {
        ShellBoxSettingsHostingController(recoveryMode: recoveryMode)
    }

    init(recoveryMode: Bool) {
        super.init(rootView: ShellBoxSettingsView(recoveryMode: recoveryMode))
    }

    @MainActor @objc required dynamic init?(coder aDecoder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }
}

struct ShellBoxSettingsView: View {
    let recoveryMode: Bool

    @Environment(\.dismiss) private var dismiss
    @State private var launchCommand = ""
    @State private var bootCommand = ""
    @State private var disableDimming = false
    @State private var showResetConfirmation = false
    @State private var alert: SettingsAlert?

    private var versionText: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown"
        return "Shell Box \(version) (Build \(build))"
    }

    var body: some View {
        Form {
            Section {
                Toggle("Disable Dimming", isOn: $disableDimming)
                    .onChange(of: disableDimming) { value in
                        UserPreferences.shared().shouldDisableDimming = value
                    }

                TextField("Launch Command", text: $launchCommand)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .onSubmit(saveLaunchCommand)

                TextField("Boot Command", text: $bootCommand)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .onSubmit(saveBootCommand)
            }

            Section("Filesystem") {
                Button("Export Container") {
                    exportContainer()
                }

                Button("Reset Mounts") {
                    iosfs_clear_all_bookmarks()
                }

                Button("Reset Rootfs", role: .destructive) {
                    showResetConfirmation = true
                }

                if FsIsManaged() {
                    Label(filesystemStatusText, systemImage: FsNeedsRepositoryUpdate() ? "arrow.triangle.2.circlepath" : "checkmark.circle")
                        .foregroundStyle(FsNeedsRepositoryUpdate() ? .primary : .secondary)
                } else {
                    Text("The current filesystem is not managed by Shell Box.")
                        .foregroundStyle(.secondary)
                }
            }

            Section("Links") {
                Link("Send Feedback", destination: URL(string: "mailto:tblodt@icloud.com?subject=Feedback%20for%20Shell%20Box")!)
                Link("GitHub", destination: URL(string: "https://github.com/ish-app/ish")!)
                Link("Fediverse", destination: URL(string: "https://publ.ish.app/ish")!)
                Link("Discord", destination: URL(string: "https://discord.gg/HFAXj44")!)
            }

            Section {
                Text(versionText)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle(recoveryMode ? "Recovery Mode" : "Settings")
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button(recoveryMode ? "Exit" : "Done") {
                    if recoveryMode {
                        UserDefaults.standard.set(false, forKey: "recovery")
                        exit(0)
                    } else {
                        saveLaunchCommand()
                        saveBootCommand()
                        dismiss()
                    }
                }
            }
        }
        .onAppear(perform: loadPreferences)
        .confirmationDialog("Reset Rootfs", isPresented: $showResetConfirmation, titleVisibility: .visible) {
            Button("Reset", role: .destructive, action: resetRootfs)
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This will delete the current filesystem and re-import from the built-in rootfs. All data in the current filesystem will be lost.")
        }
        .alert(item: $alert) { item in
            Alert(title: Text(item.title), message: Text(item.message), dismissButton: .default(Text("OK")))
        }
    }

    private var filesystemStatusText: String {
        if FsNeedsRepositoryUpdate() {
            return "An upgrade is available."
        }
        return "The current filesystem is using the latest built-in repository configuration."
    }

    private func loadPreferences() {
        let preferences = UserPreferences.shared()
        launchCommand = preferences.launchCommand.joined(separator: " ")
        bootCommand = preferences.bootCommand.joined(separator: " ")
        disableDimming = preferences.shouldDisableDimming
    }

    private func saveLaunchCommand() {
        UserPreferences.shared().launchCommand = splitCommand(launchCommand)
    }

    private func saveBootCommand() {
        UserPreferences.shared().bootCommand = splitCommand(bootCommand)
    }

    private func splitCommand(_ command: String) -> [String] {
        command.split(separator: " ").map(String.init)
    }

    private func exportContainer() {
        let fileManager = FileManager.default
        guard let documents = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first else {
            alert = SettingsAlert(title: "Export Failed", message: "Documents directory is unavailable.")
            return
        }

        let destination = documents.appendingPathComponent("roots copy")
        try? fileManager.removeItem(at: destination)

        do {
            try fileManager.copyItem(at: ContainerURL().appendingPathComponent("roots"), to: destination)
        } catch {
            alert = SettingsAlert(title: "Export Failed", message: error.localizedDescription)
        }
    }

    private func resetRootfs() {
        let roots = Roots.instance()
        let defaultRoot = roots.defaultRoot
        let rootURL = roots.rootUrl(defaultRoot)
        try? FileManager.default.removeItem(at: rootURL)
        roots.mutableOrderedSetValue(forKey: "roots").remove(defaultRoot)

        guard let archive = Bundle.main.url(forResource: "root", withExtension: "tar.gz") else {
            alert = SettingsAlert(title: "Reset Failed", message: "Built-in rootfs archive is missing.")
            return
        }

        var error: NSError?
        if !roots.importRoot(fromArchive: archive, name: defaultRoot, error: &error, progressReporter: nil) {
            alert = SettingsAlert(title: "Reset Failed", message: error?.localizedDescription ?? "Unknown error")
            return
        }
        roots.defaultRoot = defaultRoot
        exit(0)
    }
}

struct ShellBoxRootsView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var roots: [String] = []
    @State private var defaultRoot = ""
    @State private var selectedRoot: SelectedRoot?
    @State private var alert: SettingsAlert?

    var body: some View {
        List {
            ForEach(roots, id: \.self) { root in
                Button {
                    selectedRoot = SelectedRoot(name: root)
                } label: {
                    HStack {
                        Text(root)
                        Spacer()
                        if root == defaultRoot {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundStyle(.tint)
                        }
                    }
                }
                .buttonStyle(.plain)
            }
        }
        .navigationTitle("Roots")
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Done") {
                    dismiss()
                }
            }
        }
        .onAppear(perform: reload)
        .sheet(item: $selectedRoot) { root in
            NavigationView {
                ShellBoxRootDetailView(rootName: root.name) {
                    reload()
                }
            }
        }
        .alert(item: $alert) { item in
            Alert(title: Text(item.title), message: Text(item.message), dismissButton: .default(Text("OK")))
        }
    }

    private func reload() {
        let manager = Roots.instance()
        roots = manager.roots.array.compactMap { $0 as? String }
        defaultRoot = manager.defaultRoot
    }
}

struct ShellBoxRootDetailView: View {
    @Environment(\.dismiss) private var dismiss
    @State var rootName: String
    let onChange: () -> Void

    @State private var editedName = ""
    @State private var alert: SettingsAlert?
    @State private var showDeleteConfirmation = false

    private var isDefaultRoot: Bool {
        rootName == Roots.instance().defaultRoot
    }

    var body: some View {
        Form {
            Section {
                TextField("Name", text: $editedName)
                    .disabled(isDefaultRoot)
                    .onSubmit(renameRoot)

                Button("Browse Files", action: browseFiles)
                Button("Boot This Filesystem", action: bootThis)
            }

            Section {
                Button("Delete Filesystem", role: .destructive) {
                    showDeleteConfirmation = true
                }
                .disabled(isDefaultRoot)
            } footer: {
                if isDefaultRoot {
                    Text("This filesystem can't be deleted because it's currently mounted as the root.")
                }
            }
        }
        .navigationTitle(rootName)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Done") {
                    renameRoot()
                    dismiss()
                }
            }
        }
        .onAppear {
            editedName = rootName
        }
        .confirmationDialog("Really delete?", isPresented: $showDeleteConfirmation, titleVisibility: .visible) {
            Button("Delete", role: .destructive, action: deleteFilesystem)
            Button("Cancel", role: .cancel) {}
        }
        .alert(item: $alert) { item in
            Alert(title: Text(item.title), message: Text(item.message), dismissButton: .default(Text("OK")))
        }
    }

    private func renameRoot() {
        guard !isDefaultRoot, editedName != rootName else {
            return
        }

        do {
            try Roots.instance().renameRoot(rootName, toName: editedName)
            rootName = editedName
            onChange()
        } catch {
            editedName = rootName
            alert = SettingsAlert(title: "Rename Failed", message: error.localizedDescription)
        }
    }

    private func browseFiles() {
        let storageURL = NSFileProviderManager.default.documentStorageURL
        let url = storageURL.appendingPathComponent(rootName)
        var components = URLComponents(url: url, resolvingAgainstBaseURL: false)
        components?.scheme = "shareddocuments"
        if let url = components?.url {
            UIApplication.shared.open(url)
        }
    }

    private func bootThis() {
        Roots.instance().defaultRoot = rootName
        exit(0)
    }

    private func deleteFilesystem() {
        guard !isDefaultRoot else {
            return
        }
        do {
            try Roots.instance().destroyRootNamed(rootName)
            onChange()
            dismiss()
        } catch {
            alert = SettingsAlert(title: "Delete Failed", message: error.localizedDescription)
        }
    }
}

private struct SettingsAlert: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

private struct SelectedRoot: Identifiable {
    let id: String
    let name: String

    init(name: String) {
        self.id = name
        self.name = name
    }
}

private struct ShellBoxAlert: Identifiable {
    enum Kind: String {
        case message
        case installRootfs
    }

    let id = UUID()
    let title: String
    let message: String
    let kind: Kind

    init(notification: Notification) {
        let userInfo = notification.userInfo ?? [:]
        title = userInfo["title"] as? String ?? "Shell Box"
        message = userInfo["message"] as? String ?? ""
        kind = Kind(rawValue: userInfo["kind"] as? String ?? "") ?? .message
    }
}
