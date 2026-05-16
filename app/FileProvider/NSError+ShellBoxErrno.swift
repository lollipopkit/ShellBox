import FileProvider
import Foundation

private let shellBoxENOENT = -2

extension NSError {
    @objc(errorWithShellBoxErrno:itemIdentifier:)
    class func shellBoxError(withErrno err: Int, itemIdentifier identifier: NSFileProviderItemIdentifier) -> NSError {
        if err == shellBoxENOENT {
            return fileProviderErrorForNonExistentItem(withIdentifier: identifier) as NSError
        }

        return NSError(
            domain: "ShellBoxErrnoDomain",
            code: err,
            userInfo: [NSLocalizedDescriptionKey: "error code \(err)"]
        )
    }
}
