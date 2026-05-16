import UIKit

extension UIApplication {
    @objc(openURL:)
    class func shellBoxOpenURL(_ url: String) {
        guard let url = URL(string: url) else {
            return
        }
        shared.open(url, options: [:], completionHandler: nil)
    }
}
